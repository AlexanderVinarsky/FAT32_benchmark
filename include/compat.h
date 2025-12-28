// compat.h
#pragma once

#include <ctype.h>
#include <time.h>
#include <stdint.h>
#define _GNU_SOURCE

#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif

static inline void str2uppercase(char* s) {
    if (!s) return;
    for (; *s; ++s) *s = (char)toupper((unsigned char)*s);
}
