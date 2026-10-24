// Pixel kernels for the CPS3 renderer.
//
// Each kernel has a plain C version, which is the reference, and an ARM NEON
// one used when the compiler targets NEON (ARMv7 with -mfpu=neon, AArch64).
// Both must give identical results. Define CPS3_NO_SIMD to build the C
// versions only, e.g. to compare the output.

#ifndef CPS3_SIMD_H
#define CPS3_SIMD_H

#if defined(__ARM_NEON) && !defined(CPS3_NO_SIMD)
#include <arm_neon.h>
#define CPS3_NEON 1
#endif

// non-zero if one of the 4 bytes of v is 0
#define CPS3_HAS_ZERO_BYTE(v)	(((v) - 0x01010101) & ~(v) & 0x80808080)

// One line of 16 pixels from character RAM, where pixel i of the line is byte
// i ^ 3 (bytes are stored in host-endian longs), to the render buffer. Non-zero
// pixels become pixel | pal, zero is transparent. With flipx the line is drawn
// right to left. src must be 4-byte aligned.
//
// Almost all tile lines are either fully transparent or fully opaque, so
// those skip reading the render buffer.
static inline void cps3_line16(UINT32 *dst, const UINT8 *src, UINT32 pal, INT32 flipx)
{
	const UINT32 *w = (const UINT32 *)src;
	if ((w[0] | w[1] | w[2] | w[3]) == 0) return;
	INT32 opaque_all = !(CPS3_HAS_ZERO_BYTE(w[0]) | CPS3_HAS_ZERO_BYTE(w[1]) |
		CPS3_HAS_ZERO_BYTE(w[2]) | CPS3_HAS_ZERO_BYTE(w[3]));

#if defined(CPS3_NEON)
	uint8x16_t px = vrev32q_u8(vld1q_u8(src));					// pixel order
	if (flipx) {
		px = vrev64q_u8(px);
		px = vcombine_u8(vget_high_u8(px), vget_low_u8(px));
	}

	uint16x8_t px_lo = vmovl_u8(vget_low_u8(px));
	uint16x8_t px_hi = vmovl_u8(vget_high_u8(px));
	uint32x4_t vpal = vdupq_n_u32(pal);

	uint32x4_t p0 = vorrq_u32(vmovl_u16(vget_low_u16(px_lo)), vpal);
	uint32x4_t p1 = vorrq_u32(vmovl_u16(vget_high_u16(px_lo)), vpal);
	uint32x4_t p2 = vorrq_u32(vmovl_u16(vget_low_u16(px_hi)), vpal);
	uint32x4_t p3 = vorrq_u32(vmovl_u16(vget_high_u16(px_hi)), vpal);

	if (opaque_all) {
		vst1q_u32(dst +  0, p0);
		vst1q_u32(dst +  4, p1);
		vst1q_u32(dst +  8, p2);
		vst1q_u32(dst + 12, p3);
		return;
	}

	uint8x16_t opaque = vtstq_u8(px, px);						// 0xff where drawn
	int16x8_t op_lo = vmovl_s8(vreinterpret_s8_u8(vget_low_u8(opaque)));
	int16x8_t op_hi = vmovl_s8(vreinterpret_s8_u8(vget_high_u8(opaque)));
	uint32x4_t m0 = vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(op_lo)));
	uint32x4_t m1 = vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(op_lo)));
	uint32x4_t m2 = vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(op_hi)));
	uint32x4_t m3 = vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(op_hi)));

	vst1q_u32(dst +  0, vbslq_u32(m0, p0, vld1q_u32(dst +  0)));
	vst1q_u32(dst +  4, vbslq_u32(m1, p1, vld1q_u32(dst +  4)));
	vst1q_u32(dst +  8, vbslq_u32(m2, p2, vld1q_u32(dst +  8)));
	vst1q_u32(dst + 12, vbslq_u32(m3, p3, vld1q_u32(dst + 12)));
#else
	if (opaque_all) {
		if (flipx) {
			for (INT32 i = 0; i < 16; i++)
				dst[15 - i] = src[i ^ 3] | pal;
		} else {
			for (INT32 i = 0; i < 16; i++)
				dst[i] = src[i ^ 3] | pal;
		}
	} else if (flipx) {
		for (INT32 i = 0; i < 16; i++)
			if (src[i ^ 3]) dst[15 - i] = src[i ^ 3] | pal;
	} else {
		for (INT32 i = 0; i < 16; i++)
			if (src[i ^ 3]) dst[i] = src[i ^ 3] | pal;
	}
#endif
}

// One line of the render buffer through the palette to the output, clearing
// the render buffer for the next frame. Eight pixels at a time so the palette
// reads don't wait on each other.
static inline void cps3_copy_line(UINT16 *dst, UINT32 *src, INT32 n, const UINT16 *pal)
{
	INT32 i = 0;
	for (; i + 8 <= n; i += 8) {
#if defined(__GNUC__)
		__builtin_prefetch(src + i + 64);
#endif
		UINT32 s0 = src[i + 0], s1 = src[i + 1], s2 = src[i + 2], s3 = src[i + 3];
		UINT32 s4 = src[i + 4], s5 = src[i + 5], s6 = src[i + 6], s7 = src[i + 7];
		UINT16 c0 = pal[s0], c1 = pal[s1], c2 = pal[s2], c3 = pal[s3];
		UINT16 c4 = pal[s4], c5 = pal[s5], c6 = pal[s6], c7 = pal[s7];
		dst[i + 0] = c0; dst[i + 1] = c1; dst[i + 2] = c2; dst[i + 3] = c3;
		dst[i + 4] = c4; dst[i + 5] = c5; dst[i + 6] = c6; dst[i + 7] = c7;
	}
	for (; i < n; i++)
		dst[i] = pal[src[i]];

	memset(src, 0, n * sizeof(UINT32));
}

#endif // CPS3_SIMD_H
