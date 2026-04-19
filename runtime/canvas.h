#ifndef	__CANVAS_h__
#define	__CANVAS_h__

#include <common/math/fixmath.h>

#ifdef SUNPLUS
# include <../data/sunplus/Canvas/canvas_tokens.h>
#ifndef CANVAS_TOKEN_HP_tl_collision
	#define CANVAS_TOKEN_HP_tl_collision -1
#endif
#ifndef CANVAS_TOKEN_HP_br_collision
	#define CANVAS_TOKEN_HP_br_collision -1
#endif

#endif
// File: Canvas.h

// Section: Resource Management


typedef unsigned long CanvasRef;

typedef CanvasRef CanvasAssetGroupRef;
typedef CanvasRef CanvasTargetPaletteRef;
typedef CanvasRef CanvasSpriteFamilyRef;
typedef CanvasRef CanvasCelRef;
typedef CanvasRef CanvasAnimRef;


#define MAX_LODS 10
#define MAX_PARTS 5
#define MAX_SLOTS 10
#define MAX_PARTS_TIMES_SLOTS 20

#define CANVAS_ROOT FAR_POINTER_FROM_EXTERN_SYMBOL(_CANVAS_ROOT)


// Function: CanvasFindObject
// locate an object based immediatley within a container
CanvasRef CanvasFindObject(CanvasRef base, const char *path);

// Function: CanvasFindPath
// locate an object based on a /-separated path
CanvasRef CanvasFindPathFar(CanvasRef base, DECLARE_FAR_POINTER(const char) fullPath);

#define HASH__(x) HASH_(x)
#define HASH_(x) #x
#define FAR_STRING(x) ({ static const char string[] __attribute__((section(".data\n_" HASH__(__LINE__) "__string:" ))) = x; (void)string; FAR_POINTER_FROM_SYMBOL_ASM(CONCAT(__LINE__,__string)); })

// Function: CanvasFindPath
// locate an object based on a /-separated path
CanvasRef CanvasFindPath(CanvasRef base, const char *fullPath);

#define CanvasFindPath(base, path) CanvasFindPathFar(base, FAR_STRING(path))

// Function: CanvasFindPaths
// locate an object based on many /-separated paths.
// Termination is handled automatically by a macro.
CanvasRef CanvasFindPaths(CanvasRef base, ...);

#define CanvasFindPaths(x, ...) CanvasFindPaths(x, __VA_ARGS__, (const char *)0)

CanvasRef CanvasFindPathf(CanvasRef base, const char *fmt, ...);

// Function: CanvasStatePush
// loads a state (palette environment)
// if state was previously loaded, this happens very quickly
void CanvasStatePush(CanvasAssetGroupRef ref); // mem+far push

// Function: CanvasStatePushPreload
// decompress resources for a state, but don't set up palette
// if state was previously loaded, this happens very quickly
void CanvasStatePushPreload(CanvasAssetGroupRef ref); // mem+far push

// Function: CanvasStateActivatePreloaded
// loads palette and actually sets current state
void CanvasStateActivatePreloaded();

// Function: CanvasStatePop
// returns to state before last push and free memory
void CanvasStatePop(); // mem+far pop

// Function: CanvasGetFarLoadedResource
// locate the memory location of a loaded resource
// complexity is O(log(n))
far_uintptr_t CanvasGetFarLoadedResource(CanvasRef ref);

// Function: CanvasGetNearLoadedResource
// locate the memory location of a loaded resource
// complexity is O(k*log(n))
void* CanvasGetNearLoadedResource(CanvasRef ref);

// =====================================================================

#define CanvasPartIndex(family, part) CONCAT(CONCAT(CNV_SF_PRT_,family),CONCAT(_PI_,part))
#define CanvasCountParts(family) SpriteFamily_PartIndex(family, $end)

#define CNV_HOTPOINT(x) (CANVAS_TOKEN_HP_##x)
#define CNV_PALSWAP(x) (CANVAS_TOKEN_##x)


typedef struct {
	struct TargetPaletteSlots *palette;
	u16 alpha;
	u16 spriteOffset; // offset in slots, used in looking up decompressed cels
} CanvasSpriteFamilyPart;

typedef struct {
	u16 numParts;
	u16 totalSlots;
	CanvasSpriteFamilyPart parts[0];
} CanvasSpriteFamily;

typedef struct {
	u16 slot;
	u16 alpha;
} CanvasPartInfo;

enum {
	DI_TRANFORM_UNIFORM_SCALE = 1,
	DI_TRANFORM_NONUNIFORM_SCALE = 2,
	DI_TRANFORM_SCALE = DI_TRANFORM_UNIFORM_SCALE | DI_TRANFORM_NONUNIFORM_SCALE,
	DI_TRANFORM_ROTATE = 4,
};

STRUCT(CanvasDrawInfo) {
	u16 transform_flags;
	union {
		u16 flagsraw;
		struct {
			u16 z0:2, hflip:1, z1:9, depth:2, z2:2;
		} flags;
	};
	u16 mosaic;
	u16 depth;
	fixw scale; // used for LOD calcs
	CanvasSpriteFamily *family;
	fixw transform[2][3]; // [0][2] and [1][2] are 16.0, rest are 6.10
	CanvasPartInfo parts[0];
};

// Hard coding to always use 5 parts.
//#define CANVAS_DRAW_INFO(parts) struct{CanvasDrawInfo i; CanvasPartInfo _parts[parts];}
#define CANVAS_DRAW_INFO(parts) struct{CanvasDrawInfo i; CanvasPartInfo _parts[10];}

// Section: Draw Functions

// Function: CanvasDrawCel
// clobbers transform of passed in drawInfo
void CanvasDrawCel(CanvasCelRef cel, CanvasDrawInfo *drawInfo);


// Function: CanvasTransformPoint
// Transforms a sprite-space coordinate [x,y] into screen space using the transform in drawInfo
void CanvasTransformPoint(s16 pt[2], CanvasDrawInfo *drawInfo);

// Function: CanvasAllocDraw
// MemoryAllocate and CanvasInitDraw a CanvasDrawInfo with the right number of parts for the supplied family
CanvasDrawInfo* CanvasAllocDraw(CanvasSpriteFamilyRef sfr);


CanvasSpriteFamilyRef CanvasGetSpriteFamilyFromCel(CanvasCelRef cel);

// Function: CanvasInitDraw
// load default alpha from sprite family
// also performs a CanvasDrawReset
void CanvasInitDraw(CanvasSpriteFamilyRef sfr, CanvasDrawInfo *out);

// Function: CanvasDrawReset
void CanvasDrawReset(CanvasDrawInfo *out);
// Function: CanvasDrawResetLores
// like CanvasDrawReset, but pretend screen is 320x240
void CanvasDrawResetLores(CanvasDrawInfo *out);
// Function: CanvasDrawRotate
void CanvasDrawRotate(CanvasDrawInfo *out, u16 rot);
// Function: CanvasDrawTranslate
void CanvasDrawTranslate(CanvasDrawInfo *out, s32 x, s32 y);
// Function: CanvasDrawScale
void CanvasDrawScale(CanvasDrawInfo *out, fixw sx, fixw sy);
// Function: CanvasDrawTransform3D
void CanvasDrawTransform3D(CanvasDrawInfo *out, s32 x, s32 y, s32 z);
// Function: CanvasDrawTransform3Dv
void CanvasDrawTransform3Dv(CanvasDrawInfo *out, s32 xyz[3]);
// Function: CanvasDrawLocate3Dv
void CanvasDrawLocate3Dv(CanvasDrawInfo *out, s32 xyz[3]);

// Function: CanvasGetPaletteSlot
u16 *CanvasGetPaletteSlot(CanvasTargetPaletteRef pal, int slot);

// Function: CanvasGetPaletteSize
s16 CanvasGetPaletteSize(CanvasTargetPaletteRef pal);

// Function: CanvasSetPalette
void CanvasSetPalette_(CanvasTargetPaletteRef pal, int slot, int swapid);
#define CanvasSetPalette(pal, slot, id) CanvasSetPalette_(pal, slot, CNV_PALSWAP(id))



/*
// Function: CanvasPaletteSize
int CanvasPaletteSize(CanvasPaletteRef pal);
// Function: CanvasReadPalette
void CanvasReadPalette(CanvasPaletteRef pal, u16 *dst);
// Function: CanvasBlendPalette
void CanvasBlendPalette(CanvasPaletteRef pal, u16 *dst, fixw blend);
// Function: CanvasSetRuntimePalette
void CanvasSetRuntimePalette(CanvasSpriteFamilyRef family, int index, u16 *pal);*/

typedef struct {
	far_uintptr_t addr;
	s16 x, y;
} HotpointIterator;

void HotpointIteratorStart(HotpointIterator *itor, CanvasCelRef cel);
u16 HotpointIteratorNext(HotpointIterator *itor);
u16 HotpointIteratorNextWithToken_(HotpointIterator *itor, u16 token);
#define HotpointIteratorNextWithToken(itor, token) HotpointIteratorNextWithToken_(itor, CNV_HOTPOINT(token))

// =====================================================================

typedef DECLARE_FAR_POINTER(const void) CAnimScriptType;

typedef struct {
	CanvasAnimRef ref;
	int line;
	int frame;
	CanvasCelRef cel;
} CAnimation;


// Section: Animation Functions

// Function: CAnimation_Start
void CAnimation_Start(CAnimation *anim, CanvasAnimRef script);

#define CAnimation_Set(anim, script) ((anim)->ref != (script) && (CAnimation_Start(anim, script), 1))

// Function: CAnimation_GotoLine
// Sets the animScript to the specified line of the script (O(1))
// (replaces AnimationInit in the old system).
// Unsafe if line is off anim.
void CAnimation_GotoLine(CAnimation *anim, int line);

// Function: CAnimation_GotoFrame
// Sets the animScript to the specified frame of the script (O(n))
// Safe if frame is off anim.
void CAnimation_GotoFrame(CAnimation *anim, int frame);

// Function: CAnimation_Step
// Steps one frame of animation, if the script ends (and doesn't loop), then the fallbackScript is started.
// Returns:
//   0 - if the script ends (with or without a fallback).
//   1 - if the script didn't end.
int CAnimation_Step(CAnimation *anim);

// Function: CAnimation_Sprite
// The current sprite selected by this animation.
CanvasCelRef CAnimation_Cel(CAnimation *anim);

// Function: CAnimation_CountLines
// Counts the number of lines in a script.
int CAnimation_CountLines(CanvasAnimRef script);

// Function: CAnimation_CountFrames
// Counts the number of frame steps in a script.
int CAnimation_CountFrames(CanvasAnimRef script);

// Function: CAnimation_GetSpriteForLine
// Look up the sprite to draw for a specified animation line.
//CanvasCelRef CAnimation_GetSpriteForLine(CanvasAnimRef script, int line);


#endif
