#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>



#define DELTA
#define DELTA_OP +


/* Pucrunch ©1997-2008 by Pasi 'Albert' Ojala, a1bert@iki.fi */
/* Pucrunch is now under LGPL: see the doc for details. */


/* #define BIG */
/*
    Define BIG for >64k files.
    It will use even more *huge* amounts of memory.

    Note:
    Although this version uses memory proportionally to the file length,
    it is possible to use fixed-size buffers. The LZ77 history buffer
    (and backSkip) needs to be as long as is needed, the other buffers
    minimally need to be about three times the length of the maximum
    LZ77 match. Writing the compressor this way would probably make it a
    little slower, and automatic selection of e.g. escape bits might not be
    practical.

    Adjusting the number of escape bits to adapt to local
    changes in the data would be worth investigating.

    Also, the memory needed for rle/elr tables could probably be reduced
    by using a sparse table implementation. Because of the RLE property
    only the starting and ending points (or lengths) need be saved. The
    speed should not decrease too much, because the tables are used in
    LZ77 string match also.... Wait! Actually no, because the RLE/LZ77
    optimize needs to change the RLE lengths inside RLE's...

    The elr array can be reduced to half by storing only the byte that
    is before a run of bytes if we have the full backSkip table..

    Because the lzlen maximum value is 256, we could reduce the table
    from unsigned short to unsigned char by encoding 0->0, 2->1, .. 256->255.
    lzlen of the value 1 is never used anyway..

 */

#define ENABLE_VERBOSE      /* -v outputs the lz77/rle data to stdout */
#define HASH_STAT	    /* gives statistics about the hash compares */
#define BACKSKIP_FULL       /* full backSkip table - enables RESCAN. If */
                            /* not defined, backSkip only uses max 128kB */
#define RESCAN		    /* rescans LZ77 matches for a closer match. */
#define HASH_COMPARE    /* Use a 3-to-1 hash to skip impossible matches */
/* takes "inbytes" bytes, reduces string compares from 16% to 8% */



const char version[] = "\0$VER: pucrunch 1.14 22-Nov-2008\n";



static int maxGamma = 7, reservedBytes = 2;
static int escBits = 2, escMask = 0xc0;
static int extraLZPosBits = 0, rleUsed = 15;

static int memConfig = 0x37, intConfig = 0x58; /* cli */


/*
-------->
    z..zx.....x						     normal (zz != ee)
    e..e	value(LEN)	value(POSHI+1)	8+b(POSLO)   LZ77
    e..e	0    (2)	0 (2-256)	8b(POSLO)    LZ77
    e..e	100  (3)	111111 111111		     END of FILE
#ifdef DELTA
    e..e	101  (4..)	111111 111111	8b(add) 8b(POSLO)	DLZ
#endif

    e..e010	n..ne.....e				     escape + new esc
    e..e011	value(LEN)	bytecode		     Short RLE  2..
    e..e011	111..111 8b(LENLO) value(LENHI+1) bytecode   Long RLE
		(values 64.. not used (may not be available) in bytecode)


e..e011 0 0			RLE=2, rank 1 (saves 11.. bit)
e..e011 0 10 x			RLE=2, rank 2-3 (saves 9.. bit)
e..e011 0 11 0xx		RLE=2, rank 4-7 (saves 7.. bit)
e..e011 0 11 10xxx		RLE=2, rank 8-15 (saves 5.. bit)
e..e011 0 11 110xxxx xxxx	RLE=2, not ranked


LZ77, len=2 (pos<=256) saves 4 bits (2-bit escape)
LZ77, len=3 saves 10..1 bits (pos 2..15616)
LZ77, len=4 saves 18..9 bits
LZ77, len=5 saves 24..15 bits

RLE, len=2 saves 11..1(..-5) bits (bytecode rank 1..not ranked)
RLE, len=3 saves 15..2 bits
RLE, len=4 saves 23..10 bits
RLE, len=5 saves 29..16 bits

bs: 3505 LZ reference points, 41535 bytes -> 11.85, i.e. 8.4% referenced


 1) Short RLE -> gamma + 1 linear bit -> ivanova.run -29 bytes

 2) ?? .. no
    esc = RLE, with value 1
    e..e01 value(1)	n..ne.....e			     escape + new esc
    e..e01 value(LEN)	bytecode			     Short RLE  2..
    e..e01 111..111 8b(LENLO) value(LENHI+1) bytecode        Long RLE
		(values 64.. not used (may not be available) in bytecode)


*/

/*
Value:

Elias Gamma Code rediscovered, just the prefix bits are reversed, plus
there is a length limit (1 bit gained for each value in the last group)
; 0000000	not possible
; 0000001	0		1			-6 bits
; 000001x	10	x	2-3			-4 bits
; 00001xx	110 	xx	4-7			-2 bits
; 0001xxx	1110 	xxx	8-15			+0 bits
; 001xxxx	11110	xxxx	16-31			+2 bits
; 01xxxxx	111110	xxxxx	32-63			+4 bits
; 1xxxxxx	111111	xxxxxx	64-127			+5 bits

*/


#include "pucrunch.h" /* Include the decompressors */


void ListDecompressors(FILE *fp) {
  struct FixStruct *dc = &fixStruct[0];

  while (dc && dc->code) {
    fprintf(fp, "%s\n", dc->name);
    dc++;
  }
}

struct FixStruct *BestMatch(int type) {
    struct FixStruct *dc = &fixStruct[0], *best = NULL;

    while (dc && dc->code) {
	if ((dc->flags & FIXF_MACHMASK) == (type & FIXF_MACHMASK)) {
	    /* machine is correct */
	    /* Require wrap if necessary, allow wrap if not */
	    /* Require delta matches */
	    if (((dc->flags & type) & FIXF_MUSTMASK) ==
		(type & FIXF_MUSTMASK)) {

		/* Haven't found any match or this is better */
		if (!best ||
		    ((type & FIXF_WRAP) == (dc->flags & FIXF_WRAP) &&
		     (!(type & (FIXF_FAST | FIXF_SHORT)) ||
		      (dc->flags & type & (FIXF_FAST | FIXF_SHORT)))))
		    best = dc;
		/* If requirements match exactly, can return */
		/* Assumes that non-wraps are located before wrap versions */
		if ((type & (FIXF_FAST | FIXF_SHORT)) ==
		    (dc->flags & (FIXF_FAST | FIXF_SHORT))) {
		    return dc;
		}
	    }
	}
	dc++;
    }
    return best;
}


int GetHeaderSize(int type, int *deCall) {
    struct FixStruct *best;
    if (deCall)
	*deCall = 0;
    if ((type & FIXF_MACHMASK) == 0) {
	return 47; /* standalone */
    }
    best = BestMatch(type);
    if (best && deCall) {
	int i;
	for (i=0; best->fixes[i].type != ftEnd; i++) {
	    if (best->fixes[i].type == ftDeCall) {
		*deCall = best->fixes[i].offset;
		break;
	    }
	}
    }
    return best?best->codeSize:0;
}


int SavePack(int type, unsigned char *data, int size, char *target,
	     int start, int exec, int escape, unsigned char *rleValues,
	     int endAddr, int progEnd, int extraLZPosBits, int enable2MHz,
	     int memStart, int memEnd) {
    FILE *fp = NULL;
    struct FixStruct *dc;
    unsigned char *header;
    int i, overlap = 0, stackUsed = 0, ibufferUsed = 0;

    if (!data)
	return 10;
    if (!target)
	fp = stdout;

    if ((type & FIXF_MACHMASK) == 0) {
	/* Save without decompressor */

	if (fp || (fp = fopen(target, "wb"))) {
	    unsigned char head[64];
	    int cnt = 0;

	    head[cnt++] = (endAddr + overlap - size) & 0xff;	/* INPOS */
	    head[cnt++] = ((endAddr + overlap - size) >> 8);

	    head[cnt++] = 'p';
	    head[cnt++] = 'u';

	    head[cnt++] = (endAddr - 0x100) & 0xff;
	    head[cnt++] = ((endAddr - 0x100) >> 8);

	    head[cnt++] = (escape>>(8-escBits));
	    head[cnt++] = (start & 0xff);	/* OUTPOS */
	    head[cnt++] = (start >> 8);
	    head[cnt++] = escBits;
	    /* head[cnt++] = 8-escBits; */

	    head[cnt++] = maxGamma + 1;
	    /* head[cnt++] = (8-maxGamma); */ /* Long RLE */
	    head[cnt++] = (1<<maxGamma); /* Short/Long RLE */
	    /* head[cnt++] = (2<<maxGamma)-1; */ /* EOF (maxGammaValue) */

	    head[cnt++] = extraLZPosBits;

	    head[cnt++] = (exec & 0xff);
	    head[cnt++] = (exec >> 8);

	    head[cnt++] = rleUsed;
	    for(i = 1; i <= rleUsed; i++) {
		head[cnt++] = rleValues[i];
	    }

	    fwrite(head, 1, cnt, fp);
	    fwrite(data, size, 1, fp);
	    if(fp != stdout)
		fclose(fp);
	    return 0;
	}
	fprintf(stderr, "Could not open %s for writing\n", target);
	return 10;
    }
    if ((memStart & 0xff) != 1) {
	fprintf(stderr, "Misaligned basic start 0x%04x\n", memStart);
	return 10;
    } else if (memStart > 9999) {
	/* The basic line only holds 4 digits.. */
	fprintf(stderr, "Too high basic start 0x%04x\n", memStart);
	return 10;
    }

    if (endAddr > memEnd) {
	overlap = endAddr - memEnd;
	endAddr = memEnd;

	/*
	    Make the decrunch code wrap from $ffff to $004b.
	    The decrunch code first copies the data that would exceed
	    $ffff to $004b and then copy the rest of it to end at $ffff.
	 */

	if (overlap > 22) {
	    fprintf(stderr, "Warning: data overlap is %d, but only 22 "
		    "is totally safe!\n", overlap);
	    fprintf(stderr, "The data from $61 to $%02x is overwritten.\n",
		    0x4b + overlap);
	}
    }
    if (overlap) {
	type |= FIXF_WRAP;
    } else {
	type &= ~FIXF_WRAP;
    }
    dc = BestMatch(type);
    if (!dc) {
	fprintf(stderr, "No matching decompressor found\n");
	return 10;
    }
    header = dc->code;

    if (!memStart)
	memStart = 0x801;
#ifndef BIG
    if (memStart + dc->codeSize - 2 + size > 0xfe00) {
	fprintf(stderr, "Packed file's max size is 0x%04x (0x%04x)!\n",
		0xfe00-memStart-(dc->codeSize-2), size);
	return 10;
    }
#endif /* BIG */

    for (i=0; dc->fixes[i].type != ftEnd; i++) {
	switch (dc->fixes[i].type) {
	case ftFastDisable:
	    if (!enable2MHz) {
		header[dc->fixes[i].offset] = 0x2c;
	    }
	    break;
	case ftOverlap:
	    header[dc->fixes[i].offset] = overlap ? (overlap-1) : 0;
	    break;
	case ftOverlapLo:
	    header[dc->fixes[i].offset] =
		(memStart+dc->codeSize-2+rleUsed-15+size - overlap) & 0xff;
	    break;
	case ftOverlapHi:
	    header[dc->fixes[i].offset] =
		(memStart+dc->codeSize-2+rleUsed-15+size - overlap) >> 8;
	    break;
	case ftWrapCount:
	    header[dc->fixes[i].offset] =
		(memEnd>>8) - ((endAddr + overlap - size) >> 8); /* wrap point.. */
	    break;

	case ftSizePages:
	    header[dc->fixes[i].offset] = (size>>8) + 1;
	    break;
	case ftSizeLo:
	    header[dc->fixes[i].offset] =
		(memStart+dc->codeSize-2+rleUsed-15+size-0x100 - overlap) & 0xff;
	    break;
	case ftSizeHi:
	    header[dc->fixes[i].offset] =
		(memStart+dc->codeSize-2+rleUsed-15+size-0x100 - overlap) >> 8;
	    break;
	case ftEndLo:
	    header[dc->fixes[i].offset] = (endAddr - 0x100) & 0xff;
	    break;
	case ftEndHi:
	    header[dc->fixes[i].offset] = ((endAddr - 0x100) >> 8);
	    break;
	case ftEscValue:
	    header[dc->fixes[i].offset] = (escape>>(8-escBits));
	    break;
	case ftOutposLo:
	    header[dc->fixes[i].offset] = (start & 0xff);	/* OUTPOS */
	    break;
	case ftOutposHi:
	    header[dc->fixes[i].offset] = (start >> 8);
	    break;
	case ftEscBits:
	    header[dc->fixes[i].offset] = escBits;
	    break;
	case ftEsc8Bits:
	    header[dc->fixes[i].offset] = 8-escBits;
	    break;
	case ft1MaxGamma:
	    header[dc->fixes[i].offset] = (1<<maxGamma); /* Short/Long RLE */
	    break;
	case ft8MaxGamma:
	    header[dc->fixes[i].offset] = (8-maxGamma); /* Long RLE */
	    break;
	case ft2MaxGamma:
	    header[dc->fixes[i].offset] = (2<<maxGamma)-1; /* EOF (maxGammaValue) */
	    break;
	case ftExtraBits:
	    header[dc->fixes[i].offset] = extraLZPosBits;
	    break;
	case ftMemConfig:
	    header[dc->fixes[i].offset] = memConfig;
	    break;
	case ftCli:
	    header[dc->fixes[i].offset] = intConfig; /* $58/$78 cli/sei; */
	    break;
	case ftExecLo:
	    header[dc->fixes[i].offset] = (exec & 0xff);
	    break;
	case ftExecHi:
	    header[dc->fixes[i].offset] = (exec >> 8);
	    break;
	case ftInposLo:
	    header[dc->fixes[i].offset] = (endAddr + overlap - size) & 0xff;	/* INPOS */
	    break;
	case ftInposHi:
	    header[dc->fixes[i].offset] = ((endAddr + overlap - size) >> 8);
	    break;
	case ftMaxGamma:
	    header[dc->fixes[i].offset] = maxGamma + 1;
	    break;
	case ftReloc:
	    if (header[1] != (memStart>>8)) {
		header[dc->fixes[i].offset] -= (header[1] - (memStart >> 8));
	    }
	    break;

	case ftBEndLo:
	    header[dc->fixes[i].offset] = (progEnd & 0xff);
	    break;
	case ftBEndHi:
	    header[dc->fixes[i].offset] = (progEnd >> 8);
	    break;

	case ftStackSize:
	    stackUsed = header[dc->fixes[i].offset];
	    break;
	case ftIBufferSize:
	    ibufferUsed = header[dc->fixes[i].offset];
	    break;

	default:
	    break;
	}
    }

    for (i=1; i<=15; i++)
	header[dc->codeSize - 15 + i-1] = rleValues[i];

    if (header[1] != (memStart>>8)) {
	header[1] = (memStart>>8);	/* Load address */
	header[3] = (memStart>>8);	/* Line link */

	header[7] = 0x30 + (memStart+12)/1000;
	header[8] = 0x30 + ((memStart+12)/100 % 10);
	header[9] = 0x30 + ((memStart+12)/10 % 10);
	header[10] = 0x30 + ((memStart+12) % 10);
    }

    fprintf(stderr, "Saving %s\n", dc->name);
    if (fp || (fp = fopen(target, "wb"))) {
	fwrite(header, 1, dc->codeSize+rleUsed-15, fp);
	fwrite(data, size, 1, fp);
	if (fp != stdout)
	    fclose(fp);
    } else {
	fprintf(stderr, "Could not open %s for writing\n", target);
	return 10;
    }
    if (dc->flags & FIXF_SHORT) {
	fprintf(stderr, "%s uses the memory $2d-$30, ", target?target:"");
    } else {
	fprintf(stderr, "%s uses the memory $2d/$2e, ", target?target:"");
    }
    if (overlap)
	fprintf(stderr, "$4b-$%02x, ", 0x4b + overlap);
    else if ((dc->flags & FIXF_WRAP))
	fprintf(stderr, "$4b, ");
    if (stackUsed)
	fprintf(stderr, "$f7-$%x, ", 0xf7 + stackUsed);
    if (ibufferUsed)
	fprintf(stderr, "$200-$%x, ", 0x200 + ibufferUsed);
    fprintf(stderr, "and $%04x-$%04x.\n",
	    (start < memStart+1) ? start : memStart+1, endAddr-1);
    return 0;
}



#ifdef ENABLE_VERBOSE
#define F_VERBOSE (1<<0)
#endif
#define F_STATS   (1<<1)
#define F_AUTO    (1<<2)
#define F_NOOPT   (1<<3)
#define F_AUTOEX  (1<<4)
#define F_SKIP    (1<<5)
#define F_2MHZ    (1<<6)
#define F_AVOID   (1<<7)
#define F_DELTA   (1<<8)

#define F_NORLE   (1<<9)

#define F_UNPACK  (1<<14)
#define F_ERROR   (1<<15)

#ifndef min
#define min(a,b) ((a<b)?(a):(b))
#endif


#define LRANGE		(((2<<maxGamma)-3)*256)	/* 0..125, 126 -> 1..127 */
#define MAXLZLEN	(2<<maxGamma)
#define MAXRLELEN	(((2<<maxGamma)-2)*256)	/* 0..126 -> 1..127 */
#define DEFAULT_LZLEN	LRANGE

static int lrange, maxlzlen, maxrlelen;



#ifdef BIG
#define OUT_SIZE 2000000
#else
#define OUT_SIZE 65536
#endif /* BIG */
static unsigned char outBuffer[OUT_SIZE];
static int outPointer = 0;
static int bitMask = 0x80;


static void FlushBits(void) {
    if (bitMask != 0x80)
	outPointer++;
}


static void PutBit(int bit) {
    if (bit && outPointer < OUT_SIZE)
	outBuffer[outPointer] |= bitMask;
    bitMask >>= 1;
    if (!bitMask) {
	bitMask = 0x80;
	outPointer++;
    }
}


void PutValue(int value) {
    int bits = 0, count = 0;

    while (value>1) {
	bits = (bits<<1) | (value & 1);	/* is reversed compared to value */
	value >>= 1;
	count++;
	PutBit(1);
    }
    if (count<maxGamma)
	PutBit(0);
    while (count--) {
	PutBit((bits & 1));	/* output is reversed again -> same as value */
	bits >>= 1;
    }
}

#if 0
int LenValue(int value) {
    int count = 0;

    while (value>1) {
	value >>= 1;
	count += 2;
    }
    if (count<maxGamma)
	return count + 1;
    return count;
}
void InitValueLen(void) {
}
#else
int RealLenValue(int value) {
    int count = 0;

    if (value<2)	/* 1 */
	count = 0;
    else if (value<4)	/* 2-3 */
	count = 1;
    else if (value<8)	/* 4-7 */
	count = 2;
    else if (value<16)	/* 8-15 */
	count = 3;
    else if (value<32)	/* 16-31 */
	count = 4;
    else if (value<64)	/* 32-63 */
	count = 5;
    else if (value<128)	/* 64-127 */
	count = 6;
    else if (value<256)	/* 128-255 */
	count = 7;

    if (count<maxGamma)
	return 2*count + 1;
    return 2*count;
}
static int lenValue[256];
void InitValueLen(void);
void InitValueLen() {
    int i;
    for (i=1; i<256; i++)
	lenValue[i] = RealLenValue(i);
}
#define LenValue(a) (lenValue[a])

#endif


void PutNBits(int byte, int bits) {
    while (bits--)
	PutBit((byte & (1<<bits)));
}


static int gainedEscaped = 0;
static int gainedRle = 0, gainedSRle = 0, gainedLRle = 0;
static int gainedLz = 0, gainedRlecode = 0;

#ifdef DELTA
static int gainedDLz = 0, timesDLz = 0;
#endif

static int timesEscaped = 0, timesNormal = 0;
static int timesRle = 0, timesSRle = 0, timesLRle = 0;
static int timesLz = 0;

static int lenStat[8][4];


int OutputNormal(int *esc, unsigned char *data, int newesc) {
    timesNormal++;
    if ((data[0] & escMask) == *esc) {
	PutNBits((*esc>>(8-escBits)), escBits);	/* escBits>=0 */
	PutValue(2-1);
	PutBit(1);
	PutBit(0);

#if 0
	*esc = (*esc + (1<<(8-escBits))) & escMask;
	PutNBits(data[0], 8-escBits);

	gainedEscaped += 3;
#else
	*esc = newesc;
	PutNBits((*esc>>(8-escBits)), escBits);	/* escBits>=0 */
	PutNBits(data[0], 8-escBits);

	gainedEscaped += escBits + 3;
#endif
	timesEscaped++;
	return 1;
    }
    PutNBits(data[0], 8);
    return 0;
}



void OutputEof(int *esc);
void OutputEof(int *esc) {
    /* EOF marker */
    PutNBits((*esc>>(8-escBits)), escBits);	/* escBits>=0 */
    PutValue(3-1);	/* >1 */
    PutValue((2<<maxGamma)-1);	/* Maximum value */

    /* flush */
    FlushBits();
}


static unsigned char rleValues[32] = {1,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
				0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0};
static int rleHist[256];

void PutRleByte(int data) {
    int index;

    for (index = 1; index < 16/*32*/; index++) {
	if (data == rleValues[index]) {
	    if (index==1)
		lenStat[0][3]++;
	    else if (index<=3)
		lenStat[1][3]++;
	    else if (index<=7)
		lenStat[2][3]++;
	    else if (index<=15)
		lenStat[3][3]++;
	    /*else if (index<=31)
		lenStat[4][3]++;*/

	    gainedRlecode += 8 - LenValue(index);

	    PutValue(index);
	    return;
	}
    }
/*fprintf(stderr, "RLECode n: 0x%02x\n", data);*/
    PutValue(16/*32*/ + (data>>4/*3*/));

    gainedRlecode -= LenValue(16/*32*/+(data>>4/*3*/)) + 4/*3*/;

    PutNBits(data, 4/*3*/);

    lenStat[4/*5*/][3]++;
    /* Note: values 64..127 are not used if maxGamma>5 */
}


#if 0
int LenRleByte(unsigned char data) {
    int index;

    for (index = 1; index < 16/*32*/; index++) {
	if (data == rleValues[index]) {
	    return LenValue(index);
	}
    }
    return LenValue(16/*32*/ + 0) + 4/*3*/;
}
#else
static unsigned char rleLen[256];
void InitRleLen(void);
void InitRleLen() {
    int i;

    for (i=0; i<256; i++)
	rleLen[i] = LenValue(16/*32*/ + 0) + 4/*3*/;
    for (i=1; i<16 /*32*/; i++)
	rleLen[rleValues[i]] = LenValue(i);
}
#define LenRleByte(d) (rleLen[d])
#endif


int LenRle(int len, int data) {
    int out = 0;

    do {
	if (len == 1) {
	    out += escBits + 3 + 8;
	    len = 0;
	} else if (len <= (1<<maxGamma)) {
	    out += escBits + 3 + LenValue(len-1) + LenRleByte(data);
	    len = 0;
	} else {
	    int tmp = min(len, maxrlelen);
	    out += escBits + 3 + maxGamma + 8 +
			LenValue(((tmp-1)>>8)+1) + LenRleByte(data);

	    len -= tmp;
	}
    } while (len);
    return out;
}

int OutputRle(int *esc, unsigned char *data, int rlelen) {
    int len = rlelen, tmp;

    while (len) {
	if (len >= 2 && len <= (1<<maxGamma)) {
	    /* Short RLE */
	    if (len==2)
		lenStat[0][2]++;
	    else if (len<=4)
		lenStat[1][2]++;
	    else if (len<=8)
		lenStat[2][2]++;
	    else if (len<=16)
		lenStat[3][2]++;
	    else if (len<=32)
		lenStat[4][2]++;
	    else if (len<=64)
		lenStat[5][2]++;
	    else if (len<=128)
		lenStat[6][2]++;
	    else if (len<=256)
		lenStat[6][2]++;

	    PutNBits((*esc>>(8-escBits)), escBits);	/* escBits>=0 */
	    PutValue(2-1);
	    PutBit(1);
	    PutBit(1);
	    PutValue(len-1);
	    PutRleByte(*data);

	    tmp = 8*len -escBits -3 -LenValue(len-1) -LenRleByte(*data);
	    gainedRle += tmp;
	    gainedSRle += tmp;

	    timesRle++;
	    timesSRle++;
	    return 0;
	}
	if (len<3) {
	    while (len--)
		OutputNormal(esc, data, *esc);
	    return 0;
	}

	if (len <= maxrlelen) {
	    /* Run-length encoding */
	    PutNBits((*esc>>(8-escBits)), escBits);	/* escBits>=0 */

	    PutValue(2-1);
	    PutBit(1);
	    PutBit(1);

	    PutValue((1<<maxGamma) + (((len-1)&0xff)>>(8-maxGamma)));

	    PutNBits((len-1), 8-maxGamma);
	    PutValue(((len-1)>>8) + 1);
	    PutRleByte(*data);

	    tmp = 8*len -escBits -3 -maxGamma -8 -LenValue(((len-1)>>8)+1)
		-LenRleByte(*data);
	    gainedRle += tmp;
	    gainedLRle += tmp;

	    timesRle++;
	    timesLRle++;
	    return 0;
	}

	/* Run-length encoding */
	PutNBits((*esc>>(8-escBits)), escBits);	/* escBits>=0 */

	PutValue(2-1);
	PutBit(1);
	PutBit(1);

	PutValue((1<<maxGamma) + (((maxrlelen-1)&0xff)>>(8-maxGamma)));

	PutNBits((maxrlelen-1) & 0xff, 8-maxGamma);
	PutValue(((maxrlelen-1)>>8)+1);
	PutRleByte(*data);

	tmp = 8*maxrlelen -escBits -3 -maxGamma -8
	    -LenValue(((maxrlelen-1)>>8)+1) -LenRleByte(*data);
	gainedRle += tmp;
	gainedLRle += tmp;
	timesRle++;
	timesLRle++;
	len -= maxrlelen;
	data += maxrlelen;
    }
    return 0;
}


#ifdef DELTA
/*    e..e	101  (4..)	111111 111111	8b(add) 8b(POSLO)	DLZ*/
static int LenDLz(int lzlen, int lzpos) {
    return escBits + 2*maxGamma + 8 + 8 + LenValue(lzlen-1);
}

static int OutputDLz(int *esc, int lzlen, int lzpos, int add) {
    PutNBits((*esc>>(8-escBits)), escBits);	/* escBits>=0 */

    PutValue(lzlen-1);
    PutValue((2<<maxGamma)-1);	/* Maximum value */
    PutNBits(add, 8);
    PutNBits(((lzpos-1) & 0xff) ^ 0xff, 8);

    gainedDLz += 8*lzlen -(escBits + LenValue(lzlen-1) + 2*maxGamma + 16);
    timesDLz++;
    return 4;
}
#endif


static int LenLz(int lzlen, int lzpos) {
    if (lzlen==2) {
#if 0
	if (lzpos <= 16)
	    return escBits + 2 + 5;
	if (lzpos <= 128)
	    return escBits + 2 + 8;
#else
	if (lzpos <= 256)
	    return escBits + 2 + 8;
#endif
	return 100000;
    }

    return escBits + 8 + extraLZPosBits +
		LenValue(((lzpos-1)>>(8+extraLZPosBits))+1) +
		LenValue(lzlen-1);
}


static int OutputLz(int *esc, int lzlen, int lzpos, char *data, int curpos) {
    if (lzlen==2)
	lenStat[0][1]++;
    else if (lzlen<=4)
	lenStat[1][1]++;
    else if (lzlen<=8)
	lenStat[2][1]++;
    else if (lzlen<=16)
	lenStat[3][1]++;
    else if (lzlen<=32)
	lenStat[4][1]++;
    else if (lzlen<=64)
	lenStat[5][1]++;
    else if (lzlen<=128)
	lenStat[6][1]++;
    else if (lzlen<=256)
	lenStat[7][1]++;

    if (lzlen >= 2 && lzlen <= maxlzlen) {
	int tmp;

	PutNBits((*esc>>(8-escBits)), escBits);	/* escBits>=0 */

	tmp = ((lzpos-1)>>(8+extraLZPosBits))+2;
	if (tmp==2)
	    lenStat[0][0]++;
	else if (tmp<=4)
	    lenStat[1][0]++;
	else if (tmp<=8)
	    lenStat[2][0]++;
	else if (tmp<=16)
	    lenStat[3][0]++;
	else if (tmp<=32)
	    lenStat[4][0]++;
	else if (tmp<=64)
	    lenStat[5][0]++;
	else if (tmp<=128)
	    lenStat[6][0]++;
	else if (tmp<=256)
	    lenStat[6][0]++;

	if (lzlen==2) {
	    PutValue(lzlen-1);
	    PutBit(0);
	    if (lzpos > 256)
		fprintf(stderr,
			"Error at %d: lzpos too long (%d) for lzlen==2\n",
			curpos, lzpos);
#if 0
	    if (lzpos <= 16) {
		PutBit(0);
		PutNBits(((lzpos-1) & 0xff) ^ 0xff, 4);
	    } else {
		PutBit(1);
		PutNBits(((lzpos-1) & 0xff) ^ 0xff, 8);
	    }
#else
	    PutNBits(((lzpos-1) & 0xff) ^ 0xff, 8);
#endif
	} else {
	    PutValue(lzlen-1);
	    PutValue( ((lzpos-1) >> (8+extraLZPosBits)) +1);
	    PutNBits( ((lzpos-1) >> 8), extraLZPosBits);
	    PutNBits(((lzpos-1) & 0xff) ^ 0xff, 8);
	}

	gainedLz += 8*lzlen -LenLz(lzlen, lzpos);
	timesLz++;
	return 3;
    }
    fprintf(stderr, "Error: lzlen too short/long (%d)\n", lzlen);
    return lzlen;
}



static unsigned short *rle, *elr, *lzlen, *lzpos, *lzmlen, *lzmpos;
#ifdef DELTA
static unsigned short *lzlen2, *lzpos2;
#endif
static int *length, inlen;
static unsigned char *indata, *mode, *newesc;
unsigned short *backSkip;


enum MODE {
    LITERAL = 0,
    LZ77 = 1,
    RLE = 2,
#ifdef DELTA
    DLZ = 3,
#endif
    MMARK = 4
};

static int lzopt = 0;
/* Non-recursive version */
/* NOTE! IMPORTANT! the "length" array length must be inlen+1 */

int OptimizeLength(int optimize) {
    int i;

    length[inlen] = 0;		/* one off the end, our 'target' */
    for (i=inlen-1; i>=0; i--) {
    	int r1 = 8 + length[i+1], r2, r3;

	if (!lzlen[i] && !rle[i]
#ifdef DELTA
		&& (!lzlen2 || !lzlen2[i])
#endif
	) {
	    length[i] = r1;
	    mode[i] = LITERAL;
	    continue;
	}

	/* If rle>maxlzlen, skip to the start of the rle-maxlzlen.. */
	if (rle[i] > maxlzlen && elr[i] > 1) {
	    int z = elr[i];

	    i -= elr[i];

	    r2 = LenRle(rle[i], indata[i]) + length[i+ rle[i]];
	    if (optimize) {
		int ii, mini = rle[i], minv = r2;

		int bot = rle[i] - (1<<maxGamma);
		if (bot < 2)
		    bot = 2;

		for (ii=mini-1; ii>=bot; ii--) {
		    int v = LenRle(ii, indata[i]) + length[i + ii];
		    if (v < minv) {
			minv = v;
			mini = ii;
		    }
		}
		if (minv != r2) {
		    lzopt += r2 - minv;
		    rle[i] = mini;
		    r2 = minv;
		}
	    }
	    length[i] = r2;
	    mode[i] = RLE;

	    for (; z>=0; z--) {
		length[i+z] = r2;
		mode[i+z] = RLE;
	    }
	    continue;
	}
	r3 = r2 = r1 + 1000; /* r3 >= r2 > r1 */

	if (rle[i]) {
	    r2 = LenRle(rle[i], indata[i]) + length[i+ rle[i]];

	    if (optimize) {
		int ii, mini = rle[i], minv = r2;

#if 0
		int bot = rle[i] - (1<<maxGamma);
		if (bot < 2)
		    bot = 2;

		for (ii=mini-1; ii>=bot; ii--) {
		    int v = LenRle(ii, indata[i]) + length[i + ii];
		    if (v < minv) {
			minv = v;
			mini = ii;
		    }
		}
#else
		/* Check only the original length and all shorter
		   lengths that are power of two.

		   Does not really miss many 'minimums' this way,
		   at least not globally..

		   Makes the assumption that the Elias Gamma Code is
		   used, i.e. values of the form 2^n are 'optimal' */
		ii = 2;
		while (rle[i] > ii) {
		    int v = LenRle(ii, indata[i]) + length[i + ii];
		    if (v < minv) {
			minv = v;
			mini = ii;
		    }
		    ii <<= 1;
		}
#endif
		if (minv != r2) {
/*printf("%05d RL %d %d\n", i, rle[i], mini);*/
		    lzopt += r2 - minv;
		    rle[i] = mini;
		    r2 = minv;
		}
	    }
	}
	if (lzlen[i]) {
	    r3 = LenLz(lzlen[i], lzpos[i]) + length[i + lzlen[i]];

	    if (optimize && lzlen[i]>2) {
		int ii, mini = lzlen[i], minv = r3, mino = lzpos[i];
		int topLen = LenLz(lzlen[i], lzpos[i])
		    - LenValue(lzlen[i]-1);

#if 0
		int bot = 3;
		if (lzpos[i] <= 256)
		    bot = 2;

		for (ii=mini-1; ii>=bot; ii--) {
		    int v = topLen + LenValue(ii-1) + length[i + ii];
		    if (v < minv) {
			minv = v;
			mini = ii;
		    }
		}
#else
		/* Check only the original length and all shorter
		   lengths that are power of two.

		   Does not really miss many 'minimums' this way,
		   at least not globally..

		   Makes the assumption that the Elias Gamma Code is
		   used, i.e. values of the form 2^n are 'optimal' */
		ii = 4;
		while (lzlen[i] > ii) {
		    int v = topLen + LenValue(ii-1) + length[i + ii];
		    if (v < minv) {
			minv = v;
			mini = ii;
		    }
		    ii <<= 1;
		}

		/* Then check the max lengths we have found, but
		   did not originally approve because they seemed
		   to gain less than the shorter, nearer matches. */
		ii = 3;
		while (lzmlen[i] >= ii) {
		    int v = LenLz(ii, lzmpos[i]) + length[i + ii];
		    if (v < minv) {
			minv = v;
			mini = ii;
			mino = lzmpos[i];
		    }
		    ii++;
		}
#endif
#ifdef BACKSKIP_FULL
		/*
		  Note:
		  2-byte optimization checks are no longer done
		  with the rest, because the equation gives too long
		  code lengths for 2-byte matches if extraLzPosBits>0.
		  */
		/* Two-byte rescan/check */
		if (backSkip[i] && backSkip[i] <= 256) {
		    /* There are previous occurrances (near enough) */
		    int v = LenLz(2, (int)backSkip[i]) + length[i + 2];

		    if (v < minv) {
			minv = v;
			mini = 2;
			lzlen[i] = mini;
			r3 = minv;
			lzpos[i] = (int)backSkip[i];
		    }
		}
#endif /* BACKSKIP_FULL */
		if (minv != r3 && minv < r2) {
                    /*
		      printf("@%05d LZ %d %4x -> %d %4x\n",
		      i, lzlen[i], lzpos[i], mini, lzpos[i]);
		      */
		    lzopt += r3 - minv;
		    lzlen[i] = mini;
		    lzpos[i] = mino;
		    r3 = minv;
		}
	    }
	}

	if (r2 <= r1) {
	    if (r2 <= r3) {
		length[i] = r2;
		mode[i] = RLE;
	    } else {
		length[i] = r3;
		mode[i] = LZ77;
	    }
	} else {
	    if (r3 <= r1) {
		length[i] = r3;
		mode[i] = LZ77;
	    } else {
		length[i] = r1;
		mode[i] = LITERAL;
	    }
	}
#ifdef DELTA
	if (lzlen2 && lzlen2[i] > 3) {
	    r3 = LenDLz(lzlen2[i], lzpos2[i]) + length[i + lzlen2[i]];
	    if (r3 < length[i]) {
		length[i] = r3;
		mode[i] = DLZ;
	    }
	}
#endif
    }
    return length[0];
}


/*
    The algorithm in the OptimizeEscape() works as follows:
    1) Only unpacked bytes are processed, they are marked
       with MMARK. We proceed from the end to the beginning.
       Variable A (old/new length) is updated.
    2) At each unpacked byte, one and only one possible
       escape matches. A new escape code must be selected
       for this case. The optimal selection is the one which
       provides the shortest number of escapes to the end
       of the file,
	i.e. A[esc] = 1+min(A[0], A[1], .. A[states-1]).
       For other states A[esc] = A[esc];
       If we change escape in this byte, the new escape is
       the one with the smallest value in A.
    3) The starting escape is selected from the possibilities
       and mode 0 is restored to all mode 3 locations.

 */

int OptimizeEscape(int *startEscape, int *nonNormal) {
    int i, j, states = (1<<escBits);
    long minp = 0, minv = 0, other = 0;
    long a[256]; /* needs int/long */
    long b[256]; /* Remembers the # of escaped for each */
    int esc8 = 8-escBits;

    for (i=0; i<256; i++)
	b[i] = a[i] = -1;

    if (states>256) {
	fprintf(stderr, "Escape optimize: only 256 states (%d)!\n",
		states);
	return 0;
    }

    /* Mark those bytes that are actually outputted */
    for (i=0; i<inlen; ) {
	switch (mode[i]) {
#ifdef DELTA
	case DLZ:
	    other++;
	    i += lzlen2[i];
	    break;
#endif

	case LZ77:
	    other++;
	    i += lzlen[i];
	    break;

	case RLE:
	    other++;
	    i += rle[i];
	    break;

	/*case LITERAL:*/
	default:
	    mode[i++] = MMARK; /* mark it used so we can identify it */
	    break;
	}
    }

    for (i=inlen-1; i>=0; i--) {
	/* Using a table to skip non-normal bytes does not help.. */
	if (mode[i] == MMARK) {
	    int k = (indata[i] >> esc8);

	    /* Change the tag values back to normal */
	    mode[i] = LITERAL;

	    /*
		k are the matching bytes,
		minv is the minimum value,
		minp is the minimum index
	     */

	    newesc[i] = (minp << esc8);
	    a[k] = minv + 1;
	    b[k] = b[minp] + 1;
	    if (k==minp) {
		/* Minimum changed -> need to find a new minimum */
		/* a[k] may still be the minimum */
		minv++;
		for (k=states-1; k>=0; k--) {
		    if (a[k] < minv) {
			minv = a[k];
			minp = k;
			/*
			    There may be others, but the first one that
			    is smaller than the old minimum is equal to
			    any other new minimum.
			 */
			break;
		    }
		}
	    }
	}
    }

    /* Select the best value for the initial escape */
    if (startEscape) {
	i = inlen;	/* make it big enough */
	for (j=states-1; j>=0; j--) {
	    if (a[j] <= i) {
		*startEscape = (j << esc8);
		i = a[j];
	    }
	}
    }
    if (nonNormal)
	*nonNormal = other;
    return b[startEscape ? (*startEscape>>esc8) : 0];
}


/* Initialize the RLE byte code table according to all RLE's found so far */
/* O(n) */
void InitRle(int);
void InitRle(int flags) {
    int p, mr, mv, i;

    for (i=1; i<16/*32*/; i++) {
	mr = -1;
	mv = 0;

	for (p=0; p<256; p++) {
	    if (rleHist[p] > mv) {
		mv = rleHist[p];
		mr = p;
	    }
	}
	if (mv>0) {
	    rleValues[i] = mr;
	    rleHist[mr] = -1;
	} else
	    break;
    }
    InitRleLen();
}


/* Initialize the RLE byte code table according to RLE's actually used */
/* O(n) */
void OptimizeRle(int);
void OptimizeRle(int flags) {
    int p, mr, mv, i;

    if ((flags & F_NORLE)) {
	rleUsed = 0;
	return;
    }
    if ((flags & F_STATS))
	fprintf(stderr, "RLE Byte Code Re-Tune, RLE Ranks:\n");
    for (p=0; p<256; p++)
	rleHist[p] = 0;

    for (p=0; p<inlen; ) {
	switch (mode[p]) {
#ifdef DELTA
	case DLZ: /* lz */
	    p += lzlen2[p];
	    break;
#endif
	case LZ77: /* lz */
	    p += lzlen[p];
	    break;

	case RLE: /* rle */
	    rleHist[indata[p]]++;
	    p += rle[p];
	    break;

    /*  case LITERAL:
	case MMARK:*/
	default:
	    p++;
	    break;
	}
    }

    for (i=1; i<16 /*32*/; i++) {
	mr = -1;
	mv = 0;

	for (p=0; p<256; p++) {
	    if (rleHist[p] > mv) {
		mv = rleHist[p];
		mr = p;
	    }
	}
	if (mv>0) {
	    rleValues[i] = mr;
	    if ((flags & F_STATS)) {
		fprintf(stderr, " %2d.0x%02x %-3d ", i, mr, mv);
		if (!((i - 1) % 6))
		    fprintf(stderr, "\n");
	    }
	    rleHist[mr] = -1;
	} else
	    break;
    }
    rleUsed = i-1;

    if ((flags & F_STATS))
	if (((i - 1) % 6)!=1)
	    fprintf(stderr, "\n");
    InitRleLen();
}


static const unsigned char *up_Data;
static int up_Mask, up_Byte;
void up_SetInput(const unsigned char *data) {
    up_Data = data;
    up_Mask = 0x80;
    up_Byte = 0;
}
int up_GetBits(int bits) {
    int val = 0;

    while (bits--) {
	val <<= 1;
	if ((*up_Data & up_Mask))
	   val |= 1;
	up_Mask >>= 1;
	if (!up_Mask) {
	    up_Mask = 0x80;
	    up_Data++;
	    up_Byte++;
	}
    }
    return val;
}
int up_GetValue(void) {
    int i = 0;

    while (i<maxGamma) {
	if (!up_GetBits(1))
	    break;
	i++;
    }
    return (1<<i) | up_GetBits(i);
}


int UnPack(int loadAddr, const unsigned char *data, const char *file,
	   int flags) {
    long size, startEsc, endAddr, execAddr, headerSize;
    long startAddr, error = 0;
    FILE *fp;
    int i, overlap;
    long timeused = clock();
    const char *byteCodeVec;
#define MAXCODES 20
    int mismatch[MAXCODES], collect[ftEnd];
    struct FixStruct *dc;

    /* Search for the right code */
    if (data[0] == 'p' && data[1] == 'u') {
	/* was saved without decompressor */
	int cnt = 2;

	endAddr = (data[cnt] | (data[cnt+1]<<8)) + 0x100;
	cnt += 2;

	startEsc = data[cnt++];
	startAddr = data[cnt] | (data[cnt+1] << 8);
	cnt += 2;

	escBits = data[cnt++];
	if (escBits < 0 || escBits > 8) {
	    fprintf(stderr, "Error: Broken archive, escBits %d.\n",
		    escBits);
	    return 20;
	}
	maxGamma = data[cnt++] - 1;
	if (data[cnt++] != (1<<maxGamma) ||
	   maxGamma < 5 || maxGamma > 7) {
	    fprintf(stderr, "Error: Broken archive, maxGamma %d.\n",
		    maxGamma);
	    return 20;
	}
    lrange = LRANGE;
    maxlzlen = MAXLZLEN;
    maxrlelen = MAXRLELEN;

	extraLZPosBits = data[cnt++];
	if (extraLZPosBits < 0 || extraLZPosBits > 4) {
	    fprintf(stderr, "Error: Broken archive, extraLZPosBits %d.\n",
		    extraLZPosBits);
	    return 20;
	}

	execAddr = data[cnt] | (data[cnt+1]<<8);
	cnt += 2;

	rleUsed = data[cnt++];
	byteCodeVec = &data[cnt - 1];

	overlap = 0;
	memConfig = memConfig;
	intConfig = intConfig;

	size = endAddr-startAddr;
	headerSize = cnt + rleUsed;

	endAddr = loadAddr + size;

    } else {

	for (i=0; fixStruct[i].code && i < MAXCODES; i++) {
	    int j, maxDiff = 0;

	    if (fixStruct[i].code[1] != (loadAddr>>8))
		maxDiff = 5;
	    for (j=0; fixStruct[i].fixes[j].type != ftEnd; j++) {
		maxDiff++;
	    }
	    mismatch[i] = 0;
	    for (j=2; j<fixStruct[i].codeSize-15; j++) {
		if (fixStruct[i].code[j] != data[j-2])
		    mismatch[i]++;
	    }
	    if (mismatch[i] <= maxDiff) {
		fprintf(stderr, "Detected %s (%d <= %d)\n",
			fixStruct[i].name, mismatch[i], maxDiff);
		break;
	    }
	    fprintf(stderr, "Not %s (%d > %d)\n",
		    fixStruct[i].name, mismatch[i], maxDiff);
	}
	dc = &fixStruct[i];
	if (!dc->code) {
	    fprintf(stderr,
		    "Error: The file is not compressed with this program.\n");
	    return 20;
	}

	if ((loadAddr & 0xff) != 1) {
	    fprintf(stderr, "Error: Misaligned basic start address 0x%04x\n",
		    loadAddr);
	    return 20;
	}
	/* TODO: check that the decrunch code and load address match. */

	error = 0;

	for (i=0; i<ftEnd; i++) {
	    collect[i] = 0;
	}
	collect[ftMemConfig] = memConfig;
	collect[ftCli] = intConfig;
	for (i=0; dc->fixes[i].type!=ftEnd; i++) {
	    collect[dc->fixes[i].type] = data[dc->fixes[i].offset-2];
	}

	overlap = collect[ftOverlap];
	/* TODO: check overlap LO/HI and WrapCount */
	maxGamma = collect[ftMaxGamma] - 1;
	if (maxGamma < 5 || maxGamma > 7) {
	    fprintf(stderr, "Error: Broken archive, maxGamma %d.\n",
		    maxGamma);
	    return 20;
	}
    lrange = LRANGE;
    maxlzlen = MAXLZLEN;
    maxrlelen = MAXRLELEN;

	if (collect[ft1MaxGamma] != (1<<maxGamma) ||
	    collect[ft8MaxGamma] != (8-maxGamma) ||
	    collect[ft2MaxGamma] != (2<<maxGamma)-1) {
	    fprintf(stderr,
		    "Error: Broken archive, maxGamma (%d) mismatch.\n",
		    maxGamma);
	    return 20;
	}

	startEsc = collect[ftEscValue];
	startAddr = collect[ftOutposLo] | (collect[ftOutposHi]<<8);
	escBits = collect[ftEscBits];
	if (escBits < 0 || escBits > 8) {
	    fprintf(stderr, "Error: Broken archive, escBits %d.\n",
		    escBits);
	    return 20;
	}

	if (collect[ftEsc8Bits] != 8-escBits) {
	    fprintf(stderr, "Error: Broken archive, escBits (%d) mismatch.\n",
		    escBits);
	    return 20;
	}

	extraLZPosBits = collect[ftExtraBits];
	if (extraLZPosBits < 0 || extraLZPosBits > 4) {
	    fprintf(stderr, "Error: Broken archive, extraLZPosBits %d.\n",
		    extraLZPosBits);
	    return 20;
	}
	endAddr = 0x100 + (collect[ftEndLo] | (collect[ftEndHi]<<8));
	size    = endAddr - (collect[ftInposLo] | (collect[ftInposHi]<<8));
	headerSize = ((collect[ftSizeLo] | (collect[ftSizeHi]<<8))
			+ 0x100 - size - loadAddr) & 0xffff;
	execAddr = collect[ftExecLo] | (collect[ftExecHi]<<8);

	memConfig = collect[ftMemConfig];
	intConfig = collect[ftCli];
	byteCodeVec = &data[dc->codeSize - 32 -2];

	rleUsed = 15 - dc->codeSize +2 + headerSize;
    }


    if ((flags & F_STATS)) {
	fprintf(stderr,
		"Load 0x%04x, Start 0x%04lx, exec 0x%04lx, %s%s$01=$%02x\n",
		loadAddr, startAddr, execAddr,
		(intConfig==0x58)?"cli, ":"", (intConfig==0x78)?"sei, ":"",
		memConfig);
	fprintf(stderr, "Escape bits %d, starting escape 0x%02lx\n",
		escBits, (startEsc<<(8-escBits)));
	fprintf(stderr,
		"Decompressor size %ld, max length %d, LZPOS LO bits %d\n",
		headerSize+2, (2<<maxGamma), extraLZPosBits+8);
	fprintf(stderr, "rleUsed: %d\n", rleUsed);
    }

    if (rleUsed > 15) {
	fprintf(stderr, "Error: Old archive, rleUsed %d > 15.\n", rleUsed);
	return 20;
    }

    outPointer = 0;
    up_SetInput(data + headerSize);
    while (!error) {
	int sel = startEsc;

#ifndef BIG
	if (startAddr + outPointer >= up_Byte + endAddr - size) {
	    if (!error)
		fprintf(stderr, "Error: Target %5ld exceeds source %5ld..\n",
			startAddr + outPointer, up_Byte + endAddr - size);
	    error++;
	}
	if (up_Byte > size + overlap) {
	    fprintf(stderr, "Error: No EOF symbol found (%d > %d).\n",
		    up_Byte, size + overlap);
	    error++;
	}
#endif /* BIG */

	if (escBits)
	    sel = up_GetBits(escBits);
	if (sel == startEsc) {
	    int lzPos, lzLen = up_GetValue(), i;
#ifdef DELTA
	    int add = 0;
#endif

	    if (lzLen != 1) {
		int lzPosHi = up_GetValue()-1, lzPosLo;

		if (lzPosHi == (2<<maxGamma)-2) {
#ifdef DELTA
/* asm: 25 bytes longer */
		    if (lzLen > 2) {
			add = up_GetBits(8);
			lzPos = up_GetBits(8) ^ 0xff;
		    } else
#endif
		    break; /* EOF */
		} else {
		    if (extraLZPosBits) {
			lzPosHi = (lzPosHi<<extraLZPosBits) |
				    up_GetBits(extraLZPosBits);
		    }
		    lzPosLo = up_GetBits(8) ^ 0xff;
		    lzPos = (lzPosHi<<8) | lzPosLo;
		}
	    } else {
		if (up_GetBits(1)) {
		    int rleLen, byteCode, byte;

		    if (!up_GetBits(1)) {
			int newEsc = up_GetBits(escBits);

			outBuffer[outPointer++] =
			    (startEsc<<(8-escBits)) | up_GetBits(8-escBits);
/*fprintf(stdout, "%5ld %5ld  *%02x\n",
	outPointer-1, up_Byte, outBuffer[outPointer-1]);*/
			startEsc = newEsc;
			if (outPointer >= OUT_SIZE) {
			    fprintf(stderr, "Error: Broken archive, "
				    "output buffer overrun at %d.\n",
				    outPointer);
			    return 20;
			}
			continue;
		    }
		    rleLen = up_GetValue();
		    if (rleLen >= (1<<maxGamma)) {
			rleLen = ((rleLen-(1<<maxGamma))<<(8-maxGamma)) |
			    up_GetBits(8-maxGamma);
			rleLen |= ((up_GetValue()-1)<<8);
		    }
		    byteCode = up_GetValue();
		    if (byteCode < 16/*32*/) {
			byte = byteCodeVec[byteCode];
		    } else {
			byte = ((byteCode-16/*32*/)<<4/*3*/) | up_GetBits(4/*3*/);
		    }

/*fprintf(stdout, "%5ld %5ld RLE %5d 0x%02x\n", outPointer, up_Byte, rleLen+1,
	byte);*/
		    if (outPointer + rleLen + 1 >= OUT_SIZE) {
			fprintf(stderr, "Error: Broken archive, "
				"output buffer overrun at %d.\n",
				OUT_SIZE);
			return 20;
		    }
		    for (i=0; i<=rleLen; i++) {
			outBuffer[outPointer++] = byte;
		    }
		    continue;
		}
		lzPos = up_GetBits(8) ^ 0xff;
	    }
/*fprintf(stdout, "%5ld %5ld LZ %3d 0x%04x\n",
	outPointer, up_Byte, lzLen+1, lzPos+1);*/

	    /* outPointer increases in the loop, thus its minimum is here */
	    if (outPointer - lzPos -1 < 0) {
		fprintf(stderr, "Error: Broken archive, "
			"LZ copy position underrun at %d (%d). "
			"lzLen %d.\n",
			outPointer, lzPos+1, lzLen+1);
		return 20;
	    }
	    if (outPointer + lzLen + 1 >= OUT_SIZE) {
		fprintf(stderr, "Error: Broken archive, "
			"output buffer overrun at %d.\n",
			OUT_SIZE);
		return 20;
	    }
	    for (i=0; i<=lzLen; i++) {
		outBuffer[outPointer] = outBuffer[outPointer - lzPos - 1]
#ifdef DELTA
					DELTA_OP add;
#else
					;
#endif
		outPointer++;
	    }
	} else {
	    int byte = (sel<<(8-escBits)) | up_GetBits(8-escBits);
/*fprintf(stdout, "%5ld %5ld  %02x\n",
	outPointer, up_Byte, byte);*/
	    outBuffer[outPointer++] = byte;
	    if (outPointer >= OUT_SIZE) {
		fprintf(stderr, "Error: Broken archive, "
			"output buffer overrun at %d.\n",
			outPointer);
		return 20;
	    }
	}
    }
    if (error)
	fprintf(stderr, "Error: Target exceeded source %5ld times.\n",
		error);

    if ((file && (fp = fopen(file, "wb"))) || (fp = stdout)) {
	unsigned char tmp[2];
	tmp[0] = startAddr & 0xff;
	tmp[1] = (startAddr >> 8);

	fwrite(tmp, 2, 1, fp);
	fwrite(outBuffer, outPointer, 1, fp);
	if (fp != stdout)
	    fclose(fp);

	timeused = clock() - timeused;
	if (!timeused)
	    timeused++;	/* round upwards */
	fprintf(stderr,
		"Decompressed %d bytes in %4.2f seconds (%4.2f kB/s)\n",
		outPointer,
		(double)timeused/CLOCKS_PER_SEC,
		(double)CLOCKS_PER_SEC*outPointer/timeused/1024.0);

	return error;
    }
    fprintf(stderr, "Could not open file \"%s\" for writing.\n", file);
    return 20;
}



int PackLz77(int lzsz, int flags, int *startEscape,
	     int endAddr, int memEnd, int type) {
    int i, j, outlen, p, headerSize;
    int escape;
#ifdef HASH_COMPARE
    unsigned char *hashValue;
    unsigned char *a;
    int k;
#endif /* HASH_COMPARE */

#ifdef BIG
    unsigned int *lastPair;
#else
    unsigned short *lastPair;
#endif /* BIG */

#ifdef BACKSKIP_FULL
#ifdef RESCAN
    int rescan = 0;
#endif /* RESCAN */
#endif /* BACKSKIP_FULL */

#ifdef HASH_STAT
    unsigned long compares = 0, hashChecks = 0, hashEqual = 0;
#endif /* HASH_STAT */

    if (lzsz < 0 || lzsz > lrange) {
	fprintf(stderr, "LZ range must be from 0 to %d (was %d). Set to %d.\n",
		lrange, lzsz, lrange);
	lzsz = lrange;
    }
    if (lzsz > 65535) {
	fprintf(stderr,
		"LZ range must be from 0 to 65535 (was %d). Set to 65535.\n",
		lzsz);
	lzsz = 65535;
    }
    if (!lzsz)
	fprintf(stderr, "Warning: zero LZ range. Only RLE packing used.\n");

    InitRleLen();
    length = (int *)calloc(sizeof(int), inlen + 1);
    mode   = (unsigned char *)calloc(sizeof(unsigned char), inlen);
    rle    = (unsigned short *)calloc(sizeof(unsigned short), inlen);
    elr    = (unsigned short *)calloc(sizeof(unsigned short), inlen);
    lzlen  = (unsigned short *)calloc(sizeof(unsigned short), inlen);
    lzpos  = (unsigned short *)calloc(sizeof(unsigned short), inlen);
    lzmlen = (unsigned short *)calloc(sizeof(unsigned short), inlen);
    lzmpos = (unsigned short *)calloc(sizeof(unsigned short), inlen);
#ifdef DELTA
    if ((type & FIXF_DLZ)) {
	lzlen2  = (unsigned short *)calloc(sizeof(unsigned short), inlen);
	lzpos2  = (unsigned short *)calloc(sizeof(unsigned short), inlen);
    } else {
	lzlen2 = lzpos2 = NULL;
    }
#endif
    newesc = (unsigned char *)calloc(sizeof(unsigned char), inlen);
#ifdef BACKSKIP_FULL
    backSkip  = (unsigned short *)calloc(sizeof(unsigned short), inlen);
#else
    backSkip  = (unsigned short *)calloc(sizeof(unsigned short), 65536);
#endif /* BACKSKIP_FULL */
#ifdef HASH_COMPARE
    hashValue = (unsigned char *)malloc(inlen);
#endif /* HASH_COMPARE */
#ifdef BIG
    lastPair  = (unsigned int *)calloc(sizeof(unsigned int), 256*256);
#else
    lastPair  = (unsigned short *)calloc(sizeof(unsigned short), 256*256);
#endif /* BIG */


    /* error checking */
    if (!length || !mode || !rle || !elr || !lzlen || !lzpos ||
	!lzmlen || !lzmpos || !newesc || !lastPair || !backSkip
#ifdef DELTA
	|| ((type & FIXF_DLZ) && (!lzlen2 || !lzpos2))
#endif
#ifdef HASH_COMPARE
	|| !hashValue
#endif /* HASH_COMPARE */
	) {
	fprintf(stderr, "Memory allocation failed!\n");
	goto errorexit;
    }

#ifdef HASH_COMPARE
    i = 0;
    j = 0;
    a = indata + inlen;
    for (p=inlen-1; p>=0; p--) {
	k = j;
	j = i;
	i = *--a;	/* Only one read per position */

	/* Without hash: 18.56%, end+middle: 12.68% */
	/* hashValue[p] = i*2 ^ j*3 ^ k*5; */ /* 8.56% */
	/* hashValue[p] = i ^ j*2 ^ k*3; */   /* 8.85% */
	/* hashValue[p] = i + j + k; */       /* 9.33% */
	/* hashValue[p] = i + j*2 + k*3; */   /* 8.25% */
	/* hashValue[p] = i*2 + j*3 + k*5; */ /* 8.29% */
	/* hashValue[p] = i*3 + j*5 + k*7; */ /* 7.95% */
	hashValue[p] = i*3 + j*5 + k*7; /* 7.95 % */
    }
#endif /* HASH_COMPARE */
    /* Detect all RLE and LZ77 jump possibilities */
    for (p=0; p<inlen; p++) {
#ifndef BIG
	if (!(p & 2047)) {
	    fprintf(stderr, "\r%d ", p);
	    fflush(stderr);	/* for SAS/C */
	}
#endif /* BIG */
	/* check run-length code - must be done, LZ77 search needs it! */
	if (rle[p] <= 0) {
	    /*
		There are so few RLE's and especially so few
		long RLE's that byte-by-byte is good enough.
	     */
	    unsigned char *a = indata + p;
	    int val = *a++; /* if this were uchar, it would go to stack..*/
	    int top = inlen - p;
	    int rlelen = 1;

	    /* Loop for the whole RLE */
	    while (rlelen<top && *a++ == (unsigned char)val
#ifdef BIG
		  && rlelen < 65535
#endif /* BIG */
		 ) {
		rlelen++;
	    }
#ifdef HASH_STAT
	    compares += rlelen;
#endif /* HASH_STAT */

	    if (rlelen>=2) {
		rleHist[indata[p]]++;

		for (i=rlelen-1; i>=0; i--) {
		    rle[p+i] = rlelen-i;
		    elr[p+i] = i;	/* For RLE backward skipping */
		}
#if 0
		if (rlelen>maxlzlen) {
		    /* Jump over some unnecessary memory references */
		    p += rlelen - maxlzlen - 1;
		    continue;
		}
#endif
	    }
	}

	/* check LZ77 code */
	if (p+rle[p]+1<inlen) {
	    int bot = p - lzsz, maxval, maxpos, rlep = rle[p];
#ifdef HASH_COMPARE
	    unsigned char hashCompare = hashValue[p];
#else
	    unsigned char valueCompare = indata[p+2];
#endif /* HASH_COMPARE */

	    /*
		There's always 1 equal byte, although it may
		not be marked as RLE.
	     */
	    if (rlep <= 0)
		rlep = 1;
	    if (bot < 0)
		bot = 0;
	    bot += (rlep-1);

	    /*
		First get the shortest possible match (if any).
		If there is no 2-byte match, don't look further,
		because there can't be a longer match.
	     */
	    i = (int)lastPair[ (indata[p]<<8) | indata[p+1] ] -1;
	    if (i>=0 && i>=bot) {
		/* Got a 2-byte match at least */
		maxval = 2;
		maxpos = p-i;

		/*
		    A..AB	rlep # of A's, B is something else..

		    Search for bytes that are in p + (rlep-1), i.e.
		    the last rle byte ('A') and the non-matching one
		    ('B'). When found, check if the rle in the compare
		    position (i) is long enough (i.e. the same number
		    of A's at p and i-rlep+1).

		    There are dramatically less matches for AB than for
		    AA, so we get a huge speedup with this approach.
		    We are still guaranteed to find the most recent
		    longest match there is.
		 */

		i = (int)lastPair[(indata[p+(rlep-1)]<<8) | indata[p+rlep]] -1;
		while (i>=bot /* && i>=rlep-1 */) {   /* bot>=rlep-1, i>=bot  ==> i>=rlep-1 */

		    /* Equal number of A's ? */
		    if (!(rlep-1) || rle[i-(rlep-1)]==rlep) {	/* 'head' matches */
			/* rlep==1 ==> (rlep-1)==0 */
			/* ivanova.run: 443517 rlep==1,
			   709846 rle[i+1-rlep]==rlep */

			/*
			    Check the hash values corresponding to the last
			    two bytes of the currently longest match and
			    the first new matching(?) byte. If the hash
			    values don't match, don't bother to check the
			    data itself.
			 */
#ifdef HASH_STAT
			hashChecks++;
#endif /* HASH_STAT */
			if (
#ifdef HASH_COMPARE
			    hashValue[i+maxval-rlep-1] == hashCompare
#else
			    indata[i+maxval-rlep+1] == valueCompare
#endif /* HASH_COMPARE */
			   ) {
			    unsigned char *a = indata + i+2;	/* match  */
			    unsigned char *b = indata + p+rlep-1+2;/* curpos */
			    int topindex = inlen-(p+rlep-1);

			    /* the 2 first bytes ARE the same.. */
			    j = 2;
			    while (j < topindex && *a++==*b++)
				j++;

#ifdef HASH_STAT
			    hashEqual++;
			    compares += j - 1;
#endif /* HASH_STAT */
			    if (j + rlep-1 > maxval) {
				int tmplen = j+rlep-1, tmppos = p-i+rlep-1;

				if (tmplen > maxlzlen)
				    tmplen = maxlzlen;

				if (lzmlen[p] < tmplen) {
				    lzmlen[p] = tmplen;
				    lzmpos[p] = tmppos;
				}
				/* Accept only versions that really are shorter */
				if (tmplen*8 - LenLz(tmplen, tmppos) >
				    maxval*8 - LenLz(maxval, maxpos)) {
				    maxval = tmplen;
				    maxpos = tmppos;
#ifdef HASH_COMPARE
				    hashCompare = hashValue[p+maxval-2];
#else
				    valueCompare = indata[p+maxval];
#endif /* HASH_COMPARE */
				}
#if 0
				else {
				    printf("@%5d %d*8 - LenLz(%d, %4x)==%d < ",
					   p, tmplen, tmplen, tmppos,
					   tmplen*8 - LenLz(tmplen, tmppos));
				    printf("%d*8 - LenLz(%d, %4x)==%d\n",
					   maxval, maxval, maxpos,
					   maxval*8 - LenLz(maxval, maxpos));
				}
#endif
				if (maxval == maxlzlen)
				    break;
			    }
			}
		    }
#ifdef BACKSKIP_FULL
		    if (!backSkip[i])
			break; /* No previous occurrances (near enough) */
		    i -= (int)backSkip[i];
#else
		    if (!backSkip[i & 0xffff])
			break; /* No previous occurrances (near enough) */
		    i -= (int)backSkip[i & 0xffff];
#endif /* BACKSKIP_FULL */
		}

		/*
		    If there is 'A' in the previous position also,
		    RLE-like LZ77 is possible, although rarely
		    shorter than real RLE.
		 */
		if (p && rle[p-1] > maxval) {
		    maxval = rle[p-1] - 1;
		    maxpos = 1;
		}
		/*
		    Last, try to find as long as possible match
		    for the RLE part only.
		 */
		if (maxval < maxlzlen && rlep > maxval) {
		    bot = p - lzsz;
		    if (bot < 0)
			bot = 0;

		    /* Note: indata[p] == indata[p+1] */
		    i = (int)lastPair[indata[p]*257] -1;
		    while (/* i>= rlep-2 &&*/ i>=bot) {
			if (elr[i] + 2 > maxval) {
			    maxval = min(elr[i] + 2, rlep);
			    maxpos = p - i + (maxval-2);
			    if(maxval == rlep)
				break; /* Got enough */
			}
			i -= elr[i];
#ifdef BACKSKIP_FULL
			if (!backSkip[i])
			    break; /* No previous occurrances (near enough) */
			i -= (int)backSkip[i];
#else
			if (!backSkip[i & 0xffff])
			    break; /* No previous occurrances (near enough) */
			i -= (int)backSkip[i & 0xffff];
#endif /* BACKSKIP_FULL */
		    }
		}
		if (p+maxval > inlen) {
		    fprintf(stderr,
			    "Error @ %d, lzlen %d, pos %d - exceeds inlen\n",
			    p, maxval, maxpos);
		    maxval = inlen - p;
		}
		if (lzmlen[p] < maxval) {
		    lzmlen[p] = maxval;
		    lzmpos[p] = maxpos;
		}
		if (maxpos<=256 || maxval > 2) {
		    if (maxpos < 0)
			fprintf(stderr, "Error @ %d, lzlen %d, pos %d\n",
				p, maxval, maxpos);
		    lzlen[p] = (maxval<maxlzlen)?maxval:maxlzlen;
		    lzpos[p] = maxpos;
		}
	    }
	}
#ifdef DELTA
	/* check LZ77 code again, ROT1..255 */
	if ((type & FIXF_DLZ) && /* rle[p]<maxlzlen && */ p+rle[p]+1<inlen) {
	int rot;

	for (rot = 1; rot < 255/*BUG:?should be 256?*/; rot++) {
	    int bot = p - /*lzsz*/256, maxval, maxpos, rlep = rle[p];
	    unsigned char valueCompare = (indata[p+2] DELTA_OP rot) & 0xff;

	    /*
		There's always 1 equal byte, although it may
		not be marked as RLE.
	     */
	    if (rlep <= 0)
		rlep = 1;
	    if (bot < 0)
		bot = 0;
	    bot += (rlep-1);

	    /*
		First get the shortest possible match (if any).
		If there is no 2-byte match, don't look further,
		because there can't be a longer match.
	     */
	    i = (int)lastPair[ (((indata[p] DELTA_OP rot) & 0xff)<<8) |
				((indata[p+1] DELTA_OP rot) & 0xff) ] -1;
	    if (i>=0 && i>=bot) {
		/* Got a 2-byte match at least */
		maxval = 2;
		maxpos = p-i;

		/*
		    A..AB	rlep # of A's, B is something else..

		    Search for bytes that are in p + (rlep-1), i.e.
		    the last rle byte ('A') and the non-matching one
		    ('B'). When found, check if the rle in the compare
		    position (i) is long enough (i.e. the same number
		    of A's at p and i-rlep+1).

		    There are dramatically less matches for AB than for
		    AA, so we get a huge speedup with this approach.
		    We are still guaranteed to find the most recent
		    longest match there is.
		 */

		i = (int)lastPair[(((indata[p+(rlep-1)] DELTA_OP rot) & 0xff)<<8) |
				   ((indata[p+rlep] DELTA_OP rot) & 0xff)] -1;
		while (i>=bot /* && i>=rlep-1 */) {   /* bot>=rlep-1, i>=bot  ==> i>=rlep-1 */

		    /* Equal number of A's ? */
		    if (!(rlep-1) || rle[i-(rlep-1)]==rlep) {	/* 'head' matches */
			/* rlep==1 ==> (rlep-1)==0 */
			/* ivanova.run: 443517 rlep==1,
			   709846 rle[i+1-rlep]==rlep */

			/*
			    Check the hash values corresponding to the last
			    two bytes of the currently longest match and
			    the first new matching(?) byte. If the hash
			    values don't match, don't bother to check the
			    data itself.
			 */
#ifdef HASH_STAT
			hashChecks++;
#endif /* HASH_STAT */
			if (indata[i+maxval-rlep+1] == valueCompare) {
			    unsigned char *a = indata + i+2;	/* match  */
			    unsigned char *b = indata + p+rlep-1+2;/* curpos */
			    int topindex = inlen-(p+rlep-1);

			    /* the 2 first bytes ARE the same.. */
			    j = 2;
			    while (j < topindex && *a++==((*b++ DELTA_OP rot) & 0xff))
				j++;

#ifdef HASH_STAT
			    hashEqual++;
			    compares += j - 1;
#endif /* HASH_STAT */
			    if (j + rlep-1 > maxval) {
				int tmplen = j+rlep-1, tmppos = p-i+rlep-1;

				if (tmplen > maxlzlen)
				    tmplen = maxlzlen;

				/* Accept only versions that really are shorter */
				if (tmplen*8 - LenLz(tmplen, tmppos) >
				    maxval*8 - LenLz(maxval, maxpos)) {
				    maxval = tmplen;
				    maxpos = tmppos;

				    valueCompare = (indata[p+maxval] DELTA_OP rot) & 0xff;
				}
#if 0
				else {
				    printf("@%5d %d*8 - LenLz(%d, %4x)==%d < ",
					   p, tmplen, tmplen, tmppos,
					   tmplen*8 - LenLz(tmplen, tmppos));
				    printf("%d*8 - LenLz(%d, %4x)==%d\n",
					   maxval, maxval, maxpos,
					   maxval*8 - LenLz(maxval, maxpos));
				}
#endif
				if (maxval == maxlzlen)
				    break;
			    }
			}
		    }
#ifdef BACKSKIP_FULL
		    if (!backSkip[i])
			break; /* No previous occurrances (near enough) */
		    i -= (int)backSkip[i];
#else
		    if (!backSkip[i & 0xffff])
			break; /* No previous occurrances (near enough) */
		    i -= (int)backSkip[i & 0xffff];
#endif /* BACKSKIP_FULL */
		}

		if (p+maxval > inlen) {
		    fprintf(stderr,
			    "Error @ %d, lzlen %d, pos %d - exceeds inlen\n",
			    p, maxval, maxpos);
		    maxval = inlen - p;
		}
		if (maxval > 3 && maxpos <= 256 &&
		    (maxval > lzlen2[p] ||
		     (maxval == lzlen2[p] && maxpos < lzpos2[p]))) {
		    if (maxpos < 0)
			fprintf(stderr, "Error @ %d, lzlen %d, pos %d\n",
				p, maxval, maxpos);
		    lzlen2[p] = (maxval<maxlzlen)?maxval:maxlzlen;
		    lzpos2[p] = maxpos;
		}
	    }
	}
	if (lzlen2[p] <= lzlen[p] || lzlen2[p] <= rle[p]) {
	    lzlen2[p] = lzpos2[p] = 0;
	}
	}
#endif

	/* Update the two-byte history ('hash table') &
	   backSkip ('linked list') */
	if (p+1<inlen) {
	    int index = (indata[p]<<8) | indata[p+1];
	    int ptr = p - (lastPair[index]-1);

	    if (ptr > p || ptr > 0xffff)
		ptr = 0;

#ifdef BACKSKIP_FULL
	    backSkip[p] = ptr;
#else
	    backSkip[p & 0xffff] = ptr;
#endif /* BACKSKIP_FULL */
	    lastPair[index] = p+1;
	}
    }
    if ((flags & F_NORLE)) {
	for (p=1; p<inlen; p++) {
	    if (rle[p-1]-1 > lzlen[p]) {
		lzlen[p] = (rle[p]<maxlzlen)?rle[p]:maxlzlen;
		lzpos[p] = 1;
	    }
	}
	for (p=0; p<inlen; p++) {
	    rle[p] = 0;
	}
    }
    fprintf(stderr, "\rChecked: %d \n", p);
    fflush(stderr);	/* for SAS/C */


    /* Initialize the RLE selections */
    InitRle(flags);

    /* Check the normal bytes / all ratio */
    if ((flags & F_AUTO)) {
	int mb, mv;

	fprintf(stderr, "Selecting the number of escape bits.. ");
	fflush(stderr);	/* for SAS/C */

	/*
	    Absolute maximum number of escaped bytes with
	    the escape optimize is 2^-n, where n is the
	    number of escape bits used.

	    This worst case happens only on equal-
	    distributed normal bytes (01230123..).
	    This is why the typical values are so much smaller.
	 */

	mb = 0;
	mv = 8*OUT_SIZE;
	for (escBits=1; escBits<9; escBits++) {
	    int escaped, other = 0, c;

	    escMask = (0xff00>>escBits) & 0xff;

	    /* Find the optimum path for selected escape bits (no optimize) */
	    OptimizeLength(0);

	    /* Optimize the escape selections for this path & escBits */
	    escaped = OptimizeEscape(&escape, &other);

	    /* Compare value: bits lost for escaping -- bits lost for prefix */
	    c = (escBits+3)*escaped + other*escBits;
	    if ((flags & F_STATS)) {
		fprintf(stderr, " %d:%d", escBits, c);
		fflush(stderr);	/* for SAS/C */
	    }
	    if (c < mv) {
		mb = escBits;
		mv = c;
	    } else {
		/* minimum found */
		break;
	    }
	    if (escBits==4 && (flags & F_STATS))
		fprintf(stderr, "\n");
	}
	if (mb==1) {	/* Minimum was 1, check 0 */
	    int escaped;

	    escBits = 0;
	    escMask = 0;

	    /* Find the optimum path for selected escape bits (no optimize) */
	    OptimizeLength(0);
	    /* Optimize the escape selections for this path & escBits */
	    escaped = OptimizeEscape(&escape, NULL);

	    if ((flags & F_STATS)) {
		fprintf(stderr, " %d:%d", escBits, 3*escaped);
		fflush(stderr);	/* for SAS/C */
	    }
	    if (3*escaped < mv) {
		mb = 0;
		/* mv = 3*escaped; */
	    }
	}
	if ((flags & F_STATS))
	    fprintf(stderr, "\n");

	fprintf(stderr, "Selected %d-bit escapes\n", mb);
	escBits = mb;
	escMask = (0xff00>>escBits) & 0xff;
    }

    if (!(flags & F_NOOPT)) {
	fprintf(stderr, "Optimizing LZ77 and RLE lengths...");
 	fflush(stderr);	/* for SAS/C */
    }

    /* Find the optimum path (optimize) */
    OptimizeLength((flags & F_NOOPT)?0:1);
    if ((flags & F_STATS)) {
	if(!(flags & F_NOOPT))
	    fprintf(stderr, " gained %d units.\n", lzopt/8);
    } else
	fprintf(stderr, "\n");

    if (1 || (flags & F_AUTOEX)) {
	long lzstat[5] = {0,0,0,0,0}, i, cur = 0, old = extraLZPosBits;

	fprintf(stderr, "Selecting LZPOS LO length.. ");
	fflush(stderr);	/* for SAS/C */

	for (p=0; p<inlen; ) {
	    switch (mode[p]) {
	    case LZ77: /* lz */
		extraLZPosBits = 0;
		lzstat[0] += LenLz(lzlen[p], lzpos[p]);
		extraLZPosBits = 1;
		lzstat[1] += LenLz(lzlen[p], lzpos[p]);
		extraLZPosBits = 2;
		lzstat[2] += LenLz(lzlen[p], lzpos[p]);
		extraLZPosBits = 3;
		lzstat[3] += LenLz(lzlen[p], lzpos[p]);
		extraLZPosBits = 4;
		lzstat[4] += LenLz(lzlen[p], lzpos[p]);
		p += lzlen[p];
		break;
#ifdef DELTA
	    case DLZ:
		p += lzlen2[p];
		break;
#endif
	    case RLE: /* rle */
		p += rle[p];
		break;

	    default: /* normal */
		p++;
		break;
	    }
	}
	for (i=0; i<5; i++) {
	    if ((flags & F_STATS))
		fprintf(stderr, " %ld:%ld", i + 8, lzstat[i]);

	    /* first time around (lzstat[0] < lzstat[0]) */
	    if (lzstat[i] < lzstat[cur])
		cur = i;
	}
	extraLZPosBits = (flags & F_AUTOEX)?cur:old;

	if ((flags & F_STATS))
	    fprintf(stderr, "\n");

	fprintf(stderr, "Selected %d-bit LZPOS LO part\n",
		extraLZPosBits + 8);
	if (cur != old) {
	    fprintf(stderr,
		    "Note: Using option -p%ld you may get better results.\n",
		    cur);
	}
	/* Find the optimum path (optimize) */
	if (extraLZPosBits != old)
	    OptimizeLength((flags & F_NOOPT)?0:1);
    }
    if (1) {
	long stat[4] = {0,0,0,0};

	for (p=0; p<inlen; ) {
	    switch (mode[p]) {
	    case LZ77: /* lz */
		if ((lzpos[p] >> 8)+1 > (1<<maxGamma))
		    stat[3]++;
		if (lzlen[p] > (1<<maxGamma))
		    stat[0]++;
		p += lzlen[p];
		break;

	    case RLE: /* rle */
		if (rle[p] > (1<<(maxGamma-1))) {
		    if (rle[p] <= (1<<maxGamma))
			stat[1]++;
#if 0
		    else if (rle[p] <= (2<<maxGamma))
			stat[2]++;
#endif
		}
		p += rle[p];
		break;
#ifdef DELTA
	    case DLZ:
		p += lzlen2[p];
		break;
#endif
	    default: /* normal */
		p++;
		break;
	    }
	}
	/* TODO: better formula.. */
	if (maxGamma < 7 && stat[0] + stat[1] + stat[3] > 10) {
	    fprintf(stderr,
		    "Note: Using option -m%d you may get better results.\n",
		    maxGamma+1);
	}
	if (maxGamma > 5 && stat[0] + stat[1] + stat[3] < 4) {
	    fprintf(stderr,
		    "Note: Using option -m%d you may get better results.\n",
		    maxGamma-1);
	}
    }

    /* Optimize the escape selections */
    OptimizeEscape(&escape, NULL);
    if (startEscape)
	*startEscape = escape;
    OptimizeRle(flags);	/* Retune the RLE selections */

#ifdef ENABLE_VERBOSE
    if ((flags & F_VERBOSE)) {
	int oldEscape = escape;
	printf("normal RLE  LZLEN LZPOS(absolute)\n\n");

	for (p=0; p<inlen; ) {
	    switch (mode[p]) {
	    case LZ77:
		mode[p - lzpos[p]] |= MMARK; /* Was referred to by lz77 */
		p += lzlen[p];
		break;
	    case RLE:
		p += rle[p];
		break;
#ifdef DELTA
	    case DLZ:
		mode[p - lzpos2[p]] |= MMARK; /* Was referred to by lz77 */
		p += lzlen2[p];
		break;
#endif
	/*  case LITERAL:
	    case MMARK:*/
	    default:
		p++;
		break;
	    }
	}

	j = 0;
	for (p=0; p<inlen; p++) {
	    switch (mode[p]) {
#ifdef DELTA
	    case MMARK | DLZ:
	    case DLZ:
		if (j==p) {
		    printf(">");
		    j += lzlen2[p];
		} else
		    printf(" ");
		if (lzpos2) {
		    printf(" %04x*%03d*+%02x", lzpos2[p], lzlen2[p],
			   (indata[p] - indata[p-lzpos2[p]]) & 0xff);
		}
		printf(" 001   %03d   %03d  %04x(%04x)  %02x %s\n",
			rle[p], lzlen[p], lzpos[p], p-lzpos[p], indata[p],
			(mode[p] & MMARK)?"#":" ");
		break;
#endif
	    case MMARK | LITERAL:
	    case LITERAL:
		if (j==p) {
		    printf(">");
		} else
		    printf(" ");
#ifdef DELTA
		if (lzpos2) {
		    printf(" %04x %03d +%02x", lzpos2[p], lzlen2[p],
			   (indata[p] - indata[p-lzpos2[p]]) & 0xff);
		}
#endif
		if (j==p) {
		    printf("*001*  %03d   %03d  %04x(%04x)  %02x %s %02x",
			   rle[p], lzlen[p], lzpos[p], p-lzpos[p], indata[p],
			   (mode[p] & MMARK)?"#":" ", newesc[p]);
		    if ((indata[p] & escMask) == escape) {
			escape = newesc[p];
			printf("«");
		    }
		    printf("\n");
		    j += 1;
		} else {
		    printf("*001*  %03d   %03d  %04x(%04x)  %02x %s %02x\n",
			   rle[p], lzlen[p], lzpos[p], p-lzpos[p], indata[p],
			   (mode[p] & MMARK)?"#":" ", newesc[p]);
		}
		break;
	    case MMARK | LZ77:
	    case LZ77:
		if (j==p) {
		    printf(">");
		    j += lzlen[p];
		} else
		    printf(" ");
#ifdef DELTA
		if (lzpos2) {
		    printf(" %04x %03d +%02x", lzpos2[p], lzlen2[p],
			   (indata[p] - indata[p-lzpos2[p]]) & 0xff);
		}
#endif
		printf(" 001   %03d  *%03d* %04x(%04x)  %02x %s",
			rle[p], lzlen[p], lzpos[p], p-lzpos[p], indata[p],
			(mode[p] & MMARK)?"#":" ");

		printf("\n");

		break;
	    case MMARK | RLE:
	    case RLE:
		if (j==p) {
		    printf(">");
		    j += rle[p];
		} else
		    printf(" ");
#ifdef DELTA
		if (lzpos2) {
		    printf(" %04x %03d +%02x", lzpos2[p], lzlen2[p],
			   (indata[p] - indata[p-lzpos2[p]]) & 0xff);
		}
#endif
		printf(" 001  *%03d*  %03d  %04x(%04x)  %02x %s\n",
			rle[p], lzlen[p], lzpos[p], p-lzpos[p], indata[p],
			(mode[p] & MMARK)?"#":" ");
		break;
	    default:
		j++;
		break;
	    }
	    mode[p] &= ~MMARK;
	}
	escape = oldEscape;
    }
#endif /* ENABLE_VERBOSE */


    /* Perform rescan */
    {
	int esc = escape;

    for (p=0; p<inlen; ) {
	switch (mode[p]) {
	case LITERAL: /* normal */
	    if ((indata[p] & escMask) == esc) {
		esc = newesc[p];
	    }
	    p++;
	    break;

#ifdef DELTA
	case DLZ:
	    p += lzlen2[p];
	    break;
#endif

	case LZ77: /* lz77 */

#ifdef BACKSKIP_FULL
	    /* Not possible for smaller backSkip table
	       (the table is overwritten during previous use) */
#ifdef RESCAN
	    /* Re-search matches to get the closest one */
	    if (lzopt && /* If any changes to lengths.. */
	        lzlen[p] > 2 /*&& lzlen[p] > rle[p]*/) {
		int bot = p - lzpos[p] + 1, i;
		unsigned short rlep = rle[p];

		if (!rlep)
		    rlep = 1;
		if (bot < 0)
		    bot = 0;
		bot += (rlep-1);

		i = p - (int)backSkip[p];
		while (i>=bot /* && i>=rlep-1 */) {
		    /* Equal number of A's ? */
		    if (rlep==1 || rle[i-rlep+1]==rlep) {	/* 'head' matches */
			unsigned char *a = indata + i+1;	/* match  */
			unsigned char *b = indata + p+rlep-1+1;	/* curpos */
			int topindex = inlen-(p+rlep-1);

			j = 1;
			while (j < topindex && *a++==*b++)
			    j++;

			if (j + rlep-1 >= lzlen[p]) {
			    int tmppos = p-i+rlep-1;

			    rescan +=
				LenLz(lzlen[p], lzpos[p]) -
				LenLz(lzlen[p], tmppos);
#if 0
			    printf("@%d, lzlen %d, pos %04x -> %04x\n",
				    p, lzlen[p], lzpos[p], tmppos);
			    for (i=-1; i<=lzlen[p]; i++) {
				printf("%02x %02x %02x  ",
				       indata[p+i], indata[p-lzpos[p]+i],
				       indata[p-tmppos+i]);
			    }
			    printf("\n");
#endif
			    lzpos[p] = tmppos;
			    break;
			}
		    }
		    if (!backSkip[i])
			break; /* No previous occurrances (near enough) */
		    i -= (int)backSkip[i];
		}
	    }
#endif /* RESCAN */
#endif /* BACKSKIP_FULL */

	    p += lzlen[p];
	    break;

	case RLE: /* rle */
	    p += rle[p];
	    break;

	default: /* Error Flynn :-) */
	    fprintf(stderr, "Internal error: mode %d\n", mode[p]);
	    p++;
	    break;
	}
    }
    }


    /* start of output */

    for (p=0; p<inlen; ) {
	switch (mode[p]) {
	case LITERAL: /* normal */
	    length[p] = outPointer;

	    OutputNormal(&escape, indata+p, newesc[p]);
	    p++;
	    break;

#ifdef DELTA
	case DLZ:
	    for (i=0; i<lzlen2[p]; i++)
		length[p+i] = outPointer;
	    OutputDLz(&escape, lzlen2[p], lzpos2[p],
			(indata[p] - indata[p-lzpos2[p]]) & 0xff);
	    p += lzlen2[p];
	    break;
#endif

	case LZ77: /* lz77 */
	    for (i=0; i<lzlen[p]; i++)
		length[p+i] = outPointer;
	    OutputLz(&escape, lzlen[p], lzpos[p], indata+p-lzpos[p], p);
	    p += lzlen[p];
	    break;

	case RLE: /* rle */
	    for (i=0; i<rle[p]; i++)
		length[p+i] = outPointer;
	    OutputRle(&escape, indata+p, rle[p]);
	    p += rle[p];
	    break;

	default: /* Error Flynn :-) */
	    fprintf(stderr, "Internal error: mode %d\n", mode[p]);
	    p++;
	    break;
	}
    }
    OutputEof(&escape);


    /* xxxxxxxxxxxxxxxxxxx uncompressed */
    /*   yyyyyyyyyyyyyyyyy compressed */
    /* zzzz                */

    i = inlen;
    for (p=0; p<inlen; p++) {
	int pos = (inlen - outPointer) + (int)length[p] - p;
	i = min(i, pos);
    }
    if (i<0)
	reservedBytes = -i + 2;
    else
	reservedBytes = 0;

#ifndef BIG
    if (type == 0) {
	headerSize = 16 + rleUsed;
    } else
#endif /* BIG */
    {
	if (endAddr + reservedBytes + 3 > memEnd) {
	    type |= FIXF_WRAP;
	} else {
	    type &= ~FIXF_WRAP;
	}
	headerSize = GetHeaderSize(type, NULL) + rleUsed - 15;
    }
    outlen = outPointer + headerSize;	/* unpack code */
    fprintf(stderr, "In: %d, out: %d, ratio: %5.2f%% (%4.2f[%4.2f] b/B)"
	    ", gained: %5.2f%%\n",
	    inlen, outlen, (double)outlen*100.0/(double)inlen + 0.005,
	    8.0*(double)outlen/(double)inlen + 0.005,
	    8.0*(double)(outlen-headerSize+rleUsed+4)/(double)inlen + 0.005,
	    100.0 - (double)outlen*100.0/(double)inlen + 0.005);

#ifdef DELTA
    if ((type & FIXF_DLZ)) {
	fprintf(stderr, "Gained RLE: %d (S+L:%d+%d), DLZ: %d, LZ: %d, Esc: %d"
		", Decompressor: %d\n",
		gainedRle/8, gainedSRle/8, gainedLRle/8, gainedDLz/8,
		gainedLz/8, -gainedEscaped/8, -headerSize);

	fprintf(stderr, "Times  RLE: %d (%d+%d), DLZ: %d, LZ: %d, Esc: %d (normal: %d)"
		", %d escape bit%s\n",
		timesRle, timesSRle, timesLRle, timesDLz,
		timesLz, timesEscaped, timesNormal,
		escBits, (escBits==1)?"":"s" );
    } else
#endif
    {
	fprintf(stderr, "Gained RLE: %d (S+L:%d+%d), LZ: %d, Esc: %d"
		", Decompressor: %d\n",
		gainedRle/8, gainedSRle/8, gainedLRle/8,
		gainedLz/8, -gainedEscaped/8, -headerSize);

	fprintf(stderr, "Times  RLE: %d (%d+%d), LZ: %d, Esc: %d (normal: %d)"
		", %d escape bit%s\n",
		timesRle, timesSRle, timesLRle,
		timesLz, timesEscaped, timesNormal,
		escBits, (escBits==1)?"":"s" );
    }
    if ((flags & F_STATS)) {
	const char *ll[] = {"2", "3-4", "5-8", "9-16", "17-32", "33-64",
			    "65-128", "129-256"};
	fprintf(stderr, "(Gained by RLE Code: %d, LZPOS LO Bits %d"
		", maxLen: %d, tag bit/prim. %4.2f)\n",
		gainedRlecode/8 - rleUsed,
		extraLZPosBits + 8,
		(2<<maxGamma),
		(double)((timesRle+timesLz)*escBits +
			 timesEscaped*(escBits + 3))/
		(double)(timesRle+timesLz+timesNormal) + 0.0049);

	fprintf(stderr, "   LZPOS HI+2 LZLEN S-RLE RLEcode\n");
	fprintf(stderr, "   ------------------------------\n");
	for (i=0; i<=maxGamma; i++) {
	    fprintf(stderr, "%-7s %5d %5d", ll[i],
		    lenStat[i][0], lenStat[i][1]);
	    if (i<maxGamma)
		fprintf(stderr, " %5d", lenStat[i][2]);
	    else
		fprintf(stderr, "     -");

	    if (i<5)
		fprintf(stderr, "   %5d%s\n", lenStat[i][3], (i==4)?"*":"");
	    else
		fprintf(stderr, "       -\n");
	}
#ifdef BACKSKIP_FULL
#ifdef RESCAN
	fprintf(stderr, "LZ77 rescan gained %d bytes\n", rescan/8);
#endif /* RESCAN */
#endif /* BACKSKIP_FULL */


#ifdef HASH_STAT
#ifdef HASH_COMPARE
	fprintf(stderr,
		"Hash Checks %ld (%ld, %4.2f%% equal), RLE/LZ compares %ld\n",
		hashChecks, hashEqual,
		100.0*(double)hashEqual/(double)hashChecks,
		compares);
#else
	fprintf(stderr,
		"Value Checks %ld (%ld, %4.2f%% equal), RLE/LZ compares %ld\n",
		hashChecks, hashEqual,
		100.0*(double)hashEqual/(double)hashChecks,
		compares);
#endif /* HASH_COMPARE */
#endif /* HASH_STAT */

    }

errorexit:
    if (rle)
	free(rle);
    if (elr)
	free(elr);
    if (lzmlen)
	free(lzmlen);
    if (lzmpos)
	free(lzmpos);
    if (lzlen)
	free(lzlen);
    if (lzpos)
	free(lzpos);
    if (length)
	free(length);
    if (mode)
	free(mode);
    if (newesc)
	free(newesc);
    if (lastPair)
	free(lastPair);
    if (backSkip)
	free(backSkip);
#ifdef HASH_COMPARE
    if (hashValue)
	free(hashValue);
#endif /* HASH_COMPARE */
    return 0;
}



int main(int argc, char *argv[]) {
    int n, execAddr = -1, ea = -1, newlen, startAddr = -1, startEscape;
    int flags = F_2MHZ, lzlen = -1, buflen;
    char *fileIn = NULL, *fileOut = NULL;
    FILE *infp;
    unsigned char tmp[2];
    unsigned long timeused = clock();

    int machineType = 64;
    char *machineTypeTxt;
    int memStart, memEnd;
    int type = 0;

lrange = LRANGE;
maxlzlen = MAXLZLEN;
maxrlelen = MAXRLELEN;
    InitValueLen();

    flags |= (F_AUTO | F_AUTOEX);
    for (n=1; n<argc; n++) {
	if (!strcmp(argv[n], "-flist")) {
	  printf("List of Decompressors:\n");
	  printf("----------------------\n");
	  ListDecompressors(stdout);
	  return EXIT_FAILURE;
	} else if (!strcmp(argv[n], "-ffast")) {
	    type |= FIXF_FAST;
	} else if (!strcmp(argv[n], "-fnorle")) {
	    flags |= F_NORLE;
	} else if (!strcmp(argv[n], "-fshort")) {
	    type |= FIXF_SHORT;
	} else if (!strcmp(argv[n], "-fbasic")) {
	    type |= FIXF_BASIC;
#ifdef DELTA
	} else if (!strcmp(argv[n], "-fdelta")) {
	    type |= FIXF_DLZ;
#endif
	} else if (!strcmp(argv[n], "+f")) {
	    flags &= ~F_2MHZ;
	} else if (argv[n][0]=='-') {
	    int i = 1;
	    char *val, *tmp, c;
	    long tmpval;

	    while (argv[n][i]) {
		switch (argv[n][i]) {
		case 'u':
		    flags |= F_UNPACK;
		    break;

		case 'd':	/* Raw - no loading address */
		    flags |= F_SKIP;
		    break;

		case 'n':	/* noopt, no rle/lzlen optimization */
		    flags |= F_NOOPT;
		    break;

		case 's':
		    flags |= F_STATS;
		    break;

#ifdef ENABLE_VERBOSE
		case 'v':
		    flags |= F_VERBOSE;
		    break;
#endif /* ENABLE_VERBOSE */

		case 'f':
		    flags |= F_2MHZ;
		    break;

		case 'a':
		    flags |= F_AVOID;
		    break;

		case 'h':
		case '?':
		    flags |= F_ERROR;
		    break;

		case 'g':
		case 'i':
		case 'r':
		case 'x':
		case 'm':
		case 'e':
		case 'p':
		case 'l':
		case 'c': /* 64 (C64), 20 (VIC20), 16/4 (C16/Plus4) */
		    c = argv[n][i]; /* Remember the option */
		    if (argv[n][i+1]) {
			val = argv[n]+i+1;
		    } else if (n+1 < argc) {
			val = argv[n+1];
			n++;
		    } else {
			flags |= F_ERROR;
			break;
		    }

		    i = strlen(argv[n])-1;
		    if (*val=='$')
			tmpval = strtol(val+1, &tmp, 16);
		    else
			tmpval = strtol(val, &tmp, 0);
		    if (*tmp) {
			fprintf(stderr,
				"Error: invalid number: \"%s\"\n", val);
			flags |= F_ERROR;
			break;
		    }
		    switch (c) {
		    case 'r':
			lzlen = tmpval;
			break;
		    case 'x':
			ea = tmpval;
			break;
		    case 'm':
			maxGamma = tmpval;
			if (maxGamma < 5 || maxGamma > 7) {
			    fprintf(stderr, "Max length must be 5..7!\n");
			    flags |= F_ERROR;
			    maxGamma = 7;
			}
lrange = LRANGE;
maxlzlen = MAXLZLEN;
maxrlelen = MAXRLELEN;

			InitValueLen();
			break;
		    case 'e':
			escBits = tmpval;
			if (escBits < 0 || escBits > 8) {
			    fprintf(stderr, "Escape bits must be 0..8!\n");
			    flags |= F_ERROR;
			} else
			    flags &= ~F_AUTO;
			escMask = (0xff00>>escBits) & 0xff;
			break;
		    case 'p':
			extraLZPosBits = tmpval;
			if (extraLZPosBits < 0 || extraLZPosBits > 4) {
			    fprintf(stderr,
				    "Extra LZ-pos bits must be 0..4!\n");
			    flags |= F_ERROR;
			} else
			    flags &= ~F_AUTOEX;
			break;
		    case 'l':
			startAddr = tmpval;
			if (startAddr < 0 || startAddr > 0xffff) {
			    fprintf(stderr,
				    "Load address must be 0..0xffff!\n");
			    flags |= F_ERROR;
			}
			break;
		    case 'c': /* 64 (C64), 20 (VIC20), 16/4 (C16/Plus4) */
			machineType = tmpval;
			if (machineType != 64 && machineType != 20 &&
			    machineType != 16 && machineType != 4 &&
			    machineType != 128 && machineType != 0) {
			    fprintf(stderr, "Machine must be 64, 20, 16/4, 128!\n");
			    flags |= F_ERROR;
			}
			break;
		    case 'i': /* Interrupt config */
			if (tmpval==0) {
			    intConfig = 0x78; /* sei */
			} else {
			    intConfig = 0x58; /* cli */
			}
			break;
		    case 'g': /* Memory configuration */
			memConfig = (tmpval & 0xff);
			break;
		    }
		    break;

		default:
		    fprintf(stderr, "Error: Unknown option \"%c\"\n",
			    argv[n][i]);
		    flags |= F_ERROR;
		}
		i++;
	    }
	} else {
	    if (!fileIn) {
		fileIn = argv[n];
	    } else if (!fileOut) {
		fileOut = argv[n];
	    } else {
		fprintf(stderr, "Only two filenames wanted!\n");
		flags |= F_ERROR;
	    }
	}
    }

    if ((flags & F_ERROR)) {
	fprintf(stderr, "Usage: %s [-<flags>] [<infile> [<outfile>]]\n",
		argv[0]);
	fprintf(stderr,
		"\t -flist    list all decompressors\n"
		"\t -ffast    select faster version, if available (longer)\n"
		"\t -fshort   select shorter version, if available (slower)\n"
		"\t -fbasic   select version for BASIC programs (for VIC20 and C64)\n"
#ifdef DELTA
		"\t -fdelta   use delta-lz77 -- shortens some files\n"
#endif
		"\t -f        enable fast mode for C128 (C64 mode) and C16/+4 (default)\n"
		"\t +f        disable fast mode for C128 (C64 mode) and C16/+4\n"
		"\t c<val>    machine: 64 (C64), 20 (VIC20), 16 (C16/+4)\n"
		"\t a         avoid video matrix (for VIC20)\n"
		"\t d         data (no loading address)\n"
		"\t l<val>    set/override load address\n"
		"\t x<val>    set execution address\n"
		"\t e<val>    force escape bits\n"
		"\t r<val>    restrict lz search range\n"
		"\t n         no RLE/LZ length optimization\n"
		"\t s         full statistics\n"
#ifdef ENABLE_VERBOSE
		"\t v         verbose\n"
#endif /* ENABLE_VERBOSE */
		"\t p<val>    force extralzposbits\n"
		"\t m<val>    max len 5..7 (2*2^5..2*2^7)\n"
		"\t i<val>    interrupt enable after decompress (0=disable)\n"
		"\t g<val>    memory configuration after decompress\n"
		"\t u         unpack\n");
	return EXIT_FAILURE;
    }

    if (lzlen == -1)
	lzlen = DEFAULT_LZLEN;

    if (fileIn) {
	if (!(infp = fopen(fileIn, "rb"))) {
	    fprintf(stderr, "Could not open %s for reading!\n", fileIn);
	    return EXIT_FAILURE;
	}
    } else {
	fprintf(stderr, "Reading from stdin\n");
	fflush(stderr);	/* for SAS/C */
	infp = stdin;
    }

    if (!(flags & F_SKIP)) {
	fread(tmp, 1, 2, infp);
	/* Use it only if not overriden by the user */
	if (startAddr==-1)
	    startAddr = tmp[0] + 256*tmp[1];
    }
    if (startAddr==-1)
	startAddr = 0x258;

    /* Read in the data */
    inlen = 0;
    buflen = 0;
    indata = NULL;
    while (1) {
	if (buflen < inlen + lrange) {
	    unsigned char *tmp = realloc(indata, buflen + lrange);
	    if (!tmp) {
		free(indata);
		return 20;
	    }
	    indata = tmp;
	    buflen += lrange;
	}
	newlen = fread(indata + inlen, 1, lrange, infp);
	if (newlen <= 0)
	    break;
	inlen += newlen;
    }
    if (infp != stdin)
	fclose(infp);

    if ((flags & F_UNPACK)) {
	n = UnPack(startAddr, indata, fileOut, flags);
	if (indata)
	    free(indata);
	return n;
    }

    if (startAddr < 0x258
#ifndef BIG
	|| startAddr + inlen -1 > 0xffff
#endif /* BIG */
       ) {
	fprintf(stderr,
		"Only programs from 0x0258 to 0xffff can be compressed\n");
	fprintf(stderr, "(the input file is from 0x%04x to 0x%04x)\n",
		startAddr, startAddr+inlen-1);
	if (indata)
	    free(indata);
	return EXIT_FAILURE;
    }

    switch (machineType) {
    case 20:
	machineTypeTxt = "VIC20 with 8k or 16k (or 24k) expansion memory";
	memStart = 0x1201;
	memEnd = 0x4000;
	type |= FIXF_VIC20 | FIXF_WRAP;

	if (startAddr+inlen > 0x8000) {
	    fprintf(stderr, "Original file exceeds 0x8000 (0x%04x), "
		    "not a valid VIC20 file!\n", startAddr+inlen-1);
	    n = EXIT_FAILURE;
	    goto errexit;
	} else if (startAddr+inlen > 0x6000) {
	    if (startAddr < 0x1000) {
		fprintf(stderr, "Original file exceeds 0x6000 (0x%04x), "
			"3kB+24kB memory expansions assumed\n",
			startAddr+inlen-1);
		machineTypeTxt = "VIC20 with 3k+24k expansion memory";
	    } else {
		fprintf(stderr, "Original file exceeds 0x6000 (0x%04x), "
			"24kB memory expansion assumed\n",
			startAddr+inlen-1);
		machineTypeTxt = "VIC20 with 24k expansion memory";
	    }
	    memEnd = 0x8000;
	} else if (startAddr+inlen > 0x4000) {
	    if (startAddr < 0x1000) {
		fprintf(stderr, "Original file exceeds 0x4000 (0x%04x), "
			"3kB+16kB memory expansion assumed\n",
			startAddr+inlen-1);
		machineTypeTxt =
		    "VIC20 with 3k+16k (or 3k+24k) expansion memory";
	    } else {
		fprintf(stderr, "Original file exceeds 0x4000 (0x%04x), "
			"16kB memory expansion assumed\n",
			startAddr+inlen-1);
		machineTypeTxt = "VIC20 with 16k (or 24k) expansion memory";
	    }
	    memEnd = 0x6000;
	} else if (startAddr+inlen > 0x2000) {
	    if (startAddr < 0x1000) {
		fprintf(stderr, "Original file exceeds 0x2000 (0x%04x), "
			"3kB+8kB memory expansion assumed\n",
			startAddr+inlen-1);
		machineTypeTxt =
		    "VIC20 with 3k+8k (or 3k+16k, or 3k+24k) expansion memory";
	    } else {
		fprintf(stderr, "Original file exceeds 0x2000 (0x%04x), "
			"8kB memory expansion assumed\n",
			startAddr+inlen-1);
	    }
	    /* memEnd = 0x4000; */
	} else {
	    if (startAddr >= 0x1000 && startAddr < 0x1200) {
		fprintf(stderr, "Program for unexpanded VIC detected.\n");
		memStart = 0x1001;
		memEnd = (flags & F_AVOID)?0x1e00:0x2000;
		machineTypeTxt = "VIC20 without expansion memory";
	    } if (startAddr >= 0x400 && startAddr < 0x1000) {
		fprintf(stderr, "Program for 3k-expanded VIC detected.\n");
		memStart = 0x0401;
		memEnd = (flags & F_AVOID)?0x1e00:0x2000;
		machineTypeTxt = "VIC20 with 3k expansion memory";
	    }
	}
	break;
    case 16:
    case 4:
   	type |= FIXF_C16 | FIXF_WRAP;
	if (startAddr+inlen > 0x4000) {
	    fprintf(stderr, "Original file exceeds 0x4000, 61k RAM assumed\n");
	    memStart = 0x1001;
	    memEnd = 0xfd00;
	    machineTypeTxt = "Plus/4";
	} else {
	    fprintf(stderr, "Program for unexpanded C16 detected.\n");
	    memStart = 0x1001;
	    memEnd = 0x4000;
	    machineTypeTxt = "Commodore 16";
	}
	break;
    case 128:
   	type |= FIXF_C128 | FIXF_WRAP;
	memStart = 0x1c01;
	memEnd = 0x10000;
	machineTypeTxt = "Commodore 128";
	break;
    case 0:
	type |= 0;
	machineTypeTxt = "Without decompressor";
	memStart = 0x801;
	memEnd = 0x10000;
	break;
    default:	/* C64 */
	type |= FIXF_C64 | FIXF_WRAP;	/* C64, wrap active */
	machineTypeTxt = "Commodore 64";
	memStart = 0x801;	/* Loading address */
	memEnd = 0x10000;
	break;
    }

    if (startAddr <= memStart) {
	for (n=memStart-startAddr; n<memStart-startAddr+60; n++) {
	    if (indata[n]==0x9e) {	/* SYS token */
		execAddr = 0;
		n++;
		/* Skip spaces and parens */
		while (indata[n]=='(' || indata[n]==' ')
		   n++;

		while (indata[n]>='0' && indata[n]<='9') {
		    execAddr = execAddr * 10 + indata[n++] - '0';
		}
		break;
	    }
	}
    }
    if (ea != -1) {
	if (execAddr!=-1 && ea!=execAddr)
	    fprintf(stderr, "Discarding execution address 0x%04x=%d\n",
		    execAddr, execAddr);
	execAddr = ea;
    } else if (execAddr < startAddr || execAddr >= startAddr+inlen) {
	if ((type & FIXF_BASIC)) {
	    execAddr = 0xa7ae;
	} else {
	    fprintf(stderr, "Note: The execution address was not detected "
		    "correctly!\n");
	    fprintf(stderr, "      Use the -x option to set the execution "
		    "address.\n");
	}
    }
    fprintf(stderr, "Load address 0x%04x=%d, Last byte 0x%04x=%d\n",
	    startAddr, startAddr, startAddr+inlen-1, startAddr+inlen-1);
    fprintf(stderr, "Exec address 0x%04x=%d\n", execAddr, execAddr);
    fprintf(stderr, "New load address 0x%04x=%d\n", memStart, memStart);
    if (machineType == 64) {
	fprintf(stderr, "Interrupts %s and memory config set to $%02x "
		"after decompression\n",
		(intConfig==0x58)?"enabled":"disabled", memConfig);
	fprintf(stderr, "Runnable on %s\n", machineTypeTxt);
    } else if (machineType != 0) {
	fprintf(stderr, "Interrupts %s after decompression\n",
		(intConfig==0x58)?"enabled":"disabled");
	fprintf(stderr, "Runnable on %s\n", machineTypeTxt);
    } else {
	fprintf(stderr, "Standalone decompressor required\n");
    }
#if 0
    if ((flags & F_2MHZ))
	type |= FIXF_FAST;
#endif
    n = PackLz77(lzlen, flags, &startEscape, startAddr + inlen, memEnd, type);
    if (!n) {
	int endAddr = startAddr + inlen; /* end for uncompressed data */
	int hDeCall, progEnd = endAddr;

	if (GetHeaderSize(type, &hDeCall) == 0) {
	    GetHeaderSize(type & ~FIXF_WRAP, &hDeCall);
	}
	if (machineType != 0 &&
	    endAddr - ((outPointer + 255) & ~255) < memStart + hDeCall + 3) {
	    /* would overwrite the decompressor, move a bit upwards */
	    fprintf(stderr, "$%x < $%x, decompressor overwrite possible, "
		    "moving upwards\n",
		    endAddr - ((outPointer + 255) & ~255),
		    memStart + hDeCall + 3);
	    endAddr = memStart + hDeCall + 3 + ((outPointer + 255) & ~255);
	}
	/* Should check that endAddr really is larger than original endaddr! */
#if 0
	/* Move the end address for files that got expanded */
	if (memStart + hSize + outPointer > endAddr) {
	    endAddr = memStart + hSize + outPointer;
	}
#endif
	/* 3 bytes reserved for EOF */
	/* bytes reserved for temporary data expansion (escaped chars) */
	endAddr += 3 + reservedBytes;

#ifdef BIG
	endAddr = 0x10000;
#endif /* BIG */
#ifdef DELTA
	if (!timesDLz) {
	    type &= ~FIXF_DLZ;
	}
#endif
	SavePack(type, outBuffer, outPointer, fileOut,
		 startAddr, execAddr, startEscape, rleValues,
		 endAddr, progEnd, extraLZPosBits, (flags & F_2MHZ)?1:0,
		 memStart, memEnd);

	timeused = clock()-timeused;
	if (!timeused)
	    timeused++;
	fprintf(stderr,
		"Compressed %d bytes in %4.2f seconds (%4.2f kB/sec)\n",
		inlen,
		(double)timeused/CLOCKS_PER_SEC,
		(double)CLOCKS_PER_SEC*inlen/timeused/1024.0);
    }
errexit:
    if (indata)
	free(indata);
    return n;
}


