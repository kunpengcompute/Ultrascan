#ifndef HAO_RUNTIME_INLINE_H
#define HAO_RUNTIME_INLINE_H

#include "fdr_internal.h"
#include "hao_runtime.h"
#include "hs_compile.h"
#include "util/bitutils.h"
#include "util/compare.h"
#include "util/simd_utils.h"
#include "util/unaligned.h"

#include <string.h>

static really_inline
int haoGetByteAt(const struct FDR_Runtime_Args *a, s64a pos, u8 *out) {
    if (!a || !out) {
        return 0;
    }
    if (pos >= 0) {
        if ((u64a)pos >= a->len) {
            return 0;
        }
        *out = a->buf[pos];
        return 1;
    }

    if (!a->buf_history || !a->len_history) {
        return 0;
    }

    s64a idx = (s64a)a->len_history + pos;
    if (idx < 0 || (u64a)idx >= a->len_history) {
        return 0;
    }
    *out = a->buf_history[idx];
    return 1;
}

static really_inline
int haoCanDirectLoadCurrentWindow64(const struct FDR_Runtime_Args *a,
                                    size_t endPos, u32 windowBytes) {
    return a && a->buf && windowBytes == HAO_RUNTIME_BYTES_PER_RULE_SLOT &&
           endPos + 1 >= HAO_RUNTIME_BYTES_PER_RULE_SLOT;
}

static really_inline
u64a haoExtractPackedBitsFallback(u64a window, u64a mask) {
    u64a packed = 0;
    u32 outBit = 0;

    while (mask && outBit < HAO_RUNTIME_MAX_SELECTORS) {
        const u64a lowest = mask & (0 - mask);
        if (window & lowest) {
            packed |= ((u64a)1 << outBit);
        }
        mask &= mask - 1;
        outBit++;
    }

    return packed;
}

static really_inline
u64a haoLoadWindow64Raw(const struct FDR_Runtime_Args *a, size_t endPos,
                        u32 windowBytes) {
    u64a window = 0;
    u32 i;

    if (!a) {
        return 0;
    }

    if (!windowBytes || windowBytes > HAO_RUNTIME_BYTES_PER_RULE_SLOT) {
        windowBytes = HAO_RUNTIME_BYTES_PER_RULE_SLOT;
    }

    if (haoCanDirectLoadCurrentWindow64(a, endPos, windowBytes)) {
        const u8 *start = a->buf + endPos + 1 - windowBytes;
        return unaligned_load_u64a(start);
    }

    for (i = 0; i < windowBytes; i++) {
        u8 b = 0;
        const s64a pos = (s64a)endPos - (s64a)(windowBytes - 1U) +
                         (s64a)i;
        if (!haoGetByteAt(a, pos, &b)) {
            continue;
        }
        window |= ((u64a)b) << (i * 8U);
    }

    return window;
}

static really_inline
u32 haoComputeValidMask8(const struct FDR_Runtime_Args *a, size_t endPos) {
    u64a available;

    if (!a) {
        return 0;
    }

    available = (u64a)endPos + 1 + a->len_history;
    if (!available) {
        return 0;
    }
    if (available >= HAO_RUNTIME_BYTES_PER_RULE_SLOT) {
        return 0xffU;
    }

    return ((1U << (u32)available) - 1U)
           << (HAO_RUNTIME_BYTES_PER_RULE_SLOT - (u32)available);
}

static really_inline
void haoBuildLaneWindow32FromWindow(u64a window, u32 validMask8,
                                    u8 *window32, u32 *validMask32) {
    u8 laneWindow[HAO_RUNTIME_BYTES_PER_RULE_SLOT] = {0};
    u32 j;
    u32 slot;

    if (!window32 || !validMask32) {
        return;
    }

    *validMask32 = 0;
    for (j = 0; j < HAO_RUNTIME_BYTES_PER_RULE_SLOT; j++) {
        const u32 srcByte = HAO_RUNTIME_BYTES_PER_RULE_SLOT - 1U - j;
        laneWindow[j] = (u8)(window >> (srcByte * 8U));
    }

    for (slot = 0; slot < HAO_RUNTIME_RULE_SLOTS_PER_ENTRY; slot++) {
        const u32 laneBase = slot * HAO_RUNTIME_BYTES_PER_RULE_SLOT;
        memcpy(window32 + laneBase, laneWindow, HAO_RUNTIME_BYTES_PER_RULE_SLOT);
        *validMask32 |= (validMask8 << laneBase);
    }
}

static really_inline
u32 haoPackedKeyMask(u32 keyBits) {
    if (!keyBits) {
        return 0;
    }
    if (keyBits >= 32U) {
        return 0xffffffffU;
    }
    return (1U << keyBits) - 1U;
}

static really_inline
u32 haoRuntimeEncodeKeyBits(u32 hashMode, u32 keyBits) {
    return (keyBits & HAO_RUNTIME_KEY_BITS_MASK) |
           (hashMode << HAO_RUNTIME_HASH_MODE_SHIFT);
}

static really_inline
u32 haoRuntimeHeaderKeyBits(const struct HAORuntimeHeader *hdr) {
    return hdr->keyBits & HAO_RUNTIME_KEY_BITS_MASK;
}

static really_inline
u32 haoRuntimeHeaderHashMode(const struct HAORuntimeHeader *hdr) {
    const u32 hashMode = hdr->keyBits >> HAO_RUNTIME_HASH_MODE_SHIFT;
    return hashMode ? hashMode : HAO_RUNTIME_HASH_BEXT;
}

static really_inline
u16 haoRuntimeHeaderDotVectorLane(const struct HAORuntimeHeader *hdr,
                                  u32 lane) {
    return (u16)(hdr->bextMask >> (lane * 16U));
}

static really_inline
u32 haoExtractKeyBext(const struct HAORuntimeHeader *hdr, u64a window) {
    const u64a packed = haoExtractPackedBitsFallback(window, hdr->bextMask);
    return (u32)(packed & haoPackedKeyMask(haoRuntimeHeaderKeyBits(hdr)));
}

static really_inline
u32 haoExtractKeyDot(const struct HAORuntimeHeader *hdr, u64a window) {
    const u32 keyMask = haoPackedKeyMask(haoRuntimeHeaderKeyBits(hdr));
    const u64a maskedWindow = window & hdr->dotInputMask;
    u64a dot = 0;
    u32 i;

    for (i = 0; i < HAO_RUNTIME_DOT_VECTOR_LANES; i++) {
        const u64a word = (maskedWindow >> (i * 16U)) & 0xffffU;
        dot += word * haoRuntimeHeaderDotVectorLane(hdr, i);
    }
    return (u32)(dot & keyMask);
}

static really_inline
u32 haoExtractKey(const struct HAORuntimeHeader *hdr, u64a window) {
    if (haoRuntimeHeaderHashMode(hdr) == HAO_RUNTIME_HASH_DOT) {
        return haoExtractKeyDot(hdr, window);
    }
    return haoExtractKeyBext(hdr, window);
}

static really_inline
void haoExtractKeysFromBextWindows(const struct HAORuntimeHeader *hdr,
                                   const u64a *windows, u32 count, u32 *keys) {
    u32 i;
    u32 keyMask;

    if (!hdr || !windows || !keys) {
        return;
    }

    keyMask = haoPackedKeyMask(haoRuntimeHeaderKeyBits(hdr));
    for (i = 0; i < count; i++) {
        keys[i] = (u32)(haoExtractPackedBitsFallback(windows[i],
                                                     hdr->bextMask) &
                        keyMask);
    }
}

static really_inline
void haoExtractKeysFromWindows(const struct HAORuntimeHeader *hdr,
                               const u64a *windows, u32 count, u32 *keys) {
    u32 i;

    if (!hdr || !windows || !keys) {
        return;
    }

    for (i = 0; i < count; i++) {
        keys[i] = haoExtractKey(hdr, windows[i]);
    }
}

#endif // HAO_RUNTIME_INLINE_H
