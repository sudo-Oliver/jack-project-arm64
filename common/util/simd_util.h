#pragma once

#ifndef __aarch64__
#include <immintrin.h>
#else
#define SSE2NEON_PRECISE_MINMAX 1
#include "third-party/sse2neon/sse2neon.h"
#endif
