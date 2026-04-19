
#include <common/platform/sunplus/oil.h>
#include <common/platform/sunplus/Body.h>
#include <common/platform/sunplus/dma.h>
#include <common/memory.h>
#include <common/platform/sunplus/farheap.h>
#include <common/types.h>
#include <stddef.h>

// [r8:r9] next source address
// r10 offset within current buffer
// r11 address of current buffer
// r12 bits that flip between address of current buffer and other buffer
// r13 data word (shifted as bits are used)

// large assembler blocks are defined over here as macros so they can use macro labels.
asm(".include common/platform/sunplus/oil.inc");

#define InitBits(dataaddr, scratch) { \
	far_pointer_u addr;\
	int segment, offset;\
	addr.ptr = (dataaddr);\
	segment = addr.part.segment; \
	offset = addr.part.offset; \
    asm ( "InitBits %0, %1, %2 " :: "r"(segment), "r"(offset), "r"(scratch)); \
}

#define GetBit(dst) { \
	int tmp;\
	asm ( "GetBit %0, %1" : "=&r" (dst), "=r"(tmp) : "0" (dst) );\
}

#define StopBits() WaitDMA0()

#define InitDMA InitOilDMA
#define WaitDMA WaitOilDMA
#define StartDMA StartOilDMA

void InitOilDMA() {
	Port(DMA_Ctrl0)      = 0x0200;
   	Port(DMA_Ctrl0)      = 0x0001;
}

void WaitOilDMA(void) {
	while(Port(DMA_Ctrl0) & 0x0008) {} // pending transfer
	while(Port(DMA_INT) & 0x0100) {} // busy
}

void StartOilDMA(far_uintptr_t dst, far_uintptr_t src, int count, int burstflags) {
	while(Port(DMA_Ctrl0) & 0x0008) {}// pending transfer
//	WaitDMA();

   	Port(DMA_SRC_AddrL0) = far_pointer_offset(src);
   	Port(DMA_SRC_AddrH0) = far_pointer_segment(src)|burstflags;
   	Port(DMA_TAR_AddrL0) = far_pointer_offset(dst);
   	Port(DMA_TAR_AddrH0) = far_pointer_segment(dst);
   	Port(DMA_TCountH0)   = 0;
   	Port(DMA_TCountL0)   = count;

 //  	Port(DMA_INT) = 0x0001;
}

#define BURST_IN 0x2000
#define BURST_OUT 0x1000

void OilInitImage(OilImage *img, far_uintptr_t src, int outSpan)
{
	StartDMA((far_uintptr_t)(intptr_t)&img->head, src, sizeof(OilHeader), 0);
	img->base = src;
	img->block = -1;
	img->outspan = outSpan;
	WaitDMA();
}

void OilInitClearImage(OilImage *img)
{
	img->head.twidth = 640/64;
	img->head.theight = 480/64;
}

static int readHuff(const OilHuffTab *const tab)
{
	int code = 0;
	const u16 *codeafter_indexoffset = tab->codeafter;

	do
	{
		GetBit(code);
	}
	while(code >= *codeafter_indexoffset++);

	return tab->values[code - codeafter_indexoffset[
		offsetof(OilHuffTab, indexoffset) - offsetof(OilHuffTab, codeafter) - 1]];
}

#if 0
static int readVLI(int s) {
	int out = 1, offset = 0;
	if (s==0) return 0;
	GetBit(offset);
	offset ^= 1;
	if (offset)
		out = -2;
	s--;
	while(s>0) {
		GetBit(out);
		s--;
	}
	return out+offset;
}
#elif 0
static int readVLI(int s) {
	int out = 0;
	if(0 == s)
		return out;

	GetBit(out);

	if(out)
	{
		out = 1;
		while(--s>0) GetBit(out);
		return out;
	}
	else
	{
		out = -2;
		while(--s>0) GetBit(out);
		return out + 1;
	}
}
#else

#define readVLI(s) ({ \
	int tmp, dst; \
	int size = s; \
	asm ( "readVLI %0, %1, %2" : "=r" (dst), "=&r"(size), "=r"(tmp) : "1"(size) ); dst; \
})
#endif

extern const OilConst OilConstValues;
void OilInit(OilConst *c) {
	*c = OilConstValues;
	InitDMA();
// why doesn't this work?
/*	DoDMACopy0(
		(far_uintptr_t)(uintptr_t) c,
	 	(far_uintptr_t)(uintptr_t) &OilConstValues,
		sizeof(OilConst));*/
}

static far_uintptr_t GetTile(OilImage *img, int x, int y) {
	long tile = y*(long)img->head.twidth+x;
	int block = tile>>OIL_BLOCK_SHIFT;
	u16 tileoff;

	if (block != img->block) {
		img->block = block;

		StartDMA(
			(far_uintptr_t)(intptr_t)&img->blockbase,
			img->base+sizeof(OilHeader)+block*2,
			sizeof(far_uintptr_t),
			0);
		WaitDMA();

		img->blockbase = img->base+*((volatile far_uintptr_t*)&img->blockbase);
	}

	StartDMA(
		(far_uintptr_t)(intptr_t) &tileoff,
		img->blockbase+(tile & OIL_TILE_MASK),
		sizeof(u16),
		0);
	WaitDMA();
	return img->blockbase+*(volatile u16*)&tileoff;
}

static void DoDMA () __attribute__((unused));
// only called from inline assembly
// FIXME: should also be __attribute__((used)) but compiler doesn't know that attribute????
static void DoDMA () {
	#if 1
	int  segment, offset;
	uintptr_t target;
	WaitDMA(); // finish previous block
	asm("%0 = r8" "\n\t"
		"%1 = r9" "\n\t"
		"%2 = r11" : "=r"(offset), "=r"(segment), "=r"(target));
	StartDMA(
		(far_uintptr_t)(uintptr_t) target,
		FAR_POINTER_FROM_SEGMENT_AND_OFFSET(segment, offset),
		8,
		0);
	asm("r8 += 8" "\n\t"
		"r9 += 0, Carry");
	#else
	asm("DoFarRead" : : : "R1","R2","R3","R4");
	#endif
}

static const OilConst *gConst;
static OilImage *gImage;

void OilIDCT1(s16 *dstcol, s16 *srrow);
void OilIDCT2(s16 *dstrow, s16 *srrow);

//#define SET8(x,base) x[base+0]=0;x[base+1]=0;x[base+2]=0;x[base+3]=0;x[base+4]=0;x[base+5]=0;x[base+6]=0;x[base+7]=0

static void GetChromaBlock(s16 *dst, int dc) {
	int i;
	s16 tmp[64];
	for(i=0 ; i<64 ; i++) dst[i]=0;
	dst[0] = dc;
	i=1;
	do {
		int code;
		code = readHuff(&gConst->cac);
		if (code == 0xF0) {
			i+=16;
			continue;
		}
		if (code == 0x00)
			break;
		i += code>>4;
		dst[gConst->dezigzag[i]] = readVLI(code&0x0F)*gImage->head.cq[i];
		i++;
	} while (i<64);
	while(i>64){}
	for(i=0 ; i<8 ; i++)
		OilIDCT1(tmp+i, dst+i*8);
	for(i=0 ; i<8 ; i++)
		OilIDCT2(dst+i*8, tmp+i*8);
}

static void PrepLumaBlock(s16 *dst, int dc) {
	int i;
	s16 tmp[64];
	for(i=0 ; i<64 ; i++) tmp[i]=0;
	tmp[0] = dc;
	i=1;
	do {
		int code;
		code = readHuff(&gConst->lac);
		if (code == 0xF0) {
			i+=16;
			continue;
		}
		if (code == 0x00)
			break;
		i += code>>4;
		tmp[gConst->dezigzag[i]] = readVLI(code&0x0F)*gImage->head.lq[i];
		i++;
	} while (i<64);
	while(i>64){}
	for(i=0 ; i<8 ; i++)
		OilIDCT1(dst+i, tmp+i*8);
}

/*
static void FillBlock(s16 *dst, int dc) {
	int i;
	for(i=0 ; i<64 ; i++) dst[i] = dc>>3;
}*/

#define Get565(y, dst) ({\
	int tmp; \
	Port(CMA_R_Y_In) = (((y)&0x0100)?(((y)<0)?0:255):y); \
	asm("Get565FromCMA %0, %1":"=r"(dst), "=r"(tmp)); \
})

void OilDecompBlock(far_uintptr_t tgt, OilImage *img, int blkx, int blky, const OilConst *const c, OilScratchpad *s) {
	int u=0, v=0, y=0, px, mcu;
	gConst = c;
	gImage = img;

	WaitDMA();
	asm("FIR_MOV OFF");
	InitBits(GetTile(img, blkx, blky), &(s->databuf));
	Port(CMA_Ctrl) = 0;
	for(mcu=0 ; mcu<4 ; mcu++) {
		int block;
		s->cbdc[mcu] = u += readVLI(readHuff(&(c->cdc)))*img->head.cq[0];
		s->crdc[mcu] = v += readVLI(readHuff(&(c->cdc)))*img->head.cq[0];

		for(block=0 ; block<4 ; block++)
			s->ydc[mcu*4+block] = y += readVLI(readHuff(&(c->ldc)))*img->head.lq[0];
	}
	for(mcu=0 ; mcu<4 ; mcu++) {
		int block;
		GetChromaBlock(s->cb, s->cbdc[mcu]);
		//FillBlock(s->cb, s->cbdc[mcu]);
		GetChromaBlock(s->cr, s->crdc[mcu]);
		//FillBlock(s->cr, s->crdc[mcu]);

		for(block=0 ; block<4 ; block++) {
			int r;
			s16 *line = s->row1;
			u16 linechange = ((u16)s->row1) ^ ((u16)s->row2);

			far_uintptr_t blockdst = tgt+img->outspan*(((block&2)<<2))+(mcu<<4)+((block&1)<<3);

			PrepLumaBlock(s->work, s->ydc[mcu*4+block]);
		//	FillBlock(s->work, s->ydc[mcu*4+block]);


			for(r=0 ; r<8 ; r++) {
				int coffset = ((r>>1)<<3)+((block&1)<<2)+((block&2)<<4);
				s16 *ubase = s->cb+coffset;
				s16 *vbase = s->cr+coffset;
				OilIDCT2(s->tmp, s->work+r*8);
				Port(CMA_G_U_In) = ubase[0]+128;
				Port(CMA_B_V_In) = vbase[0]+128;
				Get565(s->tmp[0]+128, px); line[0] = px;
				Get565(s->tmp[1]+128, px); line[1] = px;
				Port(CMA_G_U_In) = ubase[1]+128;
				Port(CMA_B_V_In) = vbase[1]+128;
				Get565(s->tmp[2]+128, px); line[2] = px;
				Get565(s->tmp[3]+128, px); line[3] = px;
				Port(CMA_G_U_In) = ubase[2]+128;
				Port(CMA_B_V_In) = vbase[2]+128;
				Get565(s->tmp[4]+128, px); line[4] = px;
				Get565(s->tmp[5]+128, px); line[5] = px;
				Port(CMA_G_U_In) = ubase[3]+128;
				Port(CMA_B_V_In) = vbase[3]+128;
				Get565(s->tmp[6]+128, px); line[6] = px;
				Get565(s->tmp[7]+128, px); line[7] = px;
				WaitDMA();
				StartDMA(
					blockdst,
					(far_uintptr_t)(uintptr_t)(line),
					8, BURST_OUT);
				line = (u16*)(((u16)line) ^ linechange);
				blockdst += img->outspan;
			}
		}


	}
	StopBits(); // make sure last DMA wraps up before we pop it's target off the stack.
}

void OilDecompBlockDC(far_uintptr_t tgt, OilImage *img, int blkx, int blky, const OilConst *const c) {
	int data[16];
	int out[16];
	int u=0, v=0, y=0, px, mcu;


	InitBits(GetTile(img, blkx, blky), data);
	Port(CMA_Ctrl) = 0;
	for(mcu=0 ; mcu<4 ; mcu++) {
		int row;
		far_uintptr_t blockdst = tgt+mcu*16;
		u += readVLI(readHuff(&(c->cdc)))*img->head.cq[0];
		v += readVLI(readHuff(&(c->cdc)))*img->head.cq[0];
		Port(CMA_G_U_In) = ((u>>3)+128);
		Port(CMA_B_V_In) = ((v>>3)+128);


		for(row=0 ; row<2 ; row++) {
			y += readVLI(readHuff(&(c->ldc)))*img->head.lq[0];
			Get565(((y>>3)+128), px);
		//	px = ((v>>3)+128)>>3;
		//	px = (px<<11)|(px<<6)|(px);
			WaitDMA();
			out[0] = out[1] = out[2] = out[3] = px;
			out[4] = out[5] = out[6] = out[7] = px;

			y += readVLI(readHuff(&(c->ldc)))*img->head.lq[0];
			Get565(((y>>3)+128), px);
		//	px = ((v>>3)+128)>>3;
		//	px = (px<<11)|(px<<6)|(px);
			out[ 8] = out[ 9] = out[10] = out[11] = px;
			out[12] = out[13] = out[14] = out[15] = px;



			StartDMA(blockdst, (far_uintptr_t)(uintptr_t)out, 16, BURST_OUT);
			blockdst+=img->outspan;
			StartDMA(blockdst, (far_uintptr_t)(uintptr_t)out, 16, BURST_OUT);
			blockdst+=img->outspan;
			StartDMA(blockdst, (far_uintptr_t)(uintptr_t)out, 16, BURST_OUT);
			blockdst+=img->outspan;
			StartDMA(blockdst, (far_uintptr_t)(uintptr_t)out, 16, BURST_OUT);
			blockdst+=img->outspan;
			StartDMA(blockdst, (far_uintptr_t)(uintptr_t)out, 16, BURST_OUT);
			blockdst+=img->outspan;
			StartDMA(blockdst, (far_uintptr_t)(uintptr_t)out, 16, BURST_OUT);
			blockdst+=img->outspan;
			StartDMA(blockdst, (far_uintptr_t)(uintptr_t)out, 16, BURST_OUT);
			blockdst+=img->outspan;
			StartDMA(blockdst, (far_uintptr_t)(uintptr_t)out, 16, BURST_OUT);
			blockdst+=img->outspan;
		}
	}
	StopBits(); // make sure last DMA wraps up before we pop it's target off the stack.
}

const OilConst OilConstValues = {
	{// IDCT coeffs
		23170, 32138, 30274, 27246, 23170, 18205, 12540, 6393,
		23170, 27246, 12540, -6393, -23170, -32138, -30274, -18205,
		23170, 18205, -12540, -32138, -23170, 6393, 30274, 27246,
		23170, 6393, -30274, -18205, 23170, 27246, -12540, -32138,
		23170, -6393, -30274, 18205, 23170, -27246, -12540, 32138,
		23170, -18205, -12540, 32138, -23170, -6393, 30274, -27246,
		23170, -27246, 12540, 6393, -23170, 32138, -30274, 18205,
		23170, -32138, 30274, -27246, 23170, -18205, 12540, -6393},
	{ // dezigzag note: x and y are flipped for efficiency (see decodeblock)
		000,
		010, 001,
		002, 011, 020,
		030, 021, 012, 003,
		004, 013, 022, 031, 040,
		050, 041, 032, 023, 014, 005,
		006, 015, 024, 033, 042, 051, 060,
		070, 061, 052, 043, 034, 025, 016, 007,
		     017, 026, 035, 044, 053, 062, 071,
		          072, 063, 054, 045, 036, 027,
		               037, 046, 055, 064, 073,
		                    074, 065, 056, 047,
		                         057, 066, 057,
		                              076, 067,
		                                   077},
	// values in huffman tables are generated by Oilcomp by commented out
	// code in Hufftab.java
	{ // luma DC huffman table
		{0x0000, 0x0001, 0x0007, 0x000F, 0x001F, 0x003F, 0x007F, 0x00FF,
		 0x01FF, 0x03FE, 0x07FC, 0x0FF8, 0x1FF0, 0x3FE0, 0x7FC0, 0xFF80},
		{0x0000, 0x0000, 0x0001, 0x0008, 0x0017, 0x0036, 0x0075, 0x00F4,
		 0x01F3, 0x03F2, 0x07F0, 0x0FEC, 0x1FE4, 0x3FD4, 0x7FB4, 0xFF74}
	}, {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B},
	{ // luma AC huffman table
		{0x0000, 0x0002, 0x0005, 0x000D, 0x001D, 0x003C, 0x007C, 0x00FB,
		 0x01FB, 0x03FB, 0x07FA, 0x0FF8, 0x1FF0, 0x3FE0, 0x7FC1, 0xFFFF},
		{0x0000, 0x0000, 0x0002, 0x0007, 0x0014, 0x0031, 0x006D, 0x00E9,
		 0x01E4, 0x03DF, 0x07DA, 0x0FD4, 0x1FCC, 0x3FBC, 0x7F9C, 0xFF5D}
	}, {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
		0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
		0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72,
		0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
		0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45,
		0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
		0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75,
		0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
		0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3,
		0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
		0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
		0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
		0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4,
		0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA},
	{ // chroma DC huffman table
		{0x0000, 0x0003, 0x0007, 0x000F, 0x001F, 0x003F, 0x007F, 0x00FF,
		 0x01FF, 0x03FF, 0x07FF, 0x0FFE, 0x1FFC, 0x3FF8, 0x7FF0, 0xFFE0},
		{0x0000, 0x0000, 0x0003, 0x000A, 0x0019, 0x0038, 0x0077, 0x00F6,
		 0x01F5, 0x03F4, 0x07F3, 0x0FF2, 0x1FF0, 0x3FEC, 0x7FE4, 0xFFD4}
	}, {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B},
	{ // chroma AC huffman table
		{0x0000, 0x0002, 0x0005, 0x000C, 0x001C, 0x003C, 0x007B, 0x00FA,
		 0x01FB, 0x03FB, 0x07FA, 0x0FF8, 0x1FF0, 0x3FE1, 0x7FC4, 0xFFFF},
		{0x0000, 0x0000, 0x0002, 0x0007, 0x0013, 0x002F, 0x006B, 0x00E6,
		 0x01E0, 0x03DB, 0x07D6, 0x0FD0, 0x1FC8, 0x3FB8, 0x7F99, 0xFF5D}
	}, {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
		0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
		0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62, 0x72, 0xD1,
		0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
		0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44,
		0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
		0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74,
		0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
		0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
		0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4,
		0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
		0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
		0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4,
		0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA}
};

void OilLoadBackground(OilBackground *bg, int layer, DrawDepth depth, far_uintptr_t img)
{
	OilLoadBackgroundInvisible(bg, img);
	OilShowBackground(bg, layer, depth);
}

void OilLoadBackgroundInvisible(OilBackground *bg, far_uintptr_t img)
{
	int row;

	if(img)
		OilInitImage(&bg->img, img, 0);
	else
		OilInitClearImage(&bg->img);
	
	switch(bg->img.head.type) {
	case 1:
	
		if (bg->img.head.twidth <= 5) {
			bg->img.outspan = 320;
		} if (bg->img.head.twidth <= 8) {
			bg->img.outspan = 512;
		} else if (bg->img.head.twidth <= 10) {
			bg->img.outspan = 640;
		} else {
			bg->img.outspan = 1024;
		}
	
		bg->textbase = FarAllocate(bg->img.outspan * (long)bg->img.head.theight * 16);
	
		if(img)
		{
			MemoryPush();
			{
				OilConst *oconst = (OilConst*)MemoryAllocate(sizeof(OilConst));
				OilScratchpad *oscratch = (OilScratchpad*)MemoryAllocate(sizeof(OilScratchpad));
	
				OilInit(oconst);
	
				for(row=0 ; row<bg->img.head.theight ; row++) {
					int col;
					far_uintptr_t dst = bg->textbase + ((bg->img.outspan*(long)row))*16;
					if(row==25){}
					for(col=0 ; col<bg->img.head.twidth && col<16 ; col++) {
						OilDecompBlock(dst, &bg->img, col, row, oconst, oscratch);
						dst+=64;
					}
				}
			}
			MemoryPop();
		}
		else
		{
		}
		break;
	case 3:
	  {// hicolor 2.5bpp
		u16 macropal[256*4];
		u16 nummp;
		
		START_FAR_ACCESS(bg->img.base);
		START_FAR_STREAM(bg->img.base);
		
		FAR_STREAM_READ();FAR_STREAM_READ();FAR_STREAM_READ(); // skip header
		
		nummp = FAR_STREAM_READ();
		
		{
			int i;
			u16 *ptr = macropal;
			for(i=0 ; i<nummp ; i++) {
				*ptr++ = FAR_STREAM_READ();
				*ptr++ = FAR_STREAM_READ();
				*ptr++ = FAR_STREAM_READ();
				*ptr++ = FAR_STREAM_READ();
			}
		}
		
	
		if (bg->img.head.twidth <= 320/4) {
			bg->img.outspan = 320;
		} if (bg->img.head.twidth <= 512/4) {
			bg->img.outspan = 512;
		} else if (bg->img.head.twidth <= 640/4) {
			bg->img.outspan = 640;
		} else {
			bg->img.outspan = 1024;
		}
	
		bg->textbase = FarAllocate(bg->img.outspan * (long)bg->img.head.theight * 4);
		
		
		if(img)
		{
			
			MemoryPush();
			{
				u16 *rowbuf1 = (u16*)MemoryAllocate(bg->img.outspan*4);
				u16 *rowbuf2 = (u16*)MemoryAllocate(bg->img.outspan*4);
				u16 *curbuff = rowbuf1;
	
				
				
				for(row=0 ; row<bg->img.head.theight ; row++) {
					u16 *dst = curbuff;
					int col;
					
					for(col=0 ; col<bg->img.head.twidth ; col+=2) {
						u16 x3;
						u16 micropals = 0;
						for( x3 = 0 ; x3 < 8 ; x3+=4) {
							micropals <<= 8;
							if (!x3) micropals = FAR_STREAM_READ();
							if (!(micropals&0xFF00)) {
								u16 *rowptr = dst+x3;
								u16 y = 0;
								for(y=0 ; y<4 ; y++) {
									rowptr[0] = rowptr[1] = rowptr[2] = rowptr[3] = 0x8000;
									rowptr += bg->img.outspan;
								}
							} else {
								u16 *micropal =macropal+ ((micropals&0xFF00)>>6);
								u16 y, chunk = 0;
								u16 *rowptr = dst+x3;
								for(y=0 ; y<4 ; y++) {
									u16 x;
									if (!(y&1)) chunk = FAR_STREAM_READ();
									for(x=0 ; x<4 ; x++) {
										u16 pixel;
										asm("%1 = %2 ROL 2\n\t"
											"%0 = %0 ROL 4\n\t"
											"%0 &= 3\n\t" : "=r"(pixel), "=r"(chunk) : "r"(chunk));
										rowptr[x] = micropal[pixel];
									}
									rowptr += bg->img.outspan;
								}
							}
						}
						dst += 8;
					}
					
					WaitDMA();
					StartDMA(bg->textbase + bg->img.outspan*(long)(row*4), (far_uintptr_t)(uintptr_t)curbuff, bg->img.outspan*4, BURST_OUT);
					
					if (curbuff == rowbuf1)
						curbuff = rowbuf2;
					else
						curbuff = rowbuf1;
				}
			}
			WaitDMA();
			MemoryPop();
			
		}
		END_FAR_STREAM();
		END_FAR_ACCESS();
		
	  } break;
	}
	bg->xoff = 0;
	bg->yoff = 0;
	bg->whichText = -1;

}

void OilLoadFlatBackground(OilBackground *bg, int layer, DrawDepth depth, u16 index)
{
	bg->img.outspan = 0;
	if (index & 0x0100)
		bg->img.block = 1;

	bg->textbase = FarAllocate(8*8);

	{
		u16 px = (index&0xFF) | ((index&0xFF)<<8);
		int i;
		START_FAR_ACCESS(bg->textbase);
		START_FAR_STREAM(bg->textbase);
		for(i=0 ; i<8*8 ; i++) {
			FAR_STREAM_WRITE(px);
		}
		END_FAR_STREAM();
		END_FAR_ACCESS();
		bg->textbase -= 4*8; // so that it's character 1
	}

	bg->xoff = 0;
	bg->yoff = 0;
	bg->whichText = -1;

	OilShowBackground(bg, layer, depth);
}

static OilBackground *loaded[4] = {0,0,0,0};

u16 armyofone[] = {1};

void OilShowBackground(OilBackground *bg, int layer, DrawDepth depth)
{
	u16 attribute=0, control=0;
	if (bg->img.outspan == 0) {
		// flat color, 8-bit
		attribute = (0/*size*/<<14)|(0/*palette*/<<8)
				  | (0/*vs*/<<6)|(0/*hs*/<<4)|(0/*flip*/<<2)|(3/*color*/<<0);
		control = (0/*bldlvl*/<<10)|(0/*bldmode*/<<9)|(0/*bld*/<<8)|(0/*rgbm*/<<7)|(0/*mode*/<<5) \
			   | (0/*mve*/<<4)|(1/*en*/<<3)|(1/*wall*/<<2)|(1/*regm*/<<1)|(0/*bmp*/<<0);
	} else {
		switch(bg->img.head.type) {
		case 1:
			attribute = (2/*size*/<<14)|(0/*palette*/<<8)
					  | (0/*vs*/<<6)|(3/*hs*/<<4)|(0/*flip*/<<2)|(1/*color*/<<0);
			control = (0/*bldlvl*/<<10)|(0/*bldmode*/<<9)|(0/*bld*/<<8)|(1/*rgbm*/<<7)|(0/*mode*/<<5) \
				   | (0/*mve*/<<4)|(1/*en*/<<3)|(0/*wall*/<<2)|(1/*regm*/<<1)|(1/*bmp*/<<0);
			break;
		case 3:
			attribute = (2/*size*/<<14)|(0/*palette*/<<8)
					  | (0/*vs*/<<6)|(3/*hs*/<<4)|(0/*flip*/<<2)|(0/*color*/<<0);
			control = (0/*bldlvl*/<<10)|(0/*bldmode*/<<9)|(0/*bld*/<<8)|(1/*rgbm*/<<7)|(0/*mode*/<<5) \
				   | (0/*mve*/<<4)|(1/*en*/<<3)|(0/*wall*/<<2)|(1/*regm*/<<1)|(1/*bmp*/<<0);
			break;
		}
	
		attribute |= (
			bg->img.outspan == 320 ? 0 :
			bg->img.outspan == 640 ? 1 :
			bg->img.outspan <= 512 ? 2 : 3
		) << 6;
	}
	attribute |= depth<<12;

	bg->attribute = attribute;
	bg->control = control;
	bg->whichText = layer;
	loaded[layer-1] = bg;

	#undef setup
}

void OilScrollBackground(OilBackground *bg, int x, int y) {
	bg->xoff = x;
	bg->yoff = y;
}

void OilVBlankWork(void) {
	#define setup(n) do {\
		Port(Segment_Tx##n##H) = far_pointer_segment(loaded[n-1]->textbase); \
		Port(Segment_Tx##n##) = far_pointer_offset(loaded[n-1]->textbase); \
		if (loaded[n-1]->img.outspan == 0) Port(Tx##n##_N_PTR) = (u16)armyofone; \
		Port(Tx##n##_Attribute) = loaded[n-1]->attribute; \
		Port(Tx##n##_Control) = loaded[n-1]->control; \
		Port(Tx##n##_X_Position) = loaded[n-1]->xoff; \
		Port(Tx##n##_Y_Position) = loaded[n-1]->yoff; \
	}while(0)

	if (loaded[0]) setup(1);
	if (loaded[1]) setup(2);
	if (loaded[2]) setup(3);
	if (loaded[3]) setup(4);
}

void OilHideBackground(OilBackground *bg) {
	if (bg->whichText == -1) return;
	#define teardown(n) \
		Port(Tx##n##_Attribute) = 0; \
		Port(Tx##n##_Control) = 0;

	switch(bg->whichText) {
	case 1: teardown(1); break;
	case 2: teardown(2); break;
	case 3: teardown(3); break;
	case 4: teardown(4); break;
	}

	loaded[bg->whichText-1] = 0;
	
	bg->whichText = -1;

	#undef teardown
}

STRUCT(LoadedState) {
	OilBackground *loaded[4];
};

static MAKE_DESTRUCTOR(RestoreLoadedOilBGLater, LoadedState, state)
{
	int i;
		
	loaded[0] = state.loaded[0];
	loaded[1] = state.loaded[1];
	loaded[2] = state.loaded[2];
	loaded[3] = state.loaded[3];
	
	for(i = 0; i < 4; i++)
	{
		if(loaded[i])
			loaded[i]->whichText = i+1;
	}
}

void OilClearContext()
{
	int i;
	LoadedState state;
	
	state.loaded[0] = loaded[0];
	state.loaded[1] = loaded[1];
	state.loaded[2] = loaded[2];
	state.loaded[3] = loaded[3];
	
	for(i = 0; i < 4; i++)
	{
		if(loaded[i])
			OilHideBackground(loaded[i]);
	}
	
	RestoreLoadedOilBGLater(state);
}