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

// One line of 16 pixels from character RAM, where pixel i of the line is byte
// i ^ 3 (bytes are stored in host-endian longs), to the render buffer. Non-zero
// pixels become pixel | pal, zero is transparent. With flipx the line is drawn
// right to left.
static inline void cps3_line16(UINT32 *dst, const UINT8 *src, UINT32 pal, INT32 flipx)
{
#if defined(CPS3_NEON)
	uint8x16_t px = vrev32q_u8(vld1q_u8(src));					// pixel order
	if (flipx) {
		px = vrev64q_u8(px);
		px = vcombine_u8(vget_high_u8(px), vget_low_u8(px));
	}
	uint8x16_t opaque = vtstq_u8(px, px);						// 0xff where drawn

	uint16x8_t px_lo = vmovl_u8(vget_low_u8(px));
	uint16x8_t px_hi = vmovl_u8(vget_high_u8(px));
	int16x8_t op_lo = vmovl_s8(vreinterpret_s8_u8(vget_low_u8(opaque)));
	int16x8_t op_hi = vmovl_s8(vreinterpret_s8_u8(vget_high_u8(opaque)));
	uint32x4_t vpal = vdupq_n_u32(pal);

	uint32x4_t p0 = vorrq_u32(vmovl_u16(vget_low_u16(px_lo)), vpal);
	uint32x4_t p1 = vorrq_u32(vmovl_u16(vget_high_u16(px_lo)), vpal);
	uint32x4_t p2 = vorrq_u32(vmovl_u16(vget_low_u16(px_hi)), vpal);
	uint32x4_t p3 = vorrq_u32(vmovl_u16(vget_high_u16(px_hi)), vpal);
	uint32x4_t m0 = vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(op_lo)));
	uint32x4_t m1 = vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(op_lo)));
	uint32x4_t m2 = vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(op_hi)));
	uint32x4_t m3 = vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(op_hi)));

	vst1q_u32(dst +  0, vbslq_u32(m0, p0, vld1q_u32(dst +  0)));
	vst1q_u32(dst +  4, vbslq_u32(m1, p1, vld1q_u32(dst +  4)));
	vst1q_u32(dst +  8, vbslq_u32(m2, p2, vld1q_u32(dst +  8)));
	vst1q_u32(dst + 12, vbslq_u32(m3, p3, vld1q_u32(dst + 12)));
#else
	if (flipx) {
		for (INT32 i = 0; i < 16; i++)
			if (src[i ^ 3]) dst[15 - i] = src[i ^ 3] | pal;
	} else {
		for (INT32 i = 0; i < 16; i++)
			if (src[i ^ 3]) dst[i] = src[i ^ 3] | pal;
	}
#endif
}

#endif // CPS3_SIMD_H
