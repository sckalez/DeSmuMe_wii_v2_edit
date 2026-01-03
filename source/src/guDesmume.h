#ifndef __GUDESMUME_H__
#define __GUDESMUME_H__

#include <gctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ENABLE_PAIRED_SINGLE

void ps_MatrixMultVec4x4(const float *matrix, float *vecPtr);
void ps_MatrixMultVec3x3(float *matrix, float *vecPtr);
void ps_MatrixCopy(float* matrixDST, const float* matrixSRC);
void ps_MatrixTranslate(float *matrix, float *ptr);
void ps_MatrixScale(float *matrix, float *ptr);
void ps_MatrixMultiply(float* matrix, float* rightMatrix);
void ps_guMtxDesmumeTrans(f32* outST, f32* mtxCurrent, f32* coord, f32* inST);
void ps_mtx_fix2float4x4(f32* matrix, f32 divisor);
void ps_mtx_fix2float3x4(f32* matrix, f32 divisor);

#else // No paired single

static inline void c_guMtxDesmumeTrans(f32* outST, const f32* mtxCurrent, const f32* coord, const f32* inST)
{
	outST[0] = ((coord[0]*mtxCurrent[0] +
				 coord[1]*mtxCurrent[4] +
				 coord[2]*mtxCurrent[8]) + inST[0] * 16.0f) / 16.0f;
	outST[1] = ((coord[0]*mtxCurrent[1] +
				 coord[1]*mtxCurrent[5] +
				 coord[2]*mtxCurrent[9]) + inST[1] * 16.0f) / 16.0f;
	/* If callers expect outST[2]/outST[3] to be set, initialize them here */
}

#endif

#ifdef __cplusplus
}
#endif

#endif /* __GUDESMUME_H__ */
