#include <common/memory.h>
#include <common/platform/sunplus/canvas.h>
#include <common/platform/sunplus/Body.h>
#include <common/platform/sunplus/dma.h>
#include <common/platform/sunplus/farheap.h>
#include <common/platform/sunplus/draw.h>
#include <common/pad.h>
#include <common/util/misc.h>
#include <common/util/lprintf.h>

#include <common/animation.h>
#include <common/text.h>

#define MACROPAL_CACHE_SIZE 3

#define TYPE_SPRITE_FAMILY  1
#define TYPE_CEL 2
#define TYPE_ANIM 3
#define TYPE_ASSET_GROUP 4
#define TYPE_PALETTE 5


typedef struct TargetPaletteSlots {
	CanvasTargetPaletteRef palette;
	u16 *slots;
} TargetPaletteSlots;

typedef struct CanvasState {
	CanvasAssetGroupRef assetGroup;
	struct CanvasState *previous;
	u16 numAssets;
	CanvasRef *loadedAssets;
	void **loadedAddresses;
	u16 numPalettes;
	TargetPaletteSlots *palettes;
} CanvasState;

static CanvasState *sCurrentState = 0;
static CanvasState *sPreloadedState = 0;


static void loadMacropal(CanvasTargetPaletteRef pal, u16 *dst);
static u16 loadSprite(far_uintptr_t src, far_uintptr_t *dst, u16 flags, u16 offset);
static u16 loadCompressedSprite(far_uintptr_t src, far_uintptr_t *dst, u16 flags, u16 offset, u16 *macropal);
static TargetPaletteSlots* FindPaletteSlots(CanvasState *state, CanvasTargetPaletteRef pal);
static void* GetResource(CanvasState *state, CanvasRef ref);

CanvasRef CanvasFindObject(CanvasRef base, const char *name) {
	far_pointer_u ptr;
	START_FAR_ACCESS(base);
	START_FAR_STREAM(base);
	ptr.part.offset = FAR_STREAM_READ();
	ptr.part.segment = FAR_STREAM_READ();
	END_FAR_STREAM();
	START_FAR_STREAM(ptr.ptr);
	while(1) {
		int len = FAR_STREAM_READ();
		const char *n = name;
		if (len == 0) {
			ptr.ptr = 0;
			break;
		}
		while(len > 0) {
			char c = FAR_STREAM_READ();
			if (c != *(n++)) break;
			len--; // NOTE: len is left 1 more than it should be
		}
		if (len == 0) { // matched
			if (*n == 0) {
				ptr.part.offset = FAR_STREAM_READ();
				ptr.part.segment = FAR_STREAM_READ();
				break;
			} else {
				// not match, throw away ptr
				FAR_STREAM_READ();
				FAR_STREAM_READ();
			}
		} else {
			FAR_STREAM_SKIP(len+2-1); // -1 to compensate for len being 1 high when not matching
		}
	}
	END_FAR_STREAM();
	END_FAR_ACCESS();
	return ptr.ptr;
}



#undef CanvasFindPath
CanvasRef CanvasFindPathFar(CanvasRef base, DECLARE_FAR_POINTER(const char) fullPath)
{
	char buffer[256];

	CopyStringFromFarPointer(buffer, sizeof(buffer), fullPath);

	return CanvasFindPath(base, buffer);
}

CanvasRef CanvasFindPath(CanvasRef base, const char *fullPath) {
	CanvasRef at = base;
	const char *path = fullPath;
	char part[64];

	while(1) {
		int i = 0;
		while(path[i] != '/' && path[i] != 0) {
			part[i] = path[i];
			i++;
		}
		part[i] = 0;
		if (i != 0)
			at = CanvasFindObject(at, part);
		if (at == 0) {
			char basetext[40];
			lsnprintf(basetext, 40, "from 0x%08ld", base);
			die("Could not locate Canvas path:", fullPath, basetext);
		}

		if (path[i] == 0) return at;
		path = path+i+1;

	}
}

#undef CanvasFindPaths
CanvasRef CanvasFindPaths(CanvasRef ref, ...)
{
	const char *segment;
	va_list args;
	va_start(args, ref);

	while(segment = va_arg(args, const char *), segment)
	{
		ref = CanvasFindPath(ref, segment);
	}

	va_end(args);

	return ref;
}

CanvasRef CanvasFindPathf(CanvasRef base, const char *fmt, ...)
{
	char buffer[256];
	va_list args;

	va_start(args, fmt);
	lvsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	return CanvasFindPath(base, buffer);
}


void CanvasStatePush(CanvasAssetGroupRef ref) {
	CanvasStatePushPreload(ref);
	CanvasStateActivatePreloaded();
}

void CanvasStatePushPreload(CanvasAssetGroupRef ref) {
	CanvasState *newState;

	safe_assert(sPreloadedState == 0);

	MemoryPush();
	FarHeapPush();
	START_FAR_ACCESS(ref);
	START_FAR_STREAM(ref);
	FAR_STREAM_SKIP(2); // directory pointer
	{
		int type = FAR_STREAM_READ();
		safe_assert(type == TYPE_ASSET_GROUP);
	}

	newState = CREATE(CanvasState);

	FAR_STREAM_SKIP(512); // skip palette

	{
		CanvasState *at = sCurrentState;
		while(at != 0) {
			if (at->assetGroup != ref) {
				at = at->previous;
				continue;
			}
			*newState = *at;
			newState->previous = sCurrentState;
			goto RETURN;
		}
	}

	newState->assetGroup = ref;
	newState->previous = sCurrentState;

	{
		u16 numPalettes = FAR_STREAM_READ();
		u16 numSlots = FAR_STREAM_READ() + numPalettes; // accomodate null terminators
		TargetPaletteSlots *palettes = (TargetPaletteSlots*)MemoryAllocate(numPalettes*sizeof(TargetPaletteSlots));
		u16 *slotPositions = (u16*)MemoryAllocate(numSlots);
		u16 slotNum = 0;
		u16 i;

		newState->numPalettes = numPalettes;
		newState->palettes = palettes;

		for(i=0 ; i<numPalettes ; i++) {
			u16 offset = FAR_STREAM_READ();
			u16 segment = FAR_STREAM_READ();
			u16 palSlots = FAR_STREAM_READ();
			palettes[i].palette = FAR_POINTER_FROM_SEGMENT_AND_OFFSET(segment, offset);
			palettes[i].slots = slotPositions+slotNum;
			while(palSlots-- > 0)
				slotPositions[slotNum++] = FAR_STREAM_READ();
			slotPositions[slotNum++] = 0;
		}
		safe_assert(slotNum == numSlots);
	}

	{
		int numAssets = FAR_STREAM_READ(), i;

		newState->numAssets = numAssets;

		newState->loadedAssets = (CanvasRef*)MemoryAllocate(numAssets*sizeof(CanvasRef));
		newState->loadedAddresses = (void**)MemoryAllocate(numAssets*sizeof(void*));

		for(i=0 ; i<numAssets ; i++) {
			int offset = FAR_STREAM_READ();
			int segment = FAR_STREAM_READ();
			newState->loadedAssets[i] = FAR_POINTER_FROM_SEGMENT_AND_OFFSET(segment, offset);
		}
	}

	{
		int assetNum;

		u16 macropalCache[MACROPAL_CACHE_SIZE][1024];
		CanvasSpriteFamilyRef macropalTab[MACROPAL_CACHE_SIZE] = {0,0,0};
		u16 macropalCacheOrder[MACROPAL_CACHE_SIZE];

		{
			int i;
			for(i=0 ; i<MACROPAL_CACHE_SIZE ; i++)
				macropalCacheOrder[i] = i;
		}

		for(assetNum=0 ; assetNum<newState->numAssets ; assetNum++) {
			int type;
			FAR_STREAM_SEEK(newState->loadedAssets[assetNum]+2); // +2 to skip pointer
			type = FAR_STREAM_READ();
			switch(type) {
			case TYPE_SPRITE_FAMILY:
				{
					CanvasSpriteFamily *sf;
					int numParts = FAR_STREAM_READ(), part, totalSlots = 0;
					sf = (CanvasSpriteFamily*)MemoryAllocate(sizeof(CanvasSpriteFamily) + numParts*sizeof(CanvasSpriteFamilyPart));
					sf->numParts = numParts;
					for(part=0 ; part<numParts ; part++) {
						int offset = FAR_STREAM_READ();
						int segment = FAR_STREAM_READ();
						int i;
						CanvasTargetPaletteRef pal = FAR_POINTER_FROM_SEGMENT_AND_OFFSET(segment, offset);
						TargetPaletteSlots *slots = FindPaletteSlots(newState, pal);
						sf->parts[part].palette = slots;
						sf->parts[part].alpha = FAR_STREAM_READ();
						sf->parts[part].spriteOffset = totalSlots;
						for(i=0 ; slots->slots[i] ; i++)
							totalSlots++;
					}
					sf->totalSlots = totalSlots;
					newState->loadedAddresses[assetNum] = sf;
				}
				break;
			case TYPE_CEL:
				{
					int numLODs, lod;
					far_uintptr_t *loaded;
					CanvasSpriteFamily *sf;

					far_uintptr_t src;

					numLODs = FAR_STREAM_READ();

					FAR_STREAM_SKIP(numLODs*3); // don't care about LOD details right now

					{
						u16 offset = FAR_STREAM_READ();
						u16 segment = FAR_STREAM_READ();
						CanvasSpriteFamilyRef sfr = FAR_POINTER_FROM_SEGMENT_AND_OFFSET(segment, offset);
						sf = GetResource(newState, sfr);
					}

					{
						u16 offset = FAR_STREAM_READ();
						u16 segment = FAR_STREAM_READ();
						src = FAR_POINTER_FROM_SEGMENT_AND_OFFSET(segment, offset);
					}

					loaded = (far_uintptr_t*)MemoryAllocate(numLODs * sf->totalSlots * sizeof(far_uintptr_t));

					for(lod=0 ; lod < numLODs ; lod++) {
						{
							u16 offset;
							FAR_STREAM_SEEK(newState->loadedAssets[assetNum]+(2+1+1+3*lod+2));
							offset = FAR_STREAM_READ();
							FAR_STREAM_SEEK(newState->loadedAssets[assetNum]+offset);
						}
						{
							u16 part;
							for(part=0 ; part < sf->numParts ; part++) {
								u16 slot, sprite;
								far_uintptr_t slotdst[MAX_SLOTS];
								u16 numSprites = FAR_STREAM_READ();
								u16 dataSize = FAR_STREAM_READ();
								u16 celPartFlags = FAR_STREAM_READ();

								for(slot=0 ; sf->parts[part].palette->slots[slot] ; slot++) {
									slotdst[slot] = FarAllocateAligned(dataSize*32L, 8, 1);

									if(0 == (slotdst[slot] & 0xf))
									{
										FarAllocateAligned(8, 8, 1);
										slotdst[slot] += 8;
									}

									loaded[lod*sf->totalSlots + sf->parts[part].spriteOffset + slot] = slotdst[slot];
								}
								if (celPartFlags & 1) {
									u16 *macropal;
									CanvasTargetPaletteRef pal = sf->parts[part].palette->palette;
									int cacheSlot;
									for(cacheSlot=0 ; cacheSlot<MACROPAL_CACHE_SIZE ; cacheSlot++) {
										if (macropalTab[cacheSlot] == pal) break;
									}
									if (cacheSlot == MACROPAL_CACHE_SIZE) {
										cacheSlot = macropalCacheOrder[MACROPAL_CACHE_SIZE-1];
										loadMacropal(pal, macropalCache[cacheSlot]);
									}
									macropal = macropalCache[cacheSlot];
									if(macropalCacheOrder[0] != cacheSlot) {
										int i = MACROPAL_CACHE_SIZE-1;
										while (macropalCacheOrder[i] != cacheSlot)
											i--;
										while (i > 0) {
											macropalCacheOrder[i] = macropalCacheOrder[i-1];
											i--;
										}
										macropalCacheOrder[0] = cacheSlot;
									}
									for(sprite = 0 ; sprite < numSprites ; sprite++) {
										int flags;
										int sourceSize = 0;

										FAR_STREAM_READ(); FAR_STREAM_READ(); // x, y
										flags = FAR_STREAM_READ();
										for(slot=0 ; sf->parts[part].palette->slots[slot] ; slot++) {
											sourceSize = loadCompressedSprite(src, slotdst+slot, flags, (sf->parts[part].palette->slots[slot]&0x00FF)-1, macropal);
										}
										src += sourceSize;
									}
								} else {
									for(sprite = 0 ; sprite < numSprites ; sprite++) {
										int flags;
										int sourceSize = 0;

										FAR_STREAM_READ(); FAR_STREAM_READ(); // x, y
										flags = FAR_STREAM_READ();
										for(slot=0 ; sf->parts[part].palette->slots[slot] ; slot++) {
											sourceSize = loadSprite(src, slotdst+slot, flags, (sf->parts[part].palette->slots[slot]&0x00FF)-1);
										}
										src += sourceSize;
									}
								}
							}
						}
					}
					newState->loadedAddresses[assetNum] = loaded;
				}
				break;
			case TYPE_ANIM:
				// TODO
				break;
			default:
				safe_assert(!"unknown asset type in asset group");
				break;
			}
		}
	}


RETURN:
	sPreloadedState = newState;
	END_FAR_STREAM();
	END_FAR_ACCESS();
}

void CanvasStateClearPreloaded()
{
	sPreloadedState = 0;
}

void CanvasStateActivatePreloaded() {
	safe_assert(sPreloadedState != 0);
	safe_assert(sPreloadedState->previous == sCurrentState);

	START_FAR_ACCESS(sPreloadedState->assetGroup);
	START_FAR_STREAM(sPreloadedState->assetGroup);
	FAR_STREAM_SKIP(2); // directory pointer
	{
		int type = FAR_STREAM_READ();
		safe_assert(type == TYPE_ASSET_GROUP);
	}

	{
		// load palette
		u16 i;
		for(i=0 ; i<512 ; i++)
			SpritePalette.bank[0].palette[0].color[i] = FAR_STREAM_READ();
	}

	sCurrentState = sPreloadedState;
	sPreloadedState = 0;
	END_FAR_STREAM();
	END_FAR_ACCESS();
}

CanvasSpriteFamilyRef CanvasGetSpriteFamilyFromCel(CanvasCelRef cel) {
	CanvasSpriteFamilyRef sfr;

	START_FAR_ACCESS(cel);
	START_FAR_STREAM(cel);

	FAR_STREAM_SKIP(2+1);
	{
		u16 numLODs = FAR_STREAM_READ();
		FAR_STREAM_SKIP(numLODs*3);
	}
	{
		u16 offset = FAR_STREAM_READ();
		u16 segment = FAR_STREAM_READ();
		sfr = FAR_POINTER_FROM_SEGMENT_AND_OFFSET(segment, offset);
	}

	END_FAR_STREAM();
	END_FAR_ACCESS();

	return sfr;
}

void InitOilDMA(void);
void WaitOilDMA(void);
void StartOilDMA(far_uintptr_t dst, far_uintptr_t src, int count, int burstflags);
#define BURST_IN 0x2000
#define BURST_OUT 0x1000

static const u16 spriteSizeTable[] = {8,16,32,64};

u16 *CanvasGetPaletteSlot(CanvasTargetPaletteRef pal, int slot)
{
	return &SpritePalette.bank[0].palette[0].color[FindPaletteSlots(sCurrentState, pal)->slots[slot]];
}

s16 CanvasGetPaletteSize(CanvasTargetPaletteRef pal)
{
	s16 colors;

	START_FAR_ACCESS(pal);
	START_FAR_STREAM(pal);

	FAR_STREAM_READ();FAR_STREAM_READ();FAR_STREAM_READ(); // directory and type

	colors = FAR_STREAM_READ();
	// ... don't bother reading any more

	END_FAR_STREAM();
	END_FAR_ACCESS();

	return colors;
}

void CanvasSetPalette_(CanvasTargetPaletteRef pal, int slot, int swapid) {
	int colors, pals, i;
	u16 *dst;

	START_FAR_ACCESS(pal);
	START_FAR_STREAM(pal);

	FAR_STREAM_READ();FAR_STREAM_READ();FAR_STREAM_READ(); // directory and type

	colors = FAR_STREAM_READ();
	pals = FAR_STREAM_READ();
	FAR_STREAM_READ(); // pals

	for(i=0 ; i<pals ; i++) {
		int pal = FAR_STREAM_READ();
		if (pal == swapid) break;
	}
	if (i == pals) goto RETURN;

	FAR_STREAM_SKIP((pals-1-i) + colors*i); // skip to palette

	dst = CanvasGetPaletteSlot(pal, slot);

	for(i=0 ; i<colors ; i++)
		*dst++ = FAR_STREAM_READ();

RETURN:
	END_FAR_STREAM();
	END_FAR_ACCESS();
}

static void loadMacropal(CanvasTargetPaletteRef pal, u16 *dst) {
	u16 colors, palettes, micropals, i;

	START_FAR_ACCESS(pal);
	START_FAR_STREAM(pal);

	FAR_STREAM_READ();FAR_STREAM_READ();FAR_STREAM_READ(); // directory and type

	colors = FAR_STREAM_READ();
	palettes = FAR_STREAM_READ();
	micropals = FAR_STREAM_READ();
	FAR_STREAM_SKIP(palettes + colors*palettes); // skip tags & palettes

	for(i=0 ; i<micropals ; i++) {
		*dst++ = FAR_STREAM_READ();
		*dst++ = FAR_STREAM_READ();
		*dst++ = FAR_STREAM_READ();
		*dst++ = FAR_STREAM_READ();
	}

	END_FAR_STREAM();
	END_FAR_ACCESS();
}

static u16 loadCompressedSprite(far_uintptr_t src, far_uintptr_t *dst, u16 flags, u16 offset, u16 *macropal) {
	u16 width, height;
	u16 used = 0, y2, x2;
	u16 buffer1[64*4];
	u16 buffer2[64*4];
	u16 *work = buffer1;
	u16 wwidth;

	width = spriteSizeTable[(flags>>4)&3];
	height = spriteSizeTable[(flags>>6)&3];
	wwidth = width/2;

	InitOilDMA();

	START_FAR_ACCESS(src);
	START_FAR_STREAM(src);

	used = (height>>2)*(wwidth>>2);

	for(y2=0 ; y2<height ; y2+=4) {
		for(x2=0 ; x2<wwidth ; x2+=4) {
			u16 x3;
			u16 micropals = 0;
			for( x3 = 0 ; x3 < 4 ; x3+=2) {
				micropals <<= 8;
				if (!x3) micropals = FAR_STREAM_READ();
				if (!(micropals&0xFF00)) {
					u16 *rowptr = work+x2+x3;
					u16 y = 0;
					for(y=0 ; y<4 ; y++) {
						rowptr[0] = rowptr[1] = 0;
						rowptr += wwidth;
					}
				} else {
					u16 *micropal =((micropals&0xFF00)>>6)+macropal;
					u16 y, chunk = 0;
					used += 2;
					for(y=0 ; y<4 ; y++) {
						u16 x, word;
						if (!(y&1)) chunk = FAR_STREAM_READ();
						for(x=0 ; x<2 ; x++) {
							u16 pixel;
							asm("%1 = %2 ROL 2\n\t"
								"%0 = %0 ROL 4\n\t"
								"%0 &= 3\n\t" : "=r"(pixel), "=r"(chunk) : "r"(chunk));
							pixel = micropal[pixel];
							if (pixel) pixel += offset;
							word = pixel;
							asm("%1 = %2 ROL 2\n\t"
								"%0 = %0 ROL 4\n\t"
								"%0 &= 3\n\t" : "=r"(pixel), "=r"(chunk) : "r"(chunk));
							pixel = micropal[pixel];
							if (pixel) pixel += offset;
							word |= pixel<<8;
							work[y*wwidth+x2+x3+x] = word;
						}
					}
				}
			}
		}
		WaitOilDMA();
		StartOilDMA(*dst, (far_uintptr_t)(uintptr_t)work, wwidth*4, BURST_OUT);
		*dst += wwidth*4;
		(uintptr_t)work ^= (uintptr_t)buffer1^(uintptr_t)buffer2;
	}
	END_FAR_STREAM();
	END_FAR_ACCESS();

	WaitOilDMA();

	return used;
}

static u16 loadSprite(far_uintptr_t src, far_uintptr_t *dst, u16 flags, u16 offset) {
	u16 width, height;
	u16 used = 0, y2;
	u16 buffer1[64*4];
	u16 buffer2[64*4];
	u16 *work = buffer1;
	u16 wwidth;

	width = spriteSizeTable[(flags>>4)&3];
	height = spriteSizeTable[(flags>>6)&3];
	wwidth = width/2;

	InitOilDMA();

	START_FAR_ACCESS(src);
	START_FAR_STREAM(src);

	for(y2=0 ; y2<height ; y2+=4) {
		int y;
		for(y=0 ; y<4 ; y++) {
			int x;
			for(x=0 ; x<wwidth ; x++) {
				u16 px = FAR_STREAM_READ();
				if (px & 0x00FF) px += offset;
				if (px & 0xFF00) px += (offset<<8);
				work[y*wwidth+x] = px;
			}
			used += x;
		}
		WaitOilDMA();
		StartOilDMA(*dst, (far_uintptr_t)(uintptr_t)work, wwidth*4, BURST_OUT);
		*dst += wwidth*4;
		(uintptr_t)work ^= (uintptr_t)buffer1^(uintptr_t)buffer2;
	}
	END_FAR_STREAM();
	END_FAR_ACCESS();

	WaitOilDMA();

	return used;
}


static TargetPaletteSlots* FindPaletteSlots(CanvasState *state, CanvasTargetPaletteRef pal) {
	TargetPaletteSlots *pals = state->palettes;
	int num = state->numPalettes;
	while (num > 1) {
		int mid = num/2;
		if (pals[mid].palette > pal) {
			num = mid;
		} else {
			pals += mid;
			num -= mid;
		}
	}

	safe_assert(pals[0].palette == pal);
	return pals;
}

void CanvasStatePop() {
	safe_assert(sPreloadedState == 0);
	sCurrentState = sCurrentState->previous;
	FarHeapPop();
	MemoryPop();

	if (sCurrentState != 0) {
		START_FAR_ACCESS(sCurrentState->assetGroup);
		START_FAR_STREAM(sCurrentState->assetGroup);
		FAR_STREAM_SKIP(2); // directory pointer
		{
			int type = FAR_STREAM_READ();
			safe_assert(type == TYPE_ASSET_GROUP);
		}

		{
			// load palette
			u16 i;
			for(i=0 ; i<512 ; i++)
				SpritePalette.bank[0].palette[0].color[i] = FAR_STREAM_READ();
		}

		END_FAR_STREAM();
		END_FAR_ACCESS();
	}
}

static void DieNotLoaded(CanvasRef ref) {
	char path[200], *at = path;
	far_pointer_u ptr;

	ptr.ptr = CANVAS_ROOT;

	while(1) {
		far_pointer_u nextptr = {0};
		char *pathend = at;
		START_FAR_ACCESS(ptr.ptr);
		START_FAR_STREAM(ptr.ptr);
		ptr.part.offset = FAR_STREAM_READ();
		ptr.part.segment = FAR_STREAM_READ();
		END_FAR_STREAM();
		START_FAR_STREAM(ptr.ptr);
		while (1) {
			int len = FAR_STREAM_READ();
			char name[64], *nameptr = name;

			if (len == 0) {
				if (nextptr.ptr == 0) {
					// couldn't find
					ptr.ptr = 0;
					at = path;
					break;
				} else {
					// last entry was match
					at = pathend;
					break;
				}
			}
			while(len-- > 0) *nameptr++ = FAR_STREAM_READ();
			*nameptr++ = 0;
			ptr.part.offset = FAR_STREAM_READ();
			ptr.part.segment = FAR_STREAM_READ();
			if (ptr.ptr <= ref) {
				nextptr.ptr = ptr.ptr;
				nameptr = name;
				pathend = at;
				while(*nameptr) *pathend++ = *nameptr++;
				*pathend++ = '/';
			}
			if (ptr.ptr >= ref) {
				at = pathend;
				break;
			}
		}
		END_FAR_STREAM();
		END_FAR_ACCESS();
		if (nextptr.ptr == ref || nextptr.ptr == 0) break;
		ptr = nextptr;
	}
	if (!ptr.ptr) {// not found
		die("could not find Canvas asset:", lasprintf("0x%08lX", ref), "and could not find its path");
	} else {
		*at++ = 0;
		die("could not find Canvas asset:", path, "in loaded asset group");
	}
}

static void* GetResource(CanvasState *state, CanvasRef ref) {
	int num = state->numAssets, start = 0;
	while (num > 1) {
		int mid = num/2;
		if (state->loadedAssets[start+mid] > ref) {
			num = mid;
		} else {
			start += mid;
			num -= mid;
		}
	}
	if (state->loadedAssets[start] != ref) DieNotLoaded(ref);
	return state->loadedAddresses[start];
}

void* CanvasGetLoadedResource(CanvasRef ref) {
	return GetResource(sCurrentState, ref);
}

CanvasDrawInfo* CanvasAllocDraw(CanvasSpriteFamilyRef sfr) {
	CanvasSpriteFamily *sf = CanvasGetLoadedResource(sfr);
	CanvasDrawInfo *di = (CanvasDrawInfo*)MemoryAllocate(sizeof(CanvasDrawInfo)+sizeof(CanvasPartInfo)*sf->numParts);
	CanvasInitDraw(sfr, di);
	return di;
}

void CanvasInitDraw(CanvasSpriteFamilyRef sfr, CanvasDrawInfo *out) {
	int part;
	CanvasSpriteFamily *sf = CanvasGetLoadedResource(sfr);
	out->flagsraw = (DEPTH2<<12);
	out->mosaic = 0;
	out->depth = 0;
	out->scale = 1024;
	out->family = sf;
	CanvasDrawReset(out);
	for(part=0 ; part<sf->numParts ; part++) {
		out->parts[part].alpha = sf->parts[part].alpha;
		out->parts[part].slot = 0;
	}
}

void CanvasDrawReset(CanvasDrawInfo *out) {
	out->transform[0][0] = 1024;
	out->transform[0][1] = 0;
	out->transform[0][2] = 0;
	out->transform[1][0] = 0;
	out->transform[1][1] = 1024;
	out->transform[1][2] = 0;
	out->transform_flags = 0;
}

void CanvasDrawResetLores(CanvasDrawInfo *out) {
	out->transform[0][0] = 2048;
	out->transform[0][1] = 0;
	out->transform[0][2] = 0;
	out->transform[1][0] = 0;
	out->transform[1][1] = 2048;
	out->transform[1][2] = 0;
	out->transform_flags = DI_TRANFORM_UNIFORM_SCALE;
}

void CanvasDrawRotate(CanvasDrawInfo *out, u16 rot) {
	fixw s = fixw_sinfx12(rot);
	fixw c = fixw_cosfx12(rot);
	if(out->transform_flags & DI_TRANFORM_ROTATE)
	{
		s32 xx = out->transform[0][0];
		s32 xy = out->transform[0][1];
		s32 yx = out->transform[1][0];
		s32 yy = out->transform[1][1];
		out->transform[0][0] = fw_mul(c, xx)+fw_mul(s, xy);
		out->transform[0][1] = fw_mul(c, xy)-fw_mul(s, xx);
		out->transform[1][0] = fw_mul(c, yx)+fw_mul(s, yy);
		out->transform[1][1] = fw_mul(c, yy)-fw_mul(s, yx);
	}
	else if(out->transform_flags & DI_TRANFORM_SCALE)
	{
		s32 xx = out->transform[0][0];
		s32 yy = out->transform[1][1];
		out->transform[0][0] = +fw_mul(c, xx);
		out->transform[0][1] = -fw_mul(s, xx);
		out->transform[1][0] = +fw_mul(s, yy);
		out->transform[1][1] = +fw_mul(c, yy);
	}
	else
	{
		out->transform[0][0] = +c;
		out->transform[0][1] = -s;
		out->transform[1][0] = +s;
		out->transform[1][1] = +c;
	}
	out->transform_flags |= DI_TRANFORM_ROTATE;
}

void CanvasDrawScale(CanvasDrawInfo *out, fixw sx, fixw sy)
{
	if(out->transform_flags & DI_TRANFORM_ROTATE)
	{
		s32 xx = out->transform[0][0];
		s32 xy = out->transform[0][1];
		s32 yx = out->transform[1][0];
		s32 yy = out->transform[1][1];

		out->transform[0][0] = fw_mul(xx, sx);
		out->transform[0][1] = fw_mul(xy, sy);
		out->transform[1][0] = fw_mul(yx, sx);
		out->transform[1][1] = fw_mul(yy, sy);
	}
	else if(out->transform_flags & DI_TRANFORM_SCALE)
	{
		s32 xx = out->transform[0][0];
		s32 yy = out->transform[1][1];

		out->transform[0][0] = fw_mul(xx, sx);
		out->transform[1][1] = fw_mul(yy, sy);
	}
	else
	{
		out->transform[0][0] = sx;
		out->transform[1][1] = sy;
	}
	{
	  s16 scale = out->scale;
	  out->scale = fw_mul(scale, (sx+sy)/2);
	}
	out->transform_flags |= (sx == sy) ? DI_TRANFORM_UNIFORM_SCALE : DI_TRANFORM_SCALE;
}

void CanvasDrawTranslate(CanvasDrawInfo *out, s32 x, s32 y)
{
	if(out->transform_flags & DI_TRANFORM_ROTATE)
	{
		s32 xx = out->transform[0][0];
		s32 xy = out->transform[0][1];
		s32 yx = out->transform[1][0];
		s32 yy = out->transform[1][1];
		out->transform[0][2] += fw_mul(x, xx) + fw_mul(y, xy);
		out->transform[1][2] += fw_mul(x, yx) + fw_mul(y, yy);
	}
	else if(out->transform_flags & DI_TRANFORM_SCALE)
	{
		s32 xx = out->transform[0][0];
		s32 yy = out->transform[1][1];
		out->transform[0][2] += fw_mul(x, xx);
		out->transform[1][2] += fw_mul(y, yy);
	}
	else
	{
		out->transform[0][2] += x;
		out->transform[1][2] += y;
	}
}

void CanvasDrawTransform3D(CanvasDrawInfo *out, s32 x, s32 y, s32 z)
{
	fixw scale = (((u32)320<<10)/z);
	CanvasDrawTranslate(out, 320 + fw_mul(scale,x), 240 - fw_mul(scale, y));
	CanvasDrawScale(out, scale, scale);
	out->depth = z;
}

void CanvasDrawTransform3Dv(CanvasDrawInfo *out, s32 xyz[3])
{
	fixw scale = (((u32)320<<10)/xyz[2]);
	CanvasDrawTranslate(out, 320 + fw_mul(scale,xyz[0]), 240 - fw_mul(scale, xyz[1]));
	CanvasDrawScale(out, scale, scale);
	out->depth = xyz[2];
}

void CanvasDrawLocate3Dv(CanvasDrawInfo *out, s32 xyz[3])
{
	fixw scale = (((u32)320<<10)/xyz[2]);
	CanvasDrawTranslate(out, 320 + fw_mul(xyz[0], scale), 240 - fw_mul(xyz[1], scale));
	out->depth = xyz[2];
}

void CanvasTransformPoint(s16 pt[2], CanvasDrawInfo *di)
{
	s16 x = pt[0];
	s16 y = pt[1];

	if(di->transform_flags & DI_TRANFORM_ROTATE)
	{
		pt[0] = fw_mul(x, di->transform[0][0]) + fw_mul(y, di->transform[0][1]) + di->transform[0][2];
		pt[1] = fw_mul(x, di->transform[1][0]) + fw_mul(y, di->transform[1][1]) + di->transform[1][2];
	}
	else if(di->transform_flags & DI_TRANFORM_SCALE)
	{
		pt[0] = fw_mul(x, di->transform[0][0]) + di->transform[0][2];
		pt[1] = fw_mul(y, di->transform[1][1]) + di->transform[1][2];
	}
	else
	{
		pt[0] = x + di->transform[0][2];
		pt[1] = y + di->transform[1][2];
	}
}

static int TransformPiece(HWSprite3D *sprite, s16 x, s16 y, u16 flags, CanvasDrawInfo *di)
{
	s16 w = spriteSizeTable[(flags>>4)&3];
	s16 h = spriteSizeTable[(flags>>6)&3];

	if (di->flags.hflip)
		x = - x - w;

	if(GetDrawMode() & DRAW_MODE_HIRES){
		if(di->transform_flags < DI_TRANFORM_NONUNIFORM_SCALE) {
			if((sprite->att0raw&0x000C) && (di->transform[0][0] == 1024 || di->transform[0][0] == 2048)) w--, h--;
			else
			{
				if (di->transform[0][0] == 1024) {
					s16 xpos, ypos;
					w >>= 1;
					h >>= 1;
					xpos = di->transform[0][2]+x+w-320;
					ypos = 255-(di->transform[1][2]+y+h);
					if (xpos+w < -320 || xpos-w > 320 || ypos+h < -224 || ypos-h > 255) return 0;
					sprite->att0raw |= 0x000C;
					sprite->corners[0][0] = xpos;
					sprite->corners[0][1] = ypos;
					sprite->corners[1][0] = 0;
					sprite->corners[1][1] = 32;
					return 1;
				} else if (di->transform[0][0] == 2048) {
					s16 xpos = di->transform[0][2]+x*2+w-320;
					s16 ypos = 255-(di->transform[1][2]+y*2+h);
					if (xpos+w < -320 || xpos-w > 320 || ypos+h < -224 || ypos-h > 255) return 0;
					sprite->att0raw |= 0x000C;
					sprite->corners[0][0] = xpos;
					sprite->corners[0][1] = ypos;
					sprite->corners[1][0] = 0;
					sprite->corners[1][1] = 36;
					return 1;
				}
			}
		}
		if(di->transform_flags & DI_TRANFORM_ROTATE)
		{
			s16 xh = fw_mul(w, di->transform[0][1]);
			s16 yh = fw_mul(h, di->transform[1][1]);

			sprite->corners[0][0] = + di->transform[0][2] + fw_mul(x, di->transform[0][0]) + fw_mul(y, di->transform[0][1]) - 320;
			sprite->corners[0][1] = - di->transform[1][2] - fw_mul(x, di->transform[1][0]) - fw_mul(y, di->transform[1][1]) + 255;

			sprite->corners[1][0] = sprite->corners[0][0] + fw_mul(w, di->transform[0][0]);
			sprite->corners[1][1] = sprite->corners[0][1] - fw_mul(w, di->transform[1][0]);

			sprite->corners[2][0] = sprite->corners[1][0] + xh;
			sprite->corners[2][1] = sprite->corners[1][1] - yh;

			sprite->corners[3][0] = sprite->corners[0][0] + xh;
			sprite->corners[3][1] = sprite->corners[0][1] - yh;
		}
		else
		{
			s16 yh = fw_mul(h, di->transform[1][1]);

			sprite->corners[0][0] = + di->transform[0][2] + fw_mul(x, di->transform[0][0]) - 320;
			sprite->corners[0][1] = - di->transform[1][2] - fw_mul(y, di->transform[1][1]) + 255;

			sprite->corners[1][0] = sprite->corners[0][0] + fw_mul(w, di->transform[0][0]);
			sprite->corners[1][1] = sprite->corners[0][1];

			sprite->corners[2][0] = sprite->corners[1][0];
			sprite->corners[2][1] = sprite->corners[1][1] - yh;

			sprite->corners[3][0] = sprite->corners[0][0];
			sprite->corners[3][1] = sprite->corners[0][1] - yh;
		}

		if (sprite->corners[0][0] < -320 && sprite->corners[2][0] < -320) return 0;
		if (sprite->corners[0][0] > 320 && sprite->corners[2][0] > 320) return 0;
		if (sprite->corners[0][1] < -224 && sprite->corners[2][1] < -224) return 0;
		if (sprite->corners[0][1] > 255 && sprite->corners[2][1] > 255) return 0;
	}
	else
	{
		if (di->transform_flags < DI_TRANFORM_NONUNIFORM_SCALE)
		{
			if((sprite->att0raw & 0x000C) && (di->transform[0][0] == 1024 || di->transform[0][0] == 2048)) w--, h--;
			else
			{
				if (di->transform[0][0] == 1024)
				{
					s16 xpos, ypos;
					w >>= 1;
					h >>= 1;
					xpos = di->transform[0][2]+x+w-320;
					ypos = 255-(di->transform[1][2]+y+h);
					if (xpos+w < -320 || xpos-w > 320 || ypos+h < -224 || ypos-h > 255) return 0;
					xpos >>= 1;
					ypos >>= 1;
					sprite->att0raw |= 0x000C;
					sprite->corners[0][0] = xpos;
					sprite->corners[0][1] = ypos;
					sprite->corners[1][0] = 0;
					sprite->corners[1][1] = 16;
					return 1;
				}
				else if (di->transform[0][0] == 2048)
				{
					s16 xpos = di->transform[0][2]+x*2+w-320;
					s16 ypos = 255-(di->transform[1][2]+y*2+h);
					if (xpos+w < -320 || xpos-w > 320 || ypos+h < -224 || ypos-h > 255) return 0;
					xpos >>= 1;
					ypos >>= 1;
					sprite->att0raw |= 0x000C;
					sprite->corners[0][0] = xpos;
					sprite->corners[0][1] = ypos;
					sprite->corners[1][0] = 0;
					sprite->corners[1][1] = 32;
					return 1;
				}
			}
		}

		if(di->transform_flags & DI_TRANFORM_ROTATE)
		{
			s16 xh = fw_mul(w, di->transform[0][1])>>1;
			s16 yh = fw_mul(h, di->transform[1][1])>>1;

			sprite->corners[0][0] = + ((fw_mul(x, di->transform[0][0])+fw_mul(y, di->transform[0][1])+di->transform[0][2])>>1) - 160;
			sprite->corners[0][1] = - ((fw_mul(x, di->transform[1][0])+fw_mul(y, di->transform[1][1])+di->transform[1][2])>>1) + 127;

			sprite->corners[1][0] = sprite->corners[0][0] + ((fw_mul(w, di->transform[0][0]))>>1);
			sprite->corners[1][1] = sprite->corners[0][1] - ((fw_mul(w, di->transform[1][0]))>>1);

			sprite->corners[2][0] = sprite->corners[1][0] + xh;
			sprite->corners[2][1] = sprite->corners[1][1] - yh;

			sprite->corners[3][0] = sprite->corners[0][0] + xh;
			sprite->corners[3][1] = sprite->corners[0][1] - yh;
		}
		else
		{
			s16 yh = fw_mul(h, di->transform[1][1]) >> 1;

			sprite->corners[0][0] = + ((fw_mul(x, di->transform[0][0]) + di->transform[0][2]) >> 1) - 160;
			sprite->corners[0][1] = - ((fw_mul(y, di->transform[1][1]) + di->transform[1][2]) >> 1) + 127;

			sprite->corners[1][0] = sprite->corners[0][0] + ((fw_mul(w, di->transform[0][0]))>>1);

			sprite->corners[1][1] = sprite->corners[0][1];

			sprite->corners[2][0] = sprite->corners[1][0];
			sprite->corners[2][1] = sprite->corners[1][1] - yh;

			sprite->corners[3][0] = sprite->corners[0][0];
			sprite->corners[3][1] = sprite->corners[0][1] - yh;
		}

		if (sprite->corners[0][0] < -160 && sprite->corners[2][0] < -160) return 0;
		if (sprite->corners[0][0] > 160 && sprite->corners[2][0] > 160) return 0;
		if (sprite->corners[0][1] < -112 && sprite->corners[2][1] < -112) return 0;
		if (sprite->corners[0][1] > 128 && sprite->corners[2][1] > 128) return 0;
	}
	return 1;
}

static const u16 sizeTable[] = {
	1*4,2*4,4*4,8*4,
	1*8,2*8,4*8,8*8,
	1*16,2*16,4*16,8*16,
	1*32,2*32,4*32,8*32,
};

void CanvasDrawCel(CanvasCelRef cel, CanvasDrawInfo *di) {
	far_uintptr_t *sourceTable;
	int part;
	int lod = 0;
	fixw relscale;
	u16 offset;
	CanvasSpriteFamily *sf = di->family;
	if (cel == 0 || di->scale == 0) return;
	sourceTable = (far_uintptr_t*)CanvasGetLoadedResource(cel);
	START_FAR_ACCESS(cel);
	START_FAR_STREAM(cel);
	FAR_STREAM_SKIP(2+1+1);
	if (!(GetDrawMode() & DRAW_MODE_HIRES))
		di->scale >>= 1;
	while(FAR_STREAM_READ() > di->scale) {
		FAR_STREAM_READ(); // skip relative scale
		FAR_STREAM_READ(); // skip data offset
		lod++;
	}
	relscale = FAR_STREAM_READ();
	offset = FAR_STREAM_READ();

	CanvasDrawScale(di, relscale, relscale);

	FAR_STREAM_SEEK(cel+offset);

	for(part=0 ; part<di->family->numParts ; part++) {
		u16 numSprites, dataSize, att0, att1;
		unsigned long charnum = (
			sourceTable[lod*sf->totalSlots + sf->parts[part].spriteOffset + di->parts[part].slot]
			 - (0x8000ul - 0x8))>>3;

		numSprites = FAR_STREAM_READ();
		dataSize = FAR_STREAM_READ();
		FAR_STREAM_READ(); // compression flags

		if(di->parts[part].alpha == 0) {
			FAR_STREAM_SKIP(3*numSprites);
			charnum += (long)(dataSize << 2);
			continue;
		}

		att0 = di->flagsraw | 0x0003;
		att1 = 0;

		if (sf->parts[part].palette->slots[di->parts[part].slot] & 0x100)
			att0 |= 1<<15;

		if (di->parts[part].alpha != 64) {
			att0 |= 1<<14;
			att1 |= (di->parts[part].alpha<<8)&0x3F00;
		}
		if (di->mosaic != 0) {
			att1 |= (di->mosaic<<14)&0xC000;
		}

		while(numSprites-- > 0) {
			u16 x = FAR_STREAM_READ();
			u16 y = FAR_STREAM_READ();
			u16 flags = FAR_STREAM_READ();

			HWSprite3D *sprite = GetHWSprite3D();
			if(!sprite) goto RETURN; // out of sprites

			sprite->att0raw = att0;
			sprite->att1raw = att1;

			if (!TransformPiece(sprite, x, y, flags, di)) {
				ReleaseHWSprite();
				charnum += sizeTable[(flags>>4)&15];
				continue;
			}

			sprite->depth = -(s32)di->depth;
			sprite->charlow = charnum;
			//e safe_assert(sprite->charlow);
			sprite->att0raw |= flags;
			sprite->att1raw |= ((charnum>>16)&0x00FF);

			charnum += sizeTable[(flags>>4)&15];
		}
	}
RETURN:
	END_FAR_STREAM();
	END_FAR_ACCESS();
}

void CAnimation_Start(CAnimation *anim, CanvasAnimRef script) {
	anim->ref = script;
	anim->line = -1;
	anim->frame = 1;
	anim->cel = 0;
	CAnimation_Step(anim);
}

void CAnimation_GotoFrame(CAnimation *anim, int frame) {
	int line = 0, steps = 0;
	far_uintptr_t workaddr = anim->ref+2+1+1;

	START_FAR_ACCESS(workaddr);
	START_FAR_STREAM(workaddr);

	while(steps <= frame)
	{
		int cmd = FAR_STREAM_READ();
		if (cmd < 0) break;
		steps += cmd;
		line++;
		FAR_STREAM_READ();
		FAR_STREAM_READ();
	}

	END_FAR_STREAM();
	END_FAR_ACCESS();

	CAnimation_GotoLine(anim, line - 1);
	anim->frame = steps - frame;

	if(anim->frame < 1)
		anim->frame = 1;
}

void CAnimation_GotoLine(CAnimation *anim, int line) {
	anim->line = line-1;
	anim->frame = 1;
	CAnimation_Step(anim);
}

int CAnimation_Step(CAnimation *anim) {
	if (anim->frame > 0 && --anim->frame == 0) {
		u16 offset, segment;
		far_uintptr_t workaddr;
		while(anim->frame == 0){
			anim->line++;
			workaddr = anim->ref+2+1+1+3*anim->line;
			START_FAR_ACCESS(workaddr);
			START_FAR_STREAM(workaddr);
			anim->frame = FAR_STREAM_READ();
			offset = FAR_STREAM_READ();
			segment = FAR_STREAM_READ();
			END_FAR_STREAM();
			END_FAR_ACCESS();
		}
		switch(anim->frame) {
		case -1: // loop
			anim->line = -1;
			anim->frame = 1;
			return CAnimation_Step(anim);
		case -2: // stop
			anim->frame = -1;
			break;
		default: // cel
			anim->cel = FAR_POINTER_FROM_SEGMENT_AND_OFFSET(segment, offset);
		}
	}
	return anim->frame != -1;
}


CanvasCelRef CAnimation_Cel(CAnimation *anim) {
	return anim->cel;
}


int CAnimation_CountLines(CanvasAnimRef script) {
	u16 lines;
	far_uintptr_t workaddr = script+2+1;
	START_FAR_ACCESS(workaddr);
	START_FAR_STREAM(workaddr);
	lines = FAR_STREAM_READ();
	END_FAR_STREAM();
	END_FAR_ACCESS();
	return lines;
}

int CAnimation_CountFrames(CanvasAnimRef script) {
	int steps=0;
	far_uintptr_t workaddr;
	workaddr = script+2+1+1;
	START_FAR_ACCESS(workaddr);
	START_FAR_STREAM(workaddr);
	while(1) {
		int cmd = FAR_STREAM_READ();
		if (cmd < 0) break;
		steps += cmd;
		FAR_STREAM_READ();
		FAR_STREAM_READ();
	}
	END_FAR_STREAM();
	END_FAR_ACCESS();
	return steps;
}


void HotpointIteratorStart(HotpointIterator *itor, CanvasCelRef cel) {
	START_FAR_ACCESS(cel);
	START_FAR_STREAM(cel);
	FAR_STREAM_SKIP(2+1);
	{
		u16 lods = FAR_STREAM_READ();
		itor->addr = cel+2+1+1+3*lods+4;
	}
	END_FAR_STREAM();
	END_FAR_ACCESS();
}

u16 HotpointIteratorNext(HotpointIterator *itor) {
	u16 token;
	START_FAR_ACCESS(itor->addr);
	START_FAR_STREAM(itor->addr);
	token = FAR_STREAM_READ();
	if (token != 0) {
		itor->x = FAR_STREAM_READ();
		itor->y = FAR_STREAM_READ();
	}
	END_FAR_STREAM();
	END_FAR_ACCESS();
	itor->addr += 3;
	return token;
}

u16 HotpointIteratorNextWithToken_(HotpointIterator *itor, u16 search_token) {
	u16 token;
	START_FAR_ACCESS(itor->addr);
	START_FAR_STREAM(itor->addr);
	while (token = FAR_STREAM_READ(), token != 0) {
		itor->x = FAR_STREAM_READ();
		itor->y = FAR_STREAM_READ();
		itor->addr += 3;
		if (token == search_token) break;
	}
	END_FAR_STREAM();
	END_FAR_ACCESS();
	return token;
}

/*

void CanvasTestLevel(void) {
	CANVAS_DRAW_INFO(3) di;
	CANVAS_DRAW_INFO(2) logdi;
	CANVAS_DRAW_INFO(2) flowerdi;
	CAnimation fairyAnim;
	s16 scale = 1024, scalestep = 1;
	int wings = 0;

	SetDrawMode(DRAW_MODE_HIRES | DRAW_MODE_FRAME | DRAW_MODE_3D);

	ResetDraw();

	CanvasStatePush(CanvasFindPath(CANVAS_ROOT, "assetgroups/flight_pixiehollow"));

	{
		CanvasSpriteFamilyRef sfr = CanvasFindPath(CANVAS_ROOT, "sprites/actors/tinkerbell");
		CanvasInitDraw(sfr, &di.i);
		CAnimation_Start(&fairyAnim, CanvasFindPath(sfr, "anims/Tink_Run"));
	}

	CanvasInitDraw(CanvasFindPath(CANVAS_ROOT, "sprites/flight_pixiehollow/branchlog"), &logdi.i);

	CanvasInitDraw(CanvasFindPath(CANVAS_ROOT, "sprites/flight_pixiehollow/flowers"), &flowerdi.i);
	CanvasDrawTranslate(&di.i, 100, 100);
	CanvasDrawTranslate(&logdi.i, 100, -100);

	CanvasDrawTranslate(&flowerdi.i, 0, 480);



	SetFade(0);
	while(1)
	{
		u16 old;

		scale += scalestep;

		if (scale > 1200) scalestep = -1;
		if (scale < 800) scalestep = 1;

		BeginDraw(DRAW_API_HIRES, &old);

		CAnimation_Step(&fairyAnim);

		if (wings&2) {
			di.i.parts[1].alpha = 0;
			di.i.parts[2].alpha = 32;
		} else {
			di.i.parts[1].alpha = 32;
			di.i.parts[2].alpha = 0;
		}
		wings = (wings+1)&3;

		CanvasDrawCel(CAnimation_Cel(&fairyAnim), &di.i);
		CanvasDrawCel(CanvasFindPath(CANVAS_ROOT, "sprites/flight_pixiehollow/branchlog/cels/log"), &logdi.i);

		CanvasDrawReset(&flowerdi.i);
		CanvasDrawTranslate(&flowerdi.i, 320, 240);
		CanvasDrawScale(&flowerdi.i, scale, scale);
		{
			CanvasCelRef flower4 = CanvasFindPath(CANVAS_ROOT, "sprites/flight_pixiehollow/flowers/cels/flower4");

			HotpointIterator itor;
			HotpointIteratorStart(&itor, flower4);
			if (HotpointIteratorNextWithToken(&itor, orbit))
				CanvasDrawTranslate(&flowerdi.i, -itor.x, -itor.y);

			CanvasDrawCel(flower4, &flowerdi.i);
		}

		DrawTextEx(ANIMATION(0), "test", -1, 50, 50, 0);

		EndDraw(old);
	}
}
*/
