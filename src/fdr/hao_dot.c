/* Copyright (c) 2026, Huawei Technologies Co., Ltd. */

#define HAO_EXEC HaoDotExec
#define HAO_HASH_MODE HAO_RUNTIME_HASH_DOT
#include "hao_engine.c"

u32 HaoVectorLength(void) { return (u32)svcntb(); }
