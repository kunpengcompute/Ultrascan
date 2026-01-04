/*
 * Copyright (c) 2020-2021 Huawei Technologies Co., Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef KHSEL_TYPE_H_
#define KHSEL_TYPE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Below are common domain specific definitions
 */

#define KHSEL_VERSION_INFO_LEN 100
typedef struct {
    char productName[KHSEL_VERSION_INFO_LEN];
    char productVersion[KHSEL_VERSION_INFO_LEN];
    char componentName[KHSEL_VERSION_INFO_LEN];
    char componentVersion[KHSEL_VERSION_INFO_LEN];
    char componentAppendInfo[KHSEL_VERSION_INFO_LEN];
    char softwareName[KHSEL_VERSION_INFO_LEN];
    char softwareVersion[KHSEL_VERSION_INFO_LEN];
}KhselProVersion;

typedef enum {
    KHSEL_RND_ZERO,
    KHSEL_RND_NEAR,
    KHSEL_RND_FINANCIAL
} KhselRoundMode;

/*
 * Below are specific definitions for trade-off between performance and accuracy
 */
/*
 * The following macro defines the prioritize beteewn performance and accuracy.
 * value of KHSEL_ALGHINT_NONE is the same as KHSEL_ALGHINT_ACCURATE.
 * value of KHSEL_ALGHINT_FAST means prioritize performance.
 * value of KHSEL_ALGHINT_ACCURATE means prioritize accuracy.
 */
typedef enum {
    KHSEL_ALGHINT_NONE,
    KHSEL_ALGHINT_FAST,
    KHSEL_ALGHINT_ACCURATE
} KhselHintAlgorithm;

typedef enum {
    KHSEL_CMP_LT,
    KHSEL_CMP_LE,
    KHSEL_CMP_EQ,
    KHSEL_CMP_GE,
    KHSEL_CMP_GT
} KhselCmpOp;

typedef enum {
    KHSEL_ZCR,
    KHSEL_ZCXOR,
    KHSEL_ZCC
} KhselZCType;

/*
 * The following macro defines the algorithm type of the KHSELS FIRSR & Convolution & Correlation.
 */
typedef enum {
    KHSEL_ALG_AUTO,      // Automatic algorithm selection based on the data scale.
    KHSEL_ALG_DEFAULT,   // Direct calculation based on definition.
    KHSEL_ALG_FFT,       // Use FFT to accelerate computing.
} KhselAlgMode;

/*
 * The following macro defines the window type of the KHSELS FIRGen.
 */
typedef enum {
    KHSEL_WIN_BARTLETT,
    KHSEL_WIN_BLACKMAN,
    KHSEL_WIN_HAMMING,
    KHSEL_WIN_HANN,
} KhselWinType;

typedef enum {
    KHSEL_NORM_NORMAL,
    KHSEL_NORM_BIASED,
    KHSEL_NORM_UNBIASED,
} KhselNormMode;

/*
 * The following macro defines the algorithm type of the TopK.
 */
typedef enum {
    KHSEL_TOPK_AUTO,      // Automatic algorithm selection based on the source and destination len.
    KHSEL_TOPK_DIRECT,    // Direct calculation based on definition.
    KHSEL_TOPK_RADIX,     // Use Radix to accelerate computing.
} KhselTopKMode;

typedef enum {
    KHSEL1U,
    KHSEL8U,
    KHSEL8UC,
    KHSEL8S,
    KHSEL8SC,
    KHSEL16U,
    KHSEL16UC,
    KHSEL16S,
    KHSEL16SC,
    KHSEL32U,
    KHSEL32UC,
    KHSEL32S,
    KHSEL32SC,
    KHSEL32F,
    KHSEL32FC,
    KHSEL64U,
    KHSEL64UC,
    KHSEL64S,
    KHSEL64SC,
    KHSEL64F,
    KHSEL64FC,
    KHSELUNDEF
} KhselDataType;

typedef enum {
    KHSEL_SPCHBR_16000,
    KHSEL_SPCHBR_24000,
    KHSEL_SPCHBR_32000,
    KHSEL_SPCHBR_40000,
} KhselSpchBitRate;

typedef enum {
    KHSEL_AMRWB_6600,
    KHSEL_AMRWB_8850,
    KHSEL_AMRWB_12650,
    KHSEL_AMRWB_14250,
    KHSEL_AMRWB_15850,
    KHSEL_AMRWB_18250,
    KHSEL_AMRWB_19850,
    KHSEL_AMRWB_23050,
    KHSEL_AMRWB_23850,
} KhselAmrwbMode;

/*
 * Below are audio domain specific definitions
 */

/*
 * @brief a plan structure and pointer for Amrnb structure
 */
typedef enum {
    KHSEL_AMRNB_MR475 = 0,
    KHSEL_AMRNB_MR515,
    KHSEL_AMRNB_MR59,
    KHSEL_AMRNB_MR67,
    KHSEL_AMRNB_MR74,
    KHSEL_AMRNB_MR795,
    KHSEL_AMRNB_MR102,
    KHSEL_AMRNB_MR122,
    KHSEL_AMRNB_MRDTX
} KhselAmrnbMode;

/*
 * Below are 3D Image (Volume) Processing specific definitions
 */
/*
 * The following macro defines the status of the KHSEL operation.
 * value of 0 means on error.
 * value of 1 - 199 means common error.
 * value of 200 - 399 means common warning.
 * value of 400 - 599 means singal domain specific error.
 * value of 600 - 799 means image domain specific error.
 */
typedef enum {
    /* Definition of common error codes. Initial value is 0.       */
    KHSEL_STS_NO_ERR = 0,
    KHSEL_STS_NULL_PTR_ERR,
    KHSEL_STS_SIZE_ERR,
    KHSEL_STS_DIV_BY_ZERO_ERR,
    KHSEL_STS_LOGIC_ERR,
    KHSEL_STS_THRESHOLD_ERR,
    KHSEL_STS_THRESH_NEG_LEVEL_ERR,
    KHSEL_STS_BAD_ARG_ERR,
    KHSEL_STS_ROUND_MODEL_NOT_SUPPORTED_ERR,
    KHSEL_STS_MALLOC_FAILED,
    KHSEL_STS_PARAMETER_ERR,
    KHSEL_STS_FFT_ORDER_ERR,
    KHSEL_STS_FFT_ERR,
    KHSEL_STS_FFT_FACTOR_ERR,
    KHSEL_STS_FFT_FLAG_ERR,
    KHSEL_STS_FFT_POWER_ERR,
    KHSEL_STS_FFT_UNSUPPORTED_N_ERR,
    KHSEL_STS_MASK_SIZE_ERR,
    KHSEL_STS_RANGE_ERR,
    KHSEL_STS_SHIFT_ERR,
    KHSEL_STS_SCALE_ERR,
    KHSEL_STS_POLICY_STATE_ERR,
    KHSEL_STS_DATETYPE_ERR,
    KHSEL_STS_INT32_OVERFLOW_ERR,
    /* Definition of common warning codes. Initial value is 200.       */
    KHSEL_STS_NOT_SUPPORT = 200,
    KHSEL_STS_UNKNOWN_FEATURE,
    KHSEL_STS_OVERFLOW,
    KHSEL_STS_UNDERFLOW,
    KHSEL_STS_MISMATCH,
    /* Definition of common warning codes that will not change the code flow, start form 300. */
    KHSEL_STS_DIV_BY_ZERO = 300,
    KHSEL_STS_LN_ZERO_ARG,
    KHSEL_STS_LN_NEG_ARG,
    KHSEL_STS_SINGULARITY,
    KHSEL_STS_DOMAIN,
    KHSEL_STS_SQRT_NEG_ARG,
    KHSEL_STS_INV_ZERO,
    KHSEL_STS_EVEN_MEDIAN_MASK_SIZE,
    KHSEL_STS_DOUBLE_SIZE,
    KHSEL_STS_SIZE_WRN,
  /* Definition of signal domain error code. Initial value is 400.  */
    KHSEL_STS_SAMPLE_FACTOR_ERR = 400,
    KHSEL_STS_SAMPLE_PHASE_ERR,
    KHSEL_STS_RND_MODE_NOT_SUPPORTED_ERR,
    KHSEL_STS_FIR_LEN_ERR,
    KHSEL_STS_SPARSE_ERR,
    KHSEL_STS_DATA_TYPE_ERR,
    KHSEL_STS_ALG_TYPE_ERR,
    KHSEL_STS_FIRMR_FACTOR_ERR,
    KHSEL_STS_FIRMR_PHASE_ERR,
    KHSEL_STS_WIN_TYPE_ERR,
    KHSEL_STS_HUGEWIN_ERR,
    KHSEL_STS_REL_FREQ_ERR,
    /* Definition of image domain error code. Initial value is 600.   */
    KHSEL_STS_STEP_ERR = 600,
    KHSEL_STS_DOUBLE_SIZE_ERR,
    KHSEL_STS_NOT_EVEN_STEP_ERR,
    KHSEL_STS_ROI_ERR,
    KHSEL_STS_COI_ERR,
    KHSEL_STS_NOT_SUPPORTED_MODE_ERR,
    KHSEL_STS_NO_OPERATION,
    KHSEL_STS_OUT_OF_RANGE_ERR,
    KHSEL_STS_EXCEEDED_SIZE_ERROR,
    KHSEL_STS_CONTEXT_MATCH_ERR,
    KHSEL_STS_BORDER_ERR,
    KHSEL_STS_NUMCHANNELS_ERR,
    KHSEL_STS_HISTOLEVELS_ERR,
    KHSEL_STS_HISTLEVELS_ERR,
    KHSEL_STS_BADARG_ERR,
    KHSEL_STS_COEFF_ERR,
    KHSEL_STS_RECT_ERR,
    KHSEL_STS_INTWRPOLATION_ERR,
    KHSEL_STS_WRONG_INTERSECT_ROI_ERR,
    KHSEL_STS_WRONG_INTERSECT_QUAD,
    KHSEL_STS_QUAD_ERR,
    KHSEL_STS_SYM_KERNEL_EXPECTED,
    KHSEL_STS_INTERPOLATION_ERR,
    KHSEL_STS_WARPDIRECTION_ERR,
    KHSEL_STS_ANCHOR_ERR,
    KHSEL_STS_NOT_SUPPORTED_INPLACE_MODE_ERR,
    KHSEL_STS_NORM_ERR,
    KHSEL_STS_MIRROR_FLIP_ERR,
    KHSEL_STS_CHANNELORDER_ERR,
    /* Definition of audio domain error code. Initial value is 800.   */
    KHSEL_STS_HEAD_ERR = 800,
    KHSEL_STS_FRAME_ERR
} KhselResult;

enum {
    KHSEL_FFT_DIV_FWD_BY_N = 1,
    KHSEL_FFT_DIV_BWD_BY_N = 2,
    KHSEL_FFT_DIV_BY_SQRTN = 3,
    KHSEL_FFT_NODIV_BY_ANY = 4
};

typedef struct {
    uint64_t low;
    int64_t high;
} KhselDecimal128;

#ifdef __cplusplus
}
#endif

#endif /* KHSEL_TYPE_H__ */
