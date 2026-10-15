/*
 * qd_internal.h - bit plumbing and the portable 128-bit vector abstraction.
 *
 * On the ESP32-S31 one of the two 320 MHz RISC-V cores has a 128-bit data
 * path with SIMD instructions. The ISA is RV32IMAFCP -- there is no RISC-V
 * "V" vector extension, so RVV intrinsics are NOT available and must not be
 * used. GCC/Clang generic vector types (vector_size) are the right tool: they
 * lower to the hardware's packed 128-bit ops where they exist and to scalar
 * code where they don't, from the same source.
 *
 * Everything here is host-testable on x86 as well, which is how we get a
 * correctness and performance baseline before touching hardware.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef QD_INTERNAL_H
#define QD_INTERNAL_H

#include "qd/qd.h"

#include <string.h>

/* ------------------------------------------------------------------ *
 * 128-bit vector abstraction
 * ------------------------------------------------------------------ */

#if defined(__GNUC__) || defined(__clang__)
#define QD_VEC16 __attribute__((vector_size(16)))
typedef uint8_t qd_u8x16 __attribute__((vector_size(16)));
typedef uint16_t qd_u16x8 __attribute__((vector_size(16)));
typedef uint64_t qd_u64x2 __attribute__((vector_size(16)));
#define QD_HAVE_VEC16 1
#else
/* Fall back to a pair of 64-bit words. Correct everywhere, just slower. */
typedef struct {
    uint64_t lo, hi;
} qd_u64x2;
typedef struct {
    uint8_t b[16];
} qd_u8x16;
#define QD_HAVE_VEC16 0
#endif

/* ------------------------------------------------------------------ *
 * 1-bit row access helpers (MSB = leftmost pixel, as on the 68000)
 * ------------------------------------------------------------------ */

static inline uint32_t qd_load16_msb(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

/* Extract the 16 bits starting at pixel index `bit` (0-based from the left).
 * Requires p to have 3 readable bytes past the row start. */
static inline uint32_t qd_get16_msb(const uint8_t *p, int bit)
{
    int byte = bit >> 3;
    int sh = bit & 7;
    uint32_t v = ((uint32_t)p[byte] << 16) | ((uint32_t)p[byte + 1] << 8) |
                 (uint32_t)p[byte + 2];
    return (v >> (8 + sh)) & 0xFFFFu;
}

/* Load 16 bits at pixel index `bit` into the low 16 bits of a uint32. */
static inline uint32_t qd_get16_masked(const uint8_t *p, int bit, uint32_t mask)
{
    return qd_get16_msb(p, bit) & mask;
}

/* Set the 16 bits starting at pixel index `bit`, honouring `mask`. */
static inline void qd_put16_msb(uint8_t *p, int bit, uint32_t val, uint32_t mask)
{
    int byte = bit >> 3;
    int sh = bit & 7;
    uint32_t v = ((uint32_t)p[byte] << 16) | ((uint32_t)p[byte + 1] << 8) |
                 (uint32_t)p[byte + 2];
    uint32_t shifted = (val & mask) << (8 + sh);
    v = (v & ~(mask << (8 + sh))) | shifted;
    p[byte] = (uint8_t)(v >> 16);
    p[byte + 1] = (uint8_t)(v >> 8);
    p[byte + 2] = (uint8_t)v;
}

/* Pixel-run mask: `n` ones starting at bit `bit`, inside a 16-bit field. */
static inline uint32_t qd_run_mask(int bit, int n)
{
    if (n <= 0) return 0;
    if (n >= 16) return 0xFFFFu;
    if (n >= 16 - (bit & 7)) return (0xFFFFu << (16 - n)) & 0xFFFFu;
    return ((1u << n) - 1u) << (16 - (bit & 7) - n);
}

static inline uint64_t qd_mask64(int n)
{
    if (n <= 0) return 0;
    if (n >= 64) return ~(uint64_t)0;
    return (1ull << n) - 1ull;
}

/* ------------------------------------------------------------------ *
 * Region internals
 * ------------------------------------------------------------------ */

#endif /* QD_INTERNAL_H */
