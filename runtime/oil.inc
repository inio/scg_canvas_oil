.include common\platform\sunplus\Body.inc

// [r8:r9] next source address
// r10 offset within current buffer
// r11 address of current buffer
// r12 bits that flip between address of current buffer and other buffer
// r13 data word (shifted as bits are used)

InitBits: .MACRO baseseg, baseoff, buffer
	r8 = baseoff
	r9 = baseseg
	r10 = 0
	r11 = buffer
	// r12 = r11 ^ (r11+8)
	r12 = r11
	r12 += 8
	r12 ^= r11
	PUSH R4,R1 to [SP]
	CALL _DoDMA
	POP R1,R4 from [SP]
	r11 ^= r12
	.ENDM

GetBits: .macro out,size,tmp,entry
getbit#:
	cmp size, 0
	jz done#
	GetBit out, tmp
	size = size - 1
entry:
	tmp = r10 & 0xf
	cmp tmp, size 
	jl getbit#
	r10 = r10 - size
bulkread#:
	cmp size, 2
	jl bulkread1#
	je bulkread2#
bulkread3#:
	tmp = r13
	tmp = tmp LSL 3 // shift in bits we want
	r13 = tmp 
	tmp = tmp LSL 1 // shift one throw-away bits in to left-align the bit we care about
	out = out ROL 3 // "roll" in the bits
	size = size - 3
	jne bulkread#
done#:
	retf
bulkread2#:
	tmp = r13
	tmp = tmp LSL 2 // shift in bits we want
	r13 = tmp
	tmp = tmp LSL 2 // shift two throw-away bits in to left-align the bit we care about
	out = out ROL 2 // "roll" in the bits
	retf
bulkread1#:
	cmp size, 1
	jl done#
	tmp = r13
	tmp = tmp LSL 1
	r13 = tmp
	tmp = tmp LSL 3 // shift three throw-away bits in to left-align the bit we care about
	out = out ROL 1 // "roll" in the bits
	retf
	.endm

GetBit: .MACRO out,tmp
	TEST r10, 15
	jnz datainreg#
	cmp r10, 0
	jne datainbuf#
	// r11 points to now-used buffer.  There is already a pending request
	// for the other buffer.  This waits for that to be done, and starts
	// one for this buffer.
	PUSH R4,R1 to [SP]
	CALL _DoDMA
	POP R1,R4 from [SP]
	// other buffer is now done copying, switch to it.
	r11 ^= r12
	r10 = 128
datainbuf#:
	// pull next byte out of the buffer
	tmp = r10
	tmp = -tmp ASR 4
	tmp = tmp+8
	tmp += r11
	tmp = [tmp]
	r13 = tmp
datainreg#:
	tmp = r13
	r10 = r10-1
	tmp = tmp LSL 1
	r13 = tmp 
	tmp = tmp LSL 3 // shift three throw-away bits in to left-align the bit we care about
	out = out ROL 1
	.ENDM

Get565FromCMA: .MACRO out, tmp
	out = [P_CMA_B_V_Out];
	tmp = [P_CMA_G_U_Out];
	out = out LSR 3;
	tmp = tmp LSL 3;
	tmp &= 0x07E0;
	out |= tmp;
	tmp = [P_CMA_R_Y_Out];
	tmp = tmp LSL 4;
	tmp = tmp LSL 4;
	tmp &= 0xF800;
	out |= tmp; // 16
	.ENDM
	
//static int readVLI(int s) :
//	int out = 0;
//	if(0 == s)
//		return out;
//
//	GetBit(out);
//
//	if(out)
//	{
//		while(--s>0) GetBit(out);
//		return out;
//	}
//	else
//	{
//		out = -2;
//		while(--s>0) GetBit(out);
//		return out + 1;
//	}

alt_code: .section .code

readVLI: .macro out,in,tmp
	.ifndef D_readVLI_@out@in@tmp
	.define D_readVLI_@out@in@tmp
	.alt_code
readVLI_@out@in@tmp:
	out = 0
	in = in - 1
	jl done#
	GetBit out, tmp
	cmp out, 0
	je negative#
positive#:
	call GetBits_entry#
	retf
negative#:
	out = -2
	call GetBits_entry#
	out = out + 1
done#:
	retf
GetBits#:
	GetBits out, in, tmp, GetBits_entry#
	.code
	.endif
	call readVLI_@out@in@tmp
	.ENDM
	
	
DoFarRead: .MACRO
//	R1 = FR
//	INT OFF
//	PUSH R1,R1 to [SP]
	R1 = DS
	R2 = r9
	r3 = r8
	DS = R2
	R2 = R11
	R4 = D:[R3++];
	[R2++] = R4;
	R4 = D:[R3++];
	[R2++] = R4;
	R4 = D:[R3++];
	[R2++] = R4;
	R4 = D:[R3++];
	[R2++] = R4;
	R4 = D:[R3++];
	[R2++] = R4;
	R4 = D:[R3++];
	[R2++] = R4;
	R4 = D:[R3++];
	[R2++] = R4;
	R4 = D:[R3++];
	[R2++] = R4;
	R8 += 8
	R9 += 0, Carry
	DS = R1
//	POP R1,R1 from [SP]
//	7FR = R1
	.ENDM

FillZeros: .MACRO dst,tmp1,tmp2
	tmp2 = 0;
	tmp1 = 8;
loop#:
	[dst++] = tmp2;
	[dst++] = tmp2;
	[dst++] = tmp2;
	[dst++] = tmp2;
	[dst++] = tmp2;
	[dst++] = tmp2;
	[dst++] = tmp2;
	tmp1-=1;
	jnz loop#;
	.ENDM