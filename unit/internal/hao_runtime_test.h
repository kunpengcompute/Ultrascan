#ifndef HAO_RUNTIME_TEST_H
#define HAO_RUNTIME_TEST_H

#include "fdr/hao_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

struct HAORuntimeInspectSummary {
    u32 keyBits;
    u32 primaryCount;
    u32 primaryBitmapSize;
    u32 l2EntryCount;
    u32 ruleMetaCount;
    u32 nonEmptyPrimary;
    u32 multiEntryBucketCount;
    u32 maxEntriesPerKey;
    u32 totalRulesInL2;
};

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

#ifdef __cplusplus
}
#endif

#endif
