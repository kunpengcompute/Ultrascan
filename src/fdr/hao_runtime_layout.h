#ifndef HAO_RUNTIME_LAYOUT_H
#define HAO_RUNTIME_LAYOUT_H

#include "hao_runtime.h"

int haoValidateLayout(const void *blob, u32 blobSize,
                      const struct HAORuntimeHeader **outHdr);

#endif /* HAO_RUNTIME_LAYOUT_H */
