#ifndef HAO_RUNTIME_LAYOUT_H
#define HAO_RUNTIME_LAYOUT_H

#include "hao_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

int haoValidateLayout(const void *blob, u32 blobSize,
                      const struct HAORuntimeHeader **outHdr);

int haoValidateVariants(const void *blob, u32 blobSize,
                        const struct HAOVariants **out);

int haoValidateTables(const struct HAORuntimeHeader *hdr);

/* Validate native ARM matcher variants once when loading/allocating scratch. */
int haoValidateDatabase(const void *blob, u32 size);

#ifdef __cplusplus
}
#endif

#endif /* HAO_RUNTIME_LAYOUT_H */
