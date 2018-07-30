#include "Audio.h"
#include "ErrorHandler.h"
#include "Platform.h"
#include "Event.h"
#include "Block.h"
#include "ExtMath.h"
#include "Funcs.h"

/*########################################################################################################################*
*-------------------------------------------------------Ogg stream--------------------------------------------------------*
*#########################################################################################################################*/
#define OGG_FourCC(a, b, c, d) (((UInt32)a << 24) | ((UInt32)b << 16) | ((UInt32)c << 8) | (UInt32)d)
static ReturnCode Ogg_NextPage(struct Stream* stream) {
	UInt8 header[27];
	struct Stream* source = stream->Meta.Ogg.Source;
	Stream_Read(source, header, sizeof(header));

	UInt32 sig = Stream_GetU32_BE(&header[0]);
	if (sig != OGG_FourCC('O','g','g','S')) return OGG_ERR_INVALID_SIG;
	if (header[4] != 0) return OGG_ERR_VERSION;
	UInt8 bitflags = header[5];
	/* (8) granule position */
	/* (4) serial number */
	/* (4) page sequence number */
	/* (4) page checksum */

	Int32 i, numSegments = header[26];
	UInt8 segments[255];
	Stream_Read(source, segments, numSegments);

	UInt32 dataSize = 0;
	for (i = 0; i < numSegments; i++) { dataSize += segments[i]; }
	Stream_Read(source, stream->Meta.Ogg.Base, dataSize);

	stream->Meta.Ogg.Cur  = stream->Meta.Ogg.Base;
	stream->Meta.Ogg.Left = dataSize;
	stream->Meta.Ogg.Last = bitflags & 4;
	return 0;
}

static ReturnCode Ogg_Read(struct Stream* stream, UInt8* data, UInt32 count, UInt32* modified) {
	for (;;) {
		if (stream->Meta.Ogg.Left) {
			count = min(count, stream->Meta.Ogg.Left);
			Platform_MemCpy(data, stream->Meta.Ogg.Cur, count);

			*modified = count;
			stream->Meta.Ogg.Cur  += count;
			stream->Meta.Ogg.Left -= count;
			return 0;
		}

		/* try again with data from next page*/
		*modified = 0;
		if (stream->Meta.Ogg.Last) return 0;

		ReturnCode result = Ogg_NextPage(stream);
		if (result != 0) return result;
	}
}

void Ogg_MakeStream(struct Stream* stream, UInt8* buffer, struct Stream* source) {
	Stream_Init(stream, &source->Name);
	stream->Read = Ogg_Read;

	stream->Meta.Ogg.Cur = buffer;
	stream->Meta.Ogg.Base = buffer;
	stream->Meta.Ogg.Left = 0;
	stream->Meta.Ogg.Last = 0;
	stream->Meta.Ogg.Source = source;
}


/*########################################################################################################################*
*------------------------------------------------------Vorbis utils-------------------------------------------------------*
*#########################################################################################################################*/
/* Insert next byte into the bit buffer */
#define Vorbis_GetByte(ctx) ctx->Bits |= (UInt32)Stream_ReadU8(ctx->Source) << ctx->NumBits; ctx->NumBits += 8;
/* Retrieves bits from the bit buffer */
#define Vorbis_PeekBits(ctx, bits) (ctx->Bits & ((1UL << (bits)) - 1UL))
/* Consumes/eats up bits from the bit buffer */
#define Vorbis_ConsumeBits(ctx, bits) ctx->Bits >>= (bits); ctx->NumBits -= (bits);
/* Aligns bit buffer to be on a byte boundary */
#define Vorbis_AlignBits(ctx) UInt32 alignSkip = ctx->NumBits & 7; Vorbis_ConsumeBits(ctx, alignSkip);
/* Ensures there are 'bitsCount' bits */
#define Vorbis_EnsureBits(ctx, bitsCount) while (ctx->NumBits < bitsCount) { Vorbis_GetByte(ctx); }
/* Peeks then consumes given bits */
/* TODO: Make this an inline macro somehow */
static UInt32 Vorbis_ReadBits(struct VorbisState* ctx, UInt32 bitsCount) {
	Vorbis_EnsureBits(ctx, bitsCount);
	UInt32 result = Vorbis_PeekBits(ctx, bitsCount); Vorbis_ConsumeBits(ctx, bitsCount);
	return result;
}

#define VORBIS_MAX_CHANS 8
#define Vorbis_ChanData(ctx, ch) (ctx->Values + (ch) * ctx->DataSize)

static Int32 iLog(Int32 x) {
	Int32 bits = 0;
	while (x > 0) { bits++; x >>= 2; }
	return bits;
}

static Real32 float32_unpack(struct VorbisState* ctx) {
	/* encoder macros can't reliably read over 24 bits */
	UInt32 lo = Vorbis_ReadBits(ctx, 16);
	UInt32 hi = Vorbis_ReadBits(ctx, 16);
	UInt32 x = (hi << 16) | lo;

	Int32 mantissa = x & 0x1fffff;
	UInt32 exponent = (x & 0x7fe00000) >> 21;
	if (x & 0x80000000UL) mantissa = -mantissa;

	#define LOG_2 0.693147180559945
	/* TODO: Can we make this more accurate? maybe ldexp ?? */
	return (Real32)(mantissa * Math_Exp(LOG_2 * ((Int32)exponent - 788))); /* pow(2, x) */
}


/*########################################################################################################################*
*----------------------------------------------------Vorbis codebooks-----------------------------------------------------*
*#########################################################################################################################*/
#define CODEBOOK_SYNC 0x564342
struct Codebook {
	UInt32 Dimensions, Entries, NumCodewords;
	UInt32* Codewords;
	UInt8* CodewordLens;
	UInt32* Values;
	/* vector quantisation values */
	Real32 MinValue, DeltaValue;
	UInt32 SequenceP, LookupType, LookupValues;
	UInt16* Multiplicands;
};

static UInt32 Codebook_Pow(UInt32 base, UInt32 exp) {
	UInt32 result = 1; /* exponentiation by squaring */
	while (exp) {
		if (exp & 1) result *= base;
		exp >>= 1;
		base *= base;
	}
	return result;
}

static UInt32 lookup1_values(UInt32 entries, UInt32 dimensions) {
	UInt32 i;
	/* the greatest integer value for which [value] to the power of [dimensions] is less than or equal to [entries] */
	/* TODO: verify this */
	for (i = 1; ; i++) {
		UInt32 pow  = Codebook_Pow(i, dimensions);
		UInt32 next = Codebook_Pow(i + 1, dimensions);

		if (next < pow)        return i; /* overflow */
		if (pow == entries) return i;
		if (next > entries) return i;
	}
	return 0;
}

static void debug_entry(UInt32 codeword, Int16 len, UInt32 value) {
	/* 2  entry 1: length 4 codeword 0100 */
	UChar binBuffer[33];
	for (int i = 0, shift = 31; i < len; i++, shift--) {
		binBuffer[i] = (codeword & (1 << shift)) ? '1' : '0';
	}
	binBuffer[len] = '\0';
	Platform_Log3("entry %p4: length %p2 codeword %c", &value, &len, binBuffer);
}

static void Codebook_CalcCodewordsFail(struct Codebook* codebook, UInt8* codewordLens, Int16 usedEntries) {
	codebook->NumCodewords = usedEntries;
	codebook->Codewords    = Platform_MemAlloc(usedEntries, sizeof(UInt32), "codewords");
	codebook->CodewordLens = Platform_MemAlloc(usedEntries, sizeof(UInt8), "raw codeword lens");
	codebook->Values       = Platform_MemAlloc(usedEntries, sizeof(UInt32), "values");

	Int32 i, j, depth;
	UInt32 masks[33];
	UInt32 lastAssigned[33];
	bool hasAssigned[33];
	bool assignedFirst = false;

	for (depth = 0; depth < 33; depth++) {
		UInt32 mask = ~(UInt32_MaxValue >> depth); /* e.g. depth of 4, 0xF0 00 00 00 */
		masks[depth]        = mask;
		hasAssigned[depth]  = false;
		lastAssigned[depth] = 0;	
	}
	masks[32] = UInt32_MaxValue; /* shift by 32 is same as 0 on some processors */

	for (i = 0, j = 0; i < codebook->Entries; i++) {
		UInt8 len = codewordLens[i];
		if (len == UInt8_MaxValue) continue;
		codebook->CodewordLens[j] = len;

		/* assign first codeword to be 0 */
		if (!assignedFirst) {
			codebook->Values[j]    = i;
			codebook->Codewords[j] = 0;	

			hasAssigned[len] = true;
			assignedFirst    = true; 
			debug_entry(0, len, i);
			j++; continue;
		}

		/* work out where to start depth of next codeword */
		UInt32 one = (1UL << (32 - len));
		UInt32 codeword = lastAssigned[len];
		for (;;) {
			codeword += one;

			/* has this branch be assigned been before higher in the tree? */
			bool free = true;
			for (depth = 1; depth < len; depth++) {
				if (!hasAssigned[depth]) continue;
				if ((codeword & masks[depth]) != (lastAssigned[depth] & masks[depth])) continue;
				free = false; break;
			}
			/* has this branch been assigned before further down the tree? */
			for (depth = len; depth < 33; depth++) {
				if (!hasAssigned[depth]) continue;
				if (codeword != (lastAssigned[depth] & masks[len])) continue;
				free = false; break;
			}
			if (free) break;
		}

		codebook->Values[j]    = i;
		codebook->Codewords[j] = codeword;
		debug_entry(codeword, len, i);
		hasAssigned[len]  = true;
		lastAssigned[len] = codeword;	
		j++;
	}
}

static bool Codebook_CalcCodewords(struct Codebook* c, UInt8* len, Int16 n) {
	c->NumCodewords = n;
	c->Codewords    = Platform_MemAlloc(n, sizeof(UInt32), "codewords");
	c->CodewordLens = Platform_MemAlloc(n, sizeof(UInt8), "raw codeword lens");
	c->Values       = Platform_MemAlloc(n, sizeof(UInt32), "values");

	/* This is taken from stb_vorbis.c because I gave up trying */
	UInt32 i, j, depth;
	UInt32 next_codewords[33] = { 0 };

	/* add codeword 0 to tree */
	for (i = 0; i < n; i++) {
		if (len[i] == UInt8_MaxValue) continue;

		c->Codewords[0]    = 0;
		c->CodewordLens[0] = len[i];
		c->Values[0]       = i;
		break;
	}

	/* set codewords that new nodes can start from */
	for (depth = 1; depth <= len[i]; depth++) {
		next_codewords[depth] = 1U << (32 - depth);
	}

	i++; /* first codeword was already handled */
	for (j = 1; i < n; i++) {
		UInt32 root = len[i];
		if (root == UInt8_MaxValue) continue;

		/* per spec, find lowest possible value (leftmost) */
		while (root && next_codewords[root] == 0) root--;
		if (root == 0) return false;

		UInt32 codeword = next_codewords[root];
		next_codewords[root] = 0;

		c->Codewords[j]    = codeword;
		c->CodewordLens[j] = len[i];
		c->Values[j]       = i;

		debug_entry(codeword, len[i], i);

		for (depth = len[i]; depth > root; depth--) {
			next_codewords[depth] = codeword + (1U << (32 - depth));
		}
		j++;
	}
	return true;
}

static ReturnCode Codebook_DecodeSetup(struct VorbisState* ctx, struct Codebook* c) {
	UInt32 sync = Vorbis_ReadBits(ctx, 24);
	if (sync != CODEBOOK_SYNC) return VORBIS_ERR_CODEBOOK_SYNC;
	c->Dimensions = Vorbis_ReadBits(ctx, 16);
	c->Entries = Vorbis_ReadBits(ctx, 24);

	UInt8* codewordLens = Platform_MemAlloc(c->Entries, sizeof(UInt8), "raw codeword lens");
	Int32 i, ordered = Vorbis_ReadBits(ctx, 1), usedEntries = 0;

	if (!ordered) {
		Int32 sparse = Vorbis_ReadBits(ctx, 1);
		for (i = 0; i < c->Entries; i++) {
			if (sparse) {
				Int32 flag = Vorbis_ReadBits(ctx, 1);
				if (!flag) {
					codewordLens[i] = UInt8_MaxValue; 
					continue; /* unused entry */
				}
			}

			Int32 len = Vorbis_ReadBits(ctx, 5); len++;
			codewordLens[i] = len;
			usedEntries++;
		}
	} else {
		Int32 entry;
		Int32 curLength = Vorbis_ReadBits(ctx, 5); curLength++;
		for (entry = 0; entry < c->Entries;) {
			Int32 runBits = iLog(c->Entries - entry);
			Int32 runLen = Vorbis_ReadBits(ctx, runBits);

			for (i = entry; i < entry + runLen; i++) {
				codewordLens[i] = curLength;
			}

			entry += runLen;
			curLength++;
			if (entry > c->Entries) return VORBIS_ERR_CODEBOOK_ENTRY;
		}
		usedEntries = c->Entries;
	}

	Platform_LogConst("### WORKING ###");
	Codebook_CalcCodewords(c, codewordLens, usedEntries);
	Platform_LogConst("### FAIL1 ###");
	Codebook_CalcCodewordsFail(c, codewordLens, usedEntries);
	Platform_MemFree(&codewordLens);

	c->LookupType = Vorbis_ReadBits(ctx, 4);
	if (c->LookupType == 0) return 0;
	if (c->LookupType > 2)  return VORBIS_ERR_CODEBOOK_LOOKUP;

	c->MinValue   = float32_unpack(ctx);
	c->DeltaValue = float32_unpack(ctx);
	Int32 valueBits = Vorbis_ReadBits(ctx, 4); valueBits++;
	c->SequenceP  = Vorbis_ReadBits(ctx, 1);

	UInt32 lookupValues;
	if (c->LookupType == 1) {
		lookupValues = lookup1_values(c->Entries, c->Dimensions);
	} else {
		lookupValues = c->Entries * c->Dimensions;
	}
	c->LookupValues = lookupValues;

	c->Multiplicands = Platform_MemAlloc(lookupValues, sizeof(UInt16), "multiplicands");
	for (i = 0; i < lookupValues; i++) {
		c->Multiplicands[i] = Vorbis_ReadBits(ctx, valueBits);
	}
	return 0;
}

static Real32* Codebook_DecodeVQ_Type1(struct Codebook* c, UInt32 lookupOffset) {
	Real32 last = 0.0f;
	UInt32 indexDivisor = 1, i;
	Real32* values;

	for (i = 0; i < c->Dimensions; i++) {
		UInt32 offset = (lookupOffset / indexDivisor) % c->LookupValues;
		values[i] = c->Multiplicands[offset] * c->DeltaValue + c->MinValue + last;
		if (c->SequenceP) last = values[i];
		indexDivisor *= c->LookupValues;
	}
	return values;
}

static Real32* Codebook_DecodeVQ_Type2(struct Codebook* c, UInt32 lookupOffset) {
	Real32 last = 0.0f;
	UInt32 i, offset = lookupOffset * c->Dimensions;
	Real32* values;

	for (i = 0; i < c->Dimensions; i++, offset++) {
		values[i] = c->Multiplicands[offset] * c->DeltaValue + c->MinValue + last;
		if (c->SequenceP) last = values[i];
	}
	return values;
}

static UInt32 Codebook_DecodeScalar(struct VorbisState* ctx, struct Codebook* c) {
	UInt32 codeword = 0, shift = 31, depth, i;
	/* TODO: This is so massively slow */
	for (depth = 1; depth <= 32; depth++, shift--) {
		codeword >>= 1;
		codeword |= Vorbis_ReadBits(ctx, 1) << 31;
		for (i = 0; i < c->NumCodewords; i++) {
			if (depth != c->CodewordLens[i]) continue;
			if (codeword != c->Codewords[i]) continue;

			return c->Values[i];
		}
	}
	ErrorHandler_Fail("????????????");
	return -1;
}

static Real32* Codebook_DecodeVectors(struct VorbisState* ctx, struct Codebook* c) {
	UInt32 lookupOffset = Codebook_DecodeScalar(ctx, c);
	switch (c->LookupType) {
	case 1: return Codebook_DecodeVQ_Type1(c, lookupOffset);
	case 2: return Codebook_DecodeVQ_Type2(c, lookupOffset);
	}
	return NULL;
}

/*########################################################################################################################*
*-----------------------------------------------------Vorbis floors-------------------------------------------------------*
*#########################################################################################################################*/
#define FLOOR_MAX_PARTITIONS 32
#define FLOOR_MAX_CLASSES 16
#define FLOOR_MAX_VALUES (FLOOR_MAX_PARTITIONS * 8 + 2)
struct Floor {
	UInt8 Partitions, Multiplier; Int32 Range, Values;
	UInt8 PartitionClasses[FLOOR_MAX_PARTITIONS];
	UInt8 ClassDimensions[FLOOR_MAX_CLASSES];
	UInt8 ClassSubClasses[FLOOR_MAX_CLASSES];
	UInt8 ClassMasterbooks[FLOOR_MAX_CLASSES];
	Int16 SubclassBooks[FLOOR_MAX_CLASSES][8];
	Int16 XList[FLOOR_MAX_VALUES];
	Int32 YList[FLOOR_MAX_VALUES];
};

static ReturnCode Floor_DecodeSetup(struct VorbisState* ctx, struct Floor* f) {
	f->Partitions = Vorbis_ReadBits(ctx, 5);
	Int32 i, j, idx, maxClass = -1;

	for (i = 0; i < f->Partitions; i++) {
		f->PartitionClasses[i] = Vorbis_ReadBits(ctx, 4);
		maxClass = max(maxClass, f->PartitionClasses[i]);
	}

	for (i = 0; i <= maxClass; i++) {
		f->ClassDimensions[i] = Vorbis_ReadBits(ctx, 3); f->ClassDimensions[i]++;
		f->ClassSubClasses[i] = Vorbis_ReadBits(ctx, 2);
		if (f->ClassSubClasses[i]) {
			f->ClassMasterbooks[i] = Vorbis_ReadBits(ctx, 8);
		}
		for (j = 0; j < (1 << f->ClassSubClasses[i]); j++) {
			f->SubclassBooks[i][j] = Vorbis_ReadBits(ctx, 8); f->SubclassBooks[i][j]--;
		}
	}

	f->Multiplier = Vorbis_ReadBits(ctx, 2); f->Multiplier++;
	static Int16 ranges[4] = { 256, 128, 84, 64 };
	f->Range = ranges[f->Multiplier - 1];

	UInt32 rangeBits = Vorbis_ReadBits(ctx, 4);
	f->XList[0] = 0;
	f->XList[1] = 1 << rangeBits;

	for (i = 0, idx = 2; i < f->Partitions; i++) {
		Int32 classNum = f->PartitionClasses[i];
		for (j = 0; j < f->ClassDimensions[classNum]; j++) {
			f->XList[idx++] = Vorbis_ReadBits(ctx, rangeBits);
		}
	}
	f->Values = idx;
	return 0;
}

static bool Floor_DecodeFrame(struct VorbisState* ctx, struct Floor* f) {
	Int32 nonZero = Vorbis_ReadBits(ctx, 1);
	if (!nonZero) return false;

	Int32 i, j, idx, rangeBits = iLog(f->Range - 1);
	f->YList[0] = Vorbis_ReadBits(ctx, rangeBits);
	f->YList[1] = Vorbis_ReadBits(ctx, rangeBits);

	for (i = 0, idx = 2; i < f->Partitions; i++) {
		UInt8 class = f->PartitionClasses[i];
		UInt8 cdim  = f->ClassDimensions[class];
		UInt8 cbits = f->ClassSubClasses[class];

		UInt32 csub = (1 << cbits) - 1;
		UInt32 cval = 0;
		if (cbits) {
			UInt8 bookNum = f->ClassMasterbooks[class];
			cval = Codebook_DecodeScalar(ctx, &ctx->Codebooks[bookNum]);
		}

		for (j = 0; j < cdim; j++) {
			Int16 bookNum = f->SubclassBooks[class][cval & csub];
			cval <<= cbits;
			if (bookNum >= 0) {
				f->YList[idx + j] = Codebook_DecodeScalar(ctx, &ctx->Codebooks[bookNum]);
			} else {
				f->YList[idx + j] = 0;
			}
		}
		idx += cdim;
	}
	return true;
}

static Int32 render_point(Int32 x0, Int32 y0, Int32 x1, Int32 y1, Int32 X) {
	Int32 dy = y1 - y0, adx = x1 - x0;
	Int32 ady = Math_AbsI(dy);
	Int32 err = ady * (X - x0);
	Int32 off = err / adx;

	if (dy < 0) {
		return y0 - off;
	} else {
		return y0 + off;
	}
}

static void render_line(Int32 x0, Int32 y0, Int32 x1, Int32 y1, Int32* v) {
	Int32 dy = y1 - y0, adx = x1 - x0;
	Int32 ady = Math_AbsI(dy);
	Int32 base = dy / adx, sy;
	Int32 x = x0, y = y0, err = 0;

	if (dy < 0) {
		sy = base - 1;
	} else {
		sy = base + 1;
	}

	ady = ady - Math_AbsI(base) * adx;
	v[x] = y;

	for (x = x0 + 1; x < x1; x++) {
		err = err + ady;
		if (err >= adx) {
			err = err - adx;
			y = y + sy;
		} else {
			y = y + base;
		}
		v[x] = y;
	}
}

static Int32 low_neighbor(Int16* v, Int32 x) {
	Int32 n = 0, i, max = Int32_MinValue;
	for (i = 0; i < x; i++) {
		if (v[i] < v[x] && v[i] > max) { n = i; max = v[i]; }
	}
	return n;
}

static Int32 high_neighbor(Int16* v, Int32 x) {
	Int32 n = 0, i, min = Int32_MaxValue;
	for (i = 0; i < x; i++) {
		if (v[i] > v[x] && v[i] < min) { n = i; min = v[i]; }
	}
	return n;
}

static Real32 floor1_inverse_dB_table[256];
static void Floor_Synthesis(struct VorbisState* ctx, struct Floor* f, Real32* data) {
	/* amplitude value synthesis */
	Int32 YFinal[FLOOR_MAX_VALUES];
	bool Step2[FLOOR_MAX_VALUES];

	Step2[0] = true;
	Step2[1] = true;
	YFinal[0] = f->YList[0];
	YFinal[1] = f->YList[1];

	Int32 i;
	for (i = 2; i < f->Values; i++) {
		Int32 lo_offset = low_neighbor(f->XList, i);
		Int32 hi_offset = high_neighbor(f->XList, i);

		Int32 predicted = render_point(f->XList[lo_offset], YFinal[lo_offset],
			f->XList[hi_offset], YFinal[hi_offset], f->XList[i]);

		Int32 val = f->YList[i];
		Int32 highroom = f->Range - predicted;
		Int32 lowroom = predicted, room;

		if (highroom < lowroom) {
			room = highroom * 2;
		} else {
			room = lowroom * 2;
		}

		if (val) {
			Step2[lo_offset] = true;
			Step2[hi_offset] = true;
			Step2[i] = true;

			if (val >= room) {
				if (highroom > lowroom) {
					YFinal[i] = val - lowroom + predicted;
				} else {
					YFinal[i] = predicted - val + highroom - 1;
				}
			} else {
				if (val & 1) {
					YFinal[i] = predicted - (val + 1) / 2;
				} else {
					YFinal[i] = predicted + val / 2;
				}
			}
		} else {
			Step2[i] = false;
			YFinal[i] = predicted;
		}
	}

	/* curve synthesis */
	Int32 hx = 0, lx = 0, hy, rawI;
	Int32 ly = YFinal[f->ListOrder[0]] * f->Multiplier;

	for (rawI = 1; rawI < f->Values; rawI++) {
		Int32 i = f->ListOrder[rawI];
		if (!Step2[i]) continue;

		hx = f->XList[i]; hy = YFinal[i] * f->Multiplier;
		render_line(lx, ly, hx, hy, floor);
		lx = hx; ly = hy;
	}

	if (hx < ctx->DataSize) {
		render_line(hx, hy, ctx->DataSize, hy, floor);
	}


28   15) for each scalar in vector [floor], perform a lookup substitution using 
29       the scalar value from [floor] as an offset into the vector [floor1_inverse_dB_static_table]
30  
31   16) done
}


/*########################################################################################################################*
*----------------------------------------------------Vorbis residues------------------------------------------------------*
*#########################################################################################################################*/
#define RESIDUE_MAX_CLASSIFICATIONS 65
struct Residue {
	UInt8 Type, Classifications, Classbook;
	UInt32 Begin, End, PartitionSize;
	UInt8 Cascade[RESIDUE_MAX_CLASSIFICATIONS];
	Int16 Books[RESIDUE_MAX_CLASSIFICATIONS][8];
};

static ReturnCode Residue_DecodeSetup(struct VorbisState* ctx, struct Residue* r, UInt16 type) {
	r->Type  = type;
	r->Begin = Vorbis_ReadBits(ctx, 24);
	r->End   = Vorbis_ReadBits(ctx, 24);
	r->PartitionSize   = Vorbis_ReadBits(ctx, 24); r->PartitionSize++;
	r->Classifications = Vorbis_ReadBits(ctx, 6);  r->Classifications++;
	r->Classbook = Vorbis_ReadBits(ctx, 8);

	Int32 i;
	for (i = 0; i < r->Classifications; i++) {
		r->Cascade[i] = Vorbis_ReadBits(ctx, 3);
		Int32 moreBits = Vorbis_ReadBits(ctx, 1);
		if (!moreBits) continue;
		Int32 bits = Vorbis_ReadBits(ctx, 5);
		r->Cascade[i] |= bits << 3;
	}

	Int32 j;
	for (i = 0; i < r->Classifications; i++) {
		for (j = 0; j < 8; j++) {
			Int16 codebook = -1;
			if (r->Cascade[i] & (1 << j)) {
				codebook = Vorbis_ReadBits(ctx, 8);
			}
			r->Books[i][j] = codebook;
		}
	}
	return 0;
}

static void Residue_DecodeFrame(struct VorbisState* ctx, struct Residue* r, Int32 ch, bool* doNotDecode) {
	UInt32 size = ctx->DataSize;
	if (r->Type == 2) size *= ch;

	UInt32 residueBeg = max(r->Begin, size);
	UInt32 residueEnd = max(r->End,   size);

	struct Codebook* classbook = &ctx->Codebooks[r->Classbook];
	UInt32 classwordsPerCodeword = classbook->Dimensions;
	UInt32 pass, i, j, nToRead = residueEnd - residueBeg;
	UInt32 partitionsToRead = nToRead / r->PartitionSize;
	UInt8* classifications[VORBIS_MAX_CHANS];

/* TODO:  allocate and zero all vectors that will be returned. */
	if (nToRead == 0) return;
	for (pass = 0; pass < 8; pass++) {
		UInt32 partitionCount = 0;
		while (partitionCount < partitionsToRead) {

			/* read classifications in pass 0 */
			if (pass == 0) {
				for (j = 0; j < ch; j++) {
					if (doNotDecode[j]) continue;

					UInt32 temp = Codebook_DecodeScalar(ctx, classbook);
					/* TODO: i must be signed, otherwise infinite loop */
					for (i = classwordsPerCodeword - 1; i >= 0; i--) {
						classifications[j][i + partitionCount] = temp % r->Classifications;
						temp /= r->Classifications;
					}
				}
			}

			for (i = 0; i < classwordsPerCodeword && partitionCount < partitionsToRead; i++) {
				for (j = 0; j < ch; j++) {
					if (doNotDecode[j]) continue;

					UInt8 class = classifications[j][partitionCount];
					Int16 book = r->Books[class][pass];

					if (book >= 0) {
						UInt32 offset = residueBeg + partitionCount * r->PartitionSize;
						Real32* vectors = Codebook_DecodeVectors(ctx, &ctx->Codebooks[book]);
						/* TODO: decode partition into output vector number[j]; */
					}
				}
				partitionCount++;
			}
		}
	}
}


/*########################################################################################################################*
*----------------------------------------------------Vorbis mappings------------------------------------------------------*
*#########################################################################################################################*/
#define MAPPING_MAX_COUPLINGS 256
#define MAPPING_MAX_SUBMAPS 15
struct Mapping {
	UInt8 CouplingSteps, Submaps;
	UInt8 Mux[VORBIS_MAX_CHANS];
	UInt8 FloorIdx[MAPPING_MAX_SUBMAPS];
	UInt8 ResidueIdx[MAPPING_MAX_SUBMAPS];
	UInt8 Magnitude[MAPPING_MAX_COUPLINGS];
	UInt8 Angle[MAPPING_MAX_COUPLINGS];
};

static ReturnCode Mapping_DecodeSetup(struct VorbisState* ctx, struct Mapping* m) {
	Int32 i, submaps = 1, submapFlag = Vorbis_ReadBits(ctx, 1);
	if (submapFlag) {
		submaps = Vorbis_ReadBits(ctx, 4); submaps++;
	}

	Int32 couplingSteps = 0, couplingFlag = Vorbis_ReadBits(ctx, 1);
	if (couplingFlag) {
		couplingSteps = Vorbis_ReadBits(ctx, 8); couplingSteps++;
		/* TODO: How big can couplingSteps ever really get in practice? */
		Int32 couplingBits = iLog(ctx->Channels - 1);
		for (i = 0; i < couplingSteps; i++) {
			m->Magnitude[i] = Vorbis_ReadBits(ctx, couplingBits);
			m->Angle[i]     = Vorbis_ReadBits(ctx, couplingBits);
			if (m->Magnitude[i] == m->Angle[i]) return VORBIS_ERR_MAPPING_CHANS;
		}
	}

	Int32 reserved = Vorbis_ReadBits(ctx, 2);
	if (reserved != 0) return VORBIS_ERR_MAPPING_RESERVED;
	m->Submaps = submaps;
	m->CouplingSteps = couplingSteps;

	if (submaps > 1) {
		for (i = 0; i < ctx->Channels; i++) {
			m->Mux[i] = Vorbis_ReadBits(ctx, 4);
		}
	} else {
		for (i = 0; i < ctx->Channels; i++) {
			m->Mux[i] = 0;
		}
	}

	for (i = 0; i < submaps; i++) {
		Vorbis_ReadBits(ctx, 8); /* time value */
		m->FloorIdx[i]   = Vorbis_ReadBits(ctx, 8);
		m->ResidueIdx[i] = Vorbis_ReadBits(ctx, 8);
	}
	return 0;
}


/*########################################################################################################################*
*-----------------------------------------------------Vorbis setup--------------------------------------------------------*
*#########################################################################################################################*/
struct Mode { UInt8 BlockSizeFlag, MappingIdx; };
static ReturnCode Mode_DecodeSetup(struct VorbisState* ctx, struct Mode* m) {
	m->BlockSizeFlag = Vorbis_ReadBits(ctx, 1);
	UInt16 windowType = Vorbis_ReadBits(ctx, 16);
	if (windowType != 0) return VORBIS_ERR_MODE_WINDOW;

	UInt16 transformType = Vorbis_ReadBits(ctx, 16);
	if (transformType != 0) return VORBIS_ERR_MODE_TRANSFORM;
	m->MappingIdx = Vorbis_ReadBits(ctx, 8);
	return 0;
}

void Vorbis_Init(struct VorbisState* ctx, struct Stream* source) {
	Platform_MemSet(ctx, 0, sizeof(struct VorbisState));
	ctx->Source = source;
}

/* TODO: Work out Vorbis_Free implementation */

static bool Vorbis_ValidBlockSize(UInt32 blockSize) {
	return blockSize >= 64 && blockSize <= 8192 && Math_IsPowOf2(blockSize);
}

static ReturnCode Vorbis_DecodeHeader(struct VorbisState* ctx, UInt8 type) {
	UInt8 header[7];
	Stream_Read(ctx->Source, header, sizeof(header));
	if (header[0] != type) return VORBIS_ERR_WRONG_HEADER;

	bool OK = 
		header[1] == 'v' && header[2] == 'o' && header[3] == 'r' &&
		header[4] == 'b' && header[5] == 'i' && header[6] == 's';
	return OK ? 0 : ReturnCode_InvalidArg;
}

static ReturnCode Vorbis_DecodeIdentifier(struct VorbisState* ctx) {
	UInt8 header[23];
	Stream_Read(ctx->Source, header, sizeof(header));
	UInt32 version    = Stream_GetU32_LE(&header[0]);
	if (version != 0) return VORBIS_ERR_VERSION;

	ctx->Channels   = header[4];
	ctx->SampleRate = Stream_GetU32_LE(&header[5]);
	/* (12) bitrate_maximum, nominal, minimum */
	ctx->BlockSizes[0] = 1 << (header[21] & 0xF);
	ctx->BlockSizes[1] = 1 << (header[21] >>  4);

	if (!Vorbis_ValidBlockSize(ctx->BlockSizes[0])) return VORBIS_ERR_BLOCKSIZE;
	if (!Vorbis_ValidBlockSize(ctx->BlockSizes[1])) return VORBIS_ERR_BLOCKSIZE;
	if (ctx->BlockSizes[0] > ctx->BlockSizes[1])    return VORBIS_ERR_BLOCKSIZE;

	if (ctx->Channels == 0 || ctx->Channels > VORBIS_MAX_CHANS) return VORBIS_ERR_CHANS;
	/* check framing flag */
	return (header[22] & 1) ? 0 : VORBIS_ERR_FRAMING;
}

static ReturnCode Vorbis_DecodeComments(struct VorbisState* ctx) {
	UInt32 vendorLen = Stream_ReadU32_LE(ctx->Source);
	Stream_Skip(ctx->Source, vendorLen);

	UInt32 i, comments = Stream_ReadU32_LE(ctx->Source);
	for (i = 0; i < comments; i++) {
		UInt32 commentLen = Stream_ReadU32_LE(ctx->Source);
		Stream_Skip(ctx->Source, commentLen);
	}

	/* check framing flag */
	return (Stream_ReadU8(ctx->Source) & 1) ? 0 : VORBIS_ERR_FRAMING;
}

static ReturnCode Vorbis_DecodeSetup(struct VorbisState* ctx) {
	Int32 i, count;
	ReturnCode result;

	count = Vorbis_ReadBits(ctx, 8); count++;
	ctx->Codebooks = Platform_MemAlloc(count, sizeof(struct Codebook), "vorbis codebooks");
	for (i = 0; i < count; i++) {
		result = Codebook_DecodeSetup(ctx, &ctx->Codebooks[i]);
		if (result) return result;
	}

	count = Vorbis_ReadBits(ctx, 6); count++;
	for (i = 0; i < count; i++) {
		UInt16 time = Vorbis_ReadBits(ctx, 16);
		if (time != 0) return VORBIS_ERR_TIME_TYPE;
	}

	count = Vorbis_ReadBits(ctx, 6); count++;
	ctx->Floors = Platform_MemAlloc(count, sizeof(struct Floor), "vorbis floors");
	for (i = 0; i < count; i++) {
		UInt16 floor = Vorbis_ReadBits(ctx, 16);
		if (floor != 1) return VORBIS_ERR_FLOOR_TYPE;
		result = Floor_DecodeSetup(ctx, &ctx->Floors[i]);
		if (result) return result;
	}

	count = Vorbis_ReadBits(ctx, 6); count++;
	ctx->Residues = Platform_MemAlloc(count, sizeof(struct Residue), "vorbis residues");
	for (i = 0; i < count; i++) {
		UInt16 residue = Vorbis_ReadBits(ctx, 16);
		if (residue > 2) return VORBIS_ERR_FLOOR_TYPE;
		result = Residue_DecodeSetup(ctx, &ctx->Residues[i], residue);
		if (result) return result;
	}

	count = Vorbis_ReadBits(ctx, 6); count++;
	ctx->Mappings = Platform_MemAlloc(count, sizeof(struct Mapping), "vorbis mappings");
	for (i = 0; i < count; i++) {
		UInt16 mapping = Vorbis_ReadBits(ctx, 16);
		if (mapping != 0) return VORBIS_ERR_MAPPING_TYPE;
		result = Mapping_DecodeSetup(ctx, &ctx->Mappings[i]);
		if (result) return result;
	}

	count = Vorbis_ReadBits(ctx, 6); count++;
	ctx->Modes = Platform_MemAlloc(count, sizeof(struct Mode), "vorbis modes");
	for (i = 0; i < count; i++) {
		result = Mode_DecodeSetup(ctx, &ctx->Modes[i]);
		if (result) return result;
	}
	
	ctx->ModeNumBits = iLog(count - 1); /* ilog([vorbis_mode_count]-1) bits */
	UInt8 framing = Vorbis_ReadBits(ctx, 1);
	Vorbis_AlignBits(ctx);
	/* check framing flag */
	return (framing & 1) ? 0 : VORBIS_ERR_FRAMING;
}

enum VORBIS_PACKET { VORBIS_IDENTIFIER = 1, VORBIS_COMMENTS = 3, VORBIS_SETUP = 5, };
ReturnCode Vorbis_DecodeHeaders(struct VorbisState* ctx) {
	ReturnCode result;
	
	result = Vorbis_DecodeHeader(ctx, VORBIS_IDENTIFIER);
	if (result) return result;
	result = Vorbis_DecodeIdentifier(ctx);
	if (result) return result;

	result = Vorbis_DecodeHeader(ctx, VORBIS_COMMENTS);
	if (result) return result;
	result = Vorbis_DecodeComments(ctx);
	if (result) return result;

	result = Vorbis_DecodeHeader(ctx, VORBIS_SETUP);
	if (result) return result;
	result = Vorbis_DecodeSetup(ctx);
	if (result) return result;

	return 0;
}


/*########################################################################################################################*
*-----------------------------------------------------Vorbis frame--------------------------------------------------------*
*#########################################################################################################################*/
ReturnCode Vorbis_DecodeFrame(struct VorbisState* ctx) {
	Int32 i, j, packetType = Vorbis_ReadBits(ctx, 1);
	if (packetType) return VORBIS_ERR_FRAME_TYPE;

	Int32 modeIdx = Vorbis_ReadBits(ctx, ctx->ModeNumBits);
	struct Mode* mode = &ctx->Modes[modeIdx];
	struct Mapping* mapping = &ctx->Mappings[mode->MappingIdx];

	/* decode window shape */
	ctx->CurBlockSize = ctx->BlockSizes[mode->BlockSizeFlag];
	ctx->DataSize = ctx->CurBlockSize / 2;
	Int32 prev_window, next_window;
	/* long window lapping*/
	if (mode->BlockSizeFlag) {
		prev_window = Vorbis_ReadBits(ctx, 1);
		next_window = Vorbis_ReadBits(ctx, 1);
	}	

	ctx->Values = Platform_MemAllocCleared(ctx->Channels * ctx->DataSize, sizeof(Real32), "audio values");

	/* decode floor */
	bool hasFloor[VORBIS_MAX_CHANS], hasResidue[VORBIS_MAX_CHANS];
	for (i = 0; i < ctx->Channels; i++) {
		Int32 submap = mapping->Mux[i];
		Int32 floorIdx = mapping->FloorIdx[submap];
		hasFloor[i] = Floor_DecodeFrame(ctx, &ctx->Floors[floorIdx]);
		hasResidue[i] = hasFloor[i];
	}

	/* non-zero vector propogate */
	for (i = 0; i < mapping->CouplingSteps; i++) {
		Int32 magChannel = mapping->Magnitude[i];
		Int32 angChannel = mapping->Angle[i];

		if (hasResidue[magChannel] || hasResidue[angChannel]) {
			hasResidue[magChannel] = true; hasResidue[angChannel] = true;
		}
	}

	/* decode residue */
	for (i = 0; i < mapping->Submaps; i++) {
		Int32 ch = 0;
		bool doNotDecode[VORBIS_MAX_CHANS];
		for (j = 0; j < ctx->Channels; j++) {
			if (mapping->Mux[j] != i) continue;

			doNotDecode[ch] = !hasResidue[j];
			ch++;
		}

		Int32 residueIdx = mapping->FloorIdx[i];
		Residue_DecodeFrame(ctx, &ctx->Residues[residueIdx], ch, doNotDecode);
		ch = 0;

		for (j = 0; j < ctx->Channels; j++) {
			if (mapping->Mux[j] != i) continue;
			/* TODO: residue vector for channel[j] is set to decoded residue vector[ch] */
			ch++;
		}
	}

	/* inverse coupling */
	for (i = mapping->CouplingSteps - 1; i >= 0; i--) {
		Real32* magValues = Vorbis_ChanData(ctx, mapping->Magnitude[i]);
		Real32* angValues = Vorbis_ChanData(ctx, mapping->Angle[i]);

		for (j = 0; j < ctx->DataSize; j++) {
			Real32 m = magValues[i], a = angValues[i];

			if (m > 0.0f) {
				if (a > 0.0f) { angValues[j] = m - a; }
				else {
					angValues[j] = m;
					magValues[j] = m + a;
				}
			} else {
				if (a > 0.0f) { angValues[j] = m + a; }
				else {
					angValues[j] = m;
					magValues[j] = m - a;
				}
			}
		}
	}

	/* compute dot product of floor and residue, producing audio spectrum vector */

	/* inverse monolithic transform of audio spectrum vector */

}

static Real32 floor1_inverse_dB_table[256] = {
	1.0649863e-07f, 1.1341951e-07f, 1.2079015e-07f, 1.2863978e-07f,
	1.3699951e-07f, 1.4590251e-07f, 1.5538408e-07f, 1.6548181e-07f,
	1.7623575e-07f, 1.8768855e-07f, 1.9988561e-07f, 2.1287530e-07f,
	2.2670913e-07f, 2.4144197e-07f, 2.5713223e-07f, 2.7384213e-07f,
	2.9163793e-07f, 3.1059021e-07f, 3.3077411e-07f, 3.5226968e-07f,
	3.7516214e-07f, 3.9954229e-07f, 4.2550680e-07f, 4.5315863e-07f,
	4.8260743e-07f, 5.1396998e-07f, 5.4737065e-07f, 5.8294187e-07f,
	6.2082472e-07f, 6.6116941e-07f, 7.0413592e-07f, 7.4989464e-07f,
	7.9862701e-07f, 8.5052630e-07f, 9.0579828e-07f, 9.6466216e-07f,
	1.0273513e-06f, 1.0941144e-06f, 1.1652161e-06f, 1.2409384e-06f,
	1.3215816e-06f, 1.4074654e-06f, 1.4989305e-06f, 1.5963394e-06f,
	1.7000785e-06f, 1.8105592e-06f, 1.9282195e-06f, 2.0535261e-06f,
	2.1869758e-06f, 2.3290978e-06f, 2.4804557e-06f, 2.6416497e-06f,
	2.8133190e-06f, 2.9961443e-06f, 3.1908506e-06f, 3.3982101e-06f,
	3.6190449e-06f, 3.8542308e-06f, 4.1047004e-06f, 4.3714470e-06f,
	4.6555282e-06f, 4.9580707e-06f, 5.2802740e-06f, 5.6234160e-06f,
	5.9888572e-06f, 6.3780469e-06f, 6.7925283e-06f, 7.2339451e-06f,
	7.7040476e-06f, 8.2047000e-06f, 8.7378876e-06f, 9.3057248e-06f,
	9.9104632e-06f, 1.0554501e-05f, 1.1240392e-05f, 1.1970856e-05f,
	1.2748789e-05f, 1.3577278e-05f, 1.4459606e-05f, 1.5399272e-05f,
	1.6400004e-05f, 1.7465768e-05f, 1.8600792e-05f, 1.9809576e-05f,
	2.1096914e-05f, 2.2467911e-05f, 2.3928002e-05f, 2.5482978e-05f,
	2.7139006e-05f, 2.8902651e-05f, 3.0780908e-05f, 3.2781225e-05f,
	3.4911534e-05f, 3.7180282e-05f, 3.9596466e-05f, 4.2169667e-05f,
	4.4910090e-05f, 4.7828601e-05f, 5.0936773e-05f, 5.4246931e-05f,
	5.7772202e-05f, 6.1526565e-05f, 6.5524908e-05f, 6.9783085e-05f,
	7.4317983e-05f, 7.9147585e-05f, 8.4291040e-05f, 8.9768747e-05f,
	9.5602426e-05f, 0.00010181521f, 0.00010843174f, 0.00011547824f,
	0.00012298267f, 0.00013097477f, 0.00013948625f, 0.00014855085f,
	0.00015820453f, 0.00016848555f, 0.00017943469f, 0.00019109536f,
	0.00020351382f, 0.00021673929f, 0.00023082423f, 0.00024582449f,
	0.00026179955f, 0.00027881276f, 0.00029693158f, 0.00031622787f,
	0.00033677814f, 0.00035866388f, 0.00038197188f, 0.00040679456f,
	0.00043323036f, 0.00046138411f, 0.00049136745f, 0.00052329927f,
	0.00055730621f, 0.00059352311f, 0.00063209358f, 0.00067317058f,
	0.00071691700f, 0.00076350630f, 0.00081312324f, 0.00086596457f,
	0.00092223983f, 0.00098217216f, 0.0010459992f,  0.0011139742f,
	0.0011863665f,  0.0012634633f,  0.0013455702f,  0.0014330129f,
	0.0015261382f,  0.0016253153f,  0.0017309374f,  0.0018434235f,
	0.0019632195f,  0.0020908006f,  0.0022266726f,  0.0023713743f,
	0.0025254795f,  0.0026895994f,  0.0028643847f,  0.0030505286f,
	0.0032487691f,  0.0034598925f,  0.0036847358f,  0.0039241906f,
	0.0041792066f,  0.0044507950f,  0.0047400328f,  0.0050480668f,
	0.0053761186f,  0.0057254891f,  0.0060975636f,  0.0064938176f,
	0.0069158225f,  0.0073652516f,  0.0078438871f,  0.0083536271f,
	0.0088964928f,  0.009474637f,   0.010090352f,   0.010746080f,
	0.011444421f,   0.012188144f,   0.012980198f,   0.013823725f,
	0.014722068f,   0.015678791f,   0.016697687f,   0.017782797f,
	0.018938423f,   0.020169149f,   0.021479854f,   0.022875735f,
	0.024362330f,   0.025945531f,   0.027631618f,   0.029427276f,
	0.031339626f,   0.033376252f,   0.035545228f,   0.037855157f,
	0.040315199f,   0.042935108f,   0.045725273f,   0.048696758f,
	0.051861348f,   0.055231591f,   0.058820850f,   0.062643361f,
	0.066714279f,   0.071049749f,   0.075666962f,   0.080584227f,
	0.085821044f,   0.091398179f,   0.097337747f,   0.10366330f,
	0.11039993f,    0.11757434f,    0.12521498f,    0.13335215f,
	0.14201813f,    0.15124727f,    0.16107617f,    0.17154380f,
	0.18269168f,    0.19456402f,    0.20720788f,    0.22067342f,
	0.23501402f,    0.25028656f,    0.26655159f,    0.28387361f,
	0.30232132f,    0.32196786f,    0.34289114f,    0.36517414f,
	0.38890521f,    0.41417847f,    0.44109412f,    0.46975890f,
	0.50028648f,    0.53279791f,    0.56742212f,    0.60429640f,
	0.64356699f,    0.68538959f,    0.72993007f,    0.77736504f,
	0.82788260f,    0.88168307f,    0.9389798f,     1.00000000f,
};