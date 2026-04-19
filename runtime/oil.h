#ifndef	__OIL_h__
#define	__OIL_h__
//	write your header here
#include <common/platform/sunplus/draw.h>
#include <common/types.h>

typedef struct {
	u16 type;
	u16 twidth, theight;
	u16 lq[64], cq[64];
} OilHeader;

typedef struct {
	far_uintptr_t base;
	OilHeader head;
	u16 block;
	far_uintptr_t blockbase;
	int outspan;
} OilImage;

STRUCT(OilBackground) {
	OilImage img;
	far_uintptr_t textbase;
	int whichText;
	int xoff, yoff;
	int attribute, control;
	struct {
		int leftFull, rightFull;
		int topFull, botFull;
		int partialLeftCol, partialLeftColComplete;
		int partialRightCol, partialRightColComplete;
		int botValidBlocks, topValidBlocks;
	} full, dc;
} ;

typedef struct {
	u16 codeafter[16];
	u16 indexoffset[16];
	u16 values[0];
} OilHuffTab;

typedef struct {
	s16 coeffs[64]; // IDCT coefficients in 1.15
	u16 dezigzag[64]; // table for zigzag lookup
	OilHuffTab ldc;	u16 ldc_values[12];
	OilHuffTab lac;	u16 lac_values[162];
	OilHuffTab cdc;	u16 cdc_values[12];
	OilHuffTab cac;	u16 cac_values[162];
} OilConst;

typedef struct { // tables and buffers needed only during the decompression of a single tile
	s16 cbdc[4], crdc[4], ydc[16]; // DC coeffecients
	u16 databuf[16]; // DMA buffers for bringing in data
	s16 work[64], tmp[64]; // buffers for holding data during decoding and IDCT
	s16 cb[64], cr[64]; // buffers for chroma data
	u16 row1[8], row2[8]; // DM buffers for pushing pixels to the text buffer
} OilScratchpad; // 300ish bytes?

#define OIL_BLOCK_SHIFT (6)
#define OIL_BLOCK_SIZE (1<<OIL_BLOCK_SHIFT)
#define OIL_TILE_MASK (OIL_BLOCK_SIZE-1)

#define OIL_RESOURCE(name) FAR_POINTER_FROM_EXTERN_SYMBOL(_RES_##name##_OIL_sa)

void OilInitImage(OilImage *img, far_uintptr_t src, int outSpan);
// img - image description to be populated
// src - start address of oil resource
// dst - destination 565 framebuffer base address (width=1024 words)

void OilLoadBackground(OilBackground *bg, int layer, DrawDepth depth, far_uintptr_t img);
void OilLoadTerrain(OilBackground *bg, int layer, DrawDepth depth, far_uintptr_t img);

// Loads a background but does not immediately show it.
void OilLoadBackgroundInvisible(OilBackground *bg, far_uintptr_t img);

void OilLoadFlatBackground(OilBackground *bg, int layer, DrawDepth depth, u16 index);
void OilHideBackground(OilBackground *bg);
void OilShowBackground(OilBackground *bg, int layer, DrawDepth depth);

void OilScrollBackground(OilBackground *bg, int x, int y);

void OilInit(OilConst *c);
// init const block for use by OilDecompBlock

void OilDecompBlock(far_uintptr_t dst, OilImage *img, int x, int y, const OilConst *const c, OilScratchpad *s);
void OilDecompBlockDC(far_uintptr_t dst, OilImage *img, int x, int y, const OilConst *const c); // only decompress DC coefficients

void OilClearContext();

#endif
