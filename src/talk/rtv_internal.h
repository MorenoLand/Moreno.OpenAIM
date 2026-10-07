// rtv_internal.h - helpers shared by rtv_codec.cpp (decoder) and rtv_encoder.cpp
#pragma once
#include <cstdint>
#include "rtv_codec.h"
namespace rtv::detail {
#include "rtv_tables.inc"

// 32-bit wrapping arithmetic mirroring the original x86 code
inline int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
inline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
inline int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
// (a*b) >> s with round-half-up on the last shifted-out bit (original: imul; shrd; adc 0)   [jgs3aol 65f81000]
inline int32_t mulShiftRound(int32_t a, int32_t b, int s) {
  int64_t p = (int64_t)a * (int64_t)b;
  return (int32_t)((p >> s) + ((p >> (s - 1)) & 1));
}
inline int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

// LSP (cos domain, Q15, 8 values) -> LPC (Q15 direct-form coefficients a1..a8)     [jgs3aol 65f82d30]
void lsp2lpc(int32_t out[8], const int32_t in[8]);
// all-pole synthesis filter 1/A(z) in place on int16-range samples; mem holds past outputs << 15   [65f810f0]
void synthFilter(int32_t mem[8], int32_t* buf, const int32_t coef[8], int count);
// one sub-block of the long-term predictor: d = e + ((work*gain+8)>>4); updates the 120-sample history   [65f82a50/65f82a10]
void ltpSubBlock(int32_t hist[kHistLen], const int32_t lastOut[kBlockLen], int lag, int gain, const int32_t* e, int32_t* d);
}  // namespace rtv::detail
