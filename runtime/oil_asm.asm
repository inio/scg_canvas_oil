
.external _OilConstValues

.code
.public _OilIDCT1
_OilIDCT1: // dst(col), src(row)
	PUSH BP to [SP]
	BP = SP
	r1 = [BP+5] // src
	r2 = _OilConstValues
	BP = [BP+4] // dst 
	INT OFF;
	FRACTION ON;
	MR = [r1]*[r2], ss, 8
	r1 -= 8
	[BP+0] = R4
	MR = [r1]*[r2], ss, 8
	r1 -= 8
	[BP+8] = R4
	MR = [r1]*[r2], ss, 8
	r1 -= 8
	[BP+16] = R4
	MR = [r1]*[r2], ss, 8
	r1 -= 8
	[BP+24] = R4
	BP += 32
	MR = [r1]*[r2], ss, 8
	r1 -= 8
	[BP+0] = R4
	MR = [r1]*[r2], ss, 8
	r1 -= 8
	[BP+8] = R4
	MR = [r1]*[r2], ss, 8
	r1 -= 8
	[BP+16] = R4
	MR = [r1]*[r2], ss, 8
	[BP+24] = R4
	FRACTION OFF;
	INT IRQ,FIQ;
	POP BP from [SP]
	RETF
	
.public _OilIDCT2
_OilIDCT2: // dst(row), src(row)
	PUSH BP to [SP]
	BP = SP
	r1 = [BP+5] // src
	r2 = _OilConstValues
	BP = [BP+4] // dst 
	INT OFF;
	FRACTION ON;
	MR = [r1]*[r2], ss, 8
	r1 -= 8
	r4 = r4 ASR 2
	[BP++] = R4
	MR = [r1]*[r2], ss, 8
	r1 -= 8
	r4 = r4 ASR 2
	[BP++] = R4
	MR = [r1]*[r2], ss, 8
	r1 -= 8
	r4 = r4 ASR 2
	[BP++] = R4
	MR = [r1]*[r2], ss, 8
	r1 -= 8
	r4 = r4 ASR 2
	[BP++] = R4
	MR = [r1]*[r2], ss, 8
	r1 -= 8
	r4 = r4 ASR 2
	[BP++] = R4
	MR = [r1]*[r2], ss, 8
	r1 -= 8
	r4 = r4 ASR 2
	[BP++] = R4
	MR = [r1]*[r2], ss, 8
	r1 -= 8
	r4 = r4 ASR 2
	[BP++] = R4
	MR = [r1]*[r2], ss, 8
	r4 = r4 ASR 2
	[BP++] = R4
	FRACTION OFF;
	INT IRQ,FIQ;
	POP BP FROM [SP]
	RETF