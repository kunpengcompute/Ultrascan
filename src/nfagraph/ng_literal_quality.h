/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 *
 * Licensed under the BSD License.
 */

#ifndef NG_LITERAL_QUALITY_H
#define NG_LITERAL_QUALITY_H

#include "util/ue2string.h"

#include <cstddef>

namespace ue2 {

size_t neoFdrZeroCount(const ue2_literal &literal, size_t *window_out);

bool hasAllZeroNeoFdrTail(const ue2_literal &literal);

/**
 * Returns true if the final bytes of a literal are likely to create excessive
 * NeoFDR candidates in zero-padded input.
 *
 * NeoFDR confirms at most eight bytes. A four-byte literal containing three
 * zero bytes, such as 00 00 00 01 or 01 00 00 00, is therefore treated as
 * low quality in the same way as the existing eight-zero-tail case.
 */
bool isZeroDenseNeoFdrLiteral(const ue2_literal &literal);

/**
 * Returns true if a literal should not be used as a NeoFDR trigger.
 *
 * Short all-zero literals are included here because fixed-width mask
 * extraction can turn patterns such as 00 00 00 [01-07] into a three-byte
 * all-zero trigger.
 */
bool isLowQualityNeoFdrLiteral(const ue2_literal &literal);

} // namespace ue2

#endif // NG_LITERAL_QUALITY_H
