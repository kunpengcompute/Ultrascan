#ifndef HAO_RUNTIME_TEST_H
#define HAO_RUNTIME_TEST_H

#include "hao_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

hwlm_error_t HaoEngineExecNaiveForTest(const struct FDR *fdr,
                                       const struct FDR_Runtime_Args *a,
                                       hwlm_group_t control);
u32 HaoRuntimeBitmapProbeMaskForTest(const u8 *bitmap, u32 bitmapSize,
                                     const u32 *primaryIdx, u32 laneCount,
                                     int usePacked);
u64a HaoRuntimeRawLaneWordForTest(const u8 *prev32, const u8 *curr32,
                                  u32 lane);
int HaoRuntimeValidateLayoutForTest(const void *blob, u32 blobSize);
int HaoRuntimeInspectBlobForTest(const void *blob, u32 blobSize,
                                 struct HAORuntimeInspectSummary *summary);
void HaoRuntimeResetStatsForTest(void);
void HaoRuntimeGetStatsForTest(struct HAORuntimeStats *summary);
int HaoRuntimeStatsEnabledForTest(void);
hwlm_error_t HaoEngineExecBlobNaiveForTest(const void *blob, u32 blobSize,
                                           const struct FDR_Runtime_Args *a,
                                           hwlm_group_t control);
hwlm_error_t HaoEngineExecBlobBatchForTest(const void *blob, u32 blobSize,
                                           const struct FDR_Runtime_Args *a,
                                           hwlm_group_t control);

#ifdef __cplusplus
}
#endif

#endif // HAO_RUNTIME_TEST_H
