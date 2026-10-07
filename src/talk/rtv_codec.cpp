// rtv_codec.cpp - see rtv_codec.h and rtv_codec.md.  Decoder part.
#include "rtv_codec.h"
#include "rtv_internal.h"

#include <algorithm>
#include <cstring>

namespace rtv {
using namespace detail;
namespace { const uint32_t kMaskTab[9] = {0, 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe, 0xff}; }

// ---------------------------------------------------------------------------------------------
uint32_t Decoder::BitReader::get(int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; ++i) {
    uint32_t bit = 0;
    if (pos < nbits) bit = (p[pos >> 3] >> (7 - (pos & 7))) & 1;
    ++pos;
    v = (v << 1) | bit;
  }
  return v;
}

bool parseStreamHeader(const uint8_t* p, size_t n, StreamHeader& out) {
  // RtvDecodeCreate runs a validator (65f828b0) but ignores its result, so the decoder accepts any 19 bytes
  // starting with the 0xF8 stream-header symbol; isCanonicalHeader() reports what the validator would say.
  if (n < (size_t)kStreamHeaderBytes || p[0] != 0xF8) return false;
  for (int i = 0; i < 9; ++i) out.w[i] = (uint16_t)((p[1 + 2 * i] << 8) | p[2 + 2 * i]);
  return true;
}
bool isCanonicalHeader(const StreamHeader& h) {
  return h.w[0] == 0x1f40 && (h.w[2] == 4 || h.w[2] == 12) && h.w[4] != 0 && h.w[4] <= 6;
}
void serializeStreamHeader(const StreamHeader& h, uint8_t out[kStreamHeaderBytes]) {
  out[0] = 0xF8;
  for (int i = 0; i < 9; ++i) { out[1 + 2 * i] = (uint8_t)(h.w[i] >> 8); out[2 + 2 * i] = (uint8_t)h.w[i]; }
}

// ---------------------------------------------------------------------------------------------
Decoder::Decoder() {
  std::memset(&dbg, 0, sizeof dbg);
  std::memset(lsfIdx_, 0, sizeof lsfIdx_);
  std::memset(phase_, 0, sizeof phase_);
  std::memset(sub_, 0, sizeof sub_);
  std::memset(lsfPrev_, 0, sizeof lsfPrev_);
  std::memset(lsfCur_, 0, sizeof lsfCur_);
  std::memset(lsfNoise_, 0, sizeof lsfNoise_);
  std::memset(synthMem_, 0, sizeof synthMem_);
  std::memset(hist_, 0, sizeof hist_);
  std::memset(kTab_, 0, sizeof kTab_);
  std::memset(pvq_, 0, sizeof pvq_);
}

bool Decoder::init(const StreamHeader& hdr) {
  // Header word 2 == 4 selects the alternate bit-field order (unpacker 65f814e0); every other value selects the
  // mode-12 unpacker in the original.  The K range is validated here (the original would divide by zero).
  mode4_ = (hdr.mode() == 4);
  if (hdr.kMin() < 1 || hdr.kMax() < hdr.kMin() || hdr.kMax() > 38) return false;
  hdr_ = hdr;
  nBlocksDefault_ = hdr.blocksPerPacket();
  noLtp_ = hdr.noLtp();
  kMaxP_ = hdr.kMax(); kMinP_ = hdr.kMin();
  // K (pulse count) table: 32 entries (original: 65f83440)
  for (int j = 0; j < 32; ++j) kTab_[j] = kMinP_ + (((j + 1) * (kMaxP_ - kMinP_)) >> 5);
  // header flag bit 1: the first 7 entries are overwritten by min(2, kMin) (65f83498: rep stos at the table start)
  if (hdr.flags() & 2) for (int j = 0; j < 7; ++j) kTab_[j] = kMinP_ > 2 ? 2 : kMinP_;
  // PVQ enumeration table V[k][n] = number of integer vectors of dimension n with L1 norm k [65f82f80]
  for (int k = 0; k < 39; ++k)
    for (int n = 0; n <= kPvqDim; ++n) {
      if (k == 0) pvq_[k][n] = 1;
      else if (n == 0) pvq_[k][n] = 0;
      else pvq_[k][n] = pvq_[k - 1][n - 1] + pvq_[k - 1][n] + pvq_[k][n - 1];
    }
  for (int i = 0; i < kLpcOrder; ++i) { lsfPrev_[i] = kLsfInit[i]; lsfNoise_[i] = kLsfInit[i]; lsfCur_[i] = 0; synthMem_[i] = 0; }
  std::memset(hist_, 0, sizeof hist_); std::memset(lastOut_, 0, sizeof lastOut_);
  deemph_ = 0; seed_ = 0x55555555;
  active_ = noiseFlag_ = noiseLevel_ = 0;
  for (int i = 0; i < kLpcOrder; ++i) { lsfIdx_[i] = 0; lsfIdxPrev_[i] = kPrevIdxInit[i]; }
  return true;
}

// ---- bitstream unpack of one block (mode 12) [jgs3aol 65f81900] ---------------------------------
// PVQ index (5-bit "val" already read) and pitch fields of one sub-block.
void Decoder::readPvq(BitReader& br, int sb) {
  SubParams& s = sub_[sb];
  int K = kTab_[s.val];
  // number of index bits for K pulses: bits(K) = ceil(log2 V[K][20])  (table 65f83080; entry 0 is 1)
  uint64_t cnt = pvq_[K][kPvqDim];
  int nb = 0; while (nb < 64 && ((nb == 0 ? 1ull : (1ull << nb)) < cnt)) ++nb;
  if (K == 0) nb = 1;
  s.nbits = nb;
  // index is read in chunks of at most 16 bits; chunk i becomes 16-bit limb i (little-endian limbs)
  uint64_t idx = 0; int remain = nb, limb = 0;
  while (remain > 0) {
    int w = remain > 16 ? 16 : remain;
    idx |= (uint64_t)br.get(w) << (16 * limb);
    remain -= w; ++limb;
  }
  s.index = idx;
}

// Pitch fields of one sub-block: '11' = no pitch, else 7-bit lag index + 5-bit (gain-1).
void Decoder::readPitch(BitReader& br, int sb) {
  SubParams& s = sub_[sb];
  uint32_t top2 = br.get(2);
  if (top2 == 3) { s.lag = 0; s.gain = 0; }
  else {
    s.lag = (int)((top2 << 5) | br.get(5));   // 7-bit lag index (first two bits already read)
    s.gain = (int)br.get(5) + 1;              // 1..32
  }
}

void Decoder::unpackBlock(BitReader& br) {
  if (mode4_) { unpackMode4(br); return; }
  active_ = (int)br.get(1);
  if (!active_) {
    noiseFlag_ = (int)br.get(1);
    noiseLevel_ = (int)br.get(5);
  }
  if (active_) {
    int v = (int)br.get(7);                       // four base-3 digits, most significant first
    for (int sb = kSubBlocks - 1; sb >= 0; --sb) { phase_[sb] = v % 3; v /= 3; }
    for (int sb = 0; sb < kSubBlocks; ++sb) {
      sub_[sb].val = (int)br.get(5);
      readPvq(br, sb);
      if (!noLtp_) readPitch(br, sb);
    }
  }
  if (active_ || noiseFlag_) {
    for (int i = 0; i < kLpcOrder; ++i) lsfIdxPrev_[i] = lsfIdx_[i] = (int)br.get(kLsfBits[i]);
  }
}

// Alternate field order selected by header word 2 == 4 [jgs3aol 65f814e0]; every block is active.
void Decoder::unpackMode4(BitReader& br) {
  active_ = 1;
  for (int sb = 0; sb < kSubBlocks; ++sb) sub_[sb].val = (int)br.get(5);
  if ((hdr_.flags() & 4) && sub_[1].val < 6 && sub_[2].val < 6 && sub_[3].val < 6) {
    for (int i = 0; i < kLpcOrder; ++i) lsfIdx_[i] = lsfIdxPrev_[i];             // LSP indices repeated
  } else {
    for (int i = 0; i < kLpcOrder; ++i) lsfIdxPrev_[i] = lsfIdx_[i] = (int)br.get(kLsfBits[i]);
  }
  if (hdr_.flags() & 1) {
    for (int sb = 0; sb < kSubBlocks; ++sb) phase_[sb] = 1;
  } else {
    int v = (int)br.get(7);
    for (int sb = kSubBlocks - 1; sb >= 0; --sb) { phase_[sb] = v % 3; v /= 3; }
  }
  for (int sb = 0; sb < kSubBlocks; ++sb) readPvq(br, sb);
  if (!noLtp_) for (int sb = 0; sb < kSubBlocks; ++sb) readPitch(br, sb);   // pitch fields come after all four PVQ indices
}

// ---- fixed-codebook (pulse) excitation for one sub-block [65f82140, 65f821b0, 65f82240] ------------
void Decoder::subExcitation(int sb, int32_t* out60) const {
  const SubParams& s = sub_[sb];
  int K = kTab_[s.val];
  int32_t G = kGain[s.val];
  // enumerative PVQ decode (Fischer): positions processed from the last to the first
  int32_t tmp[kPvqDim];
  for (int i = 0; i < kPvqDim; ++i) tmp[i] = 0;
  uint64_t idx = s.index;
  int k = K;
  for (int j = kPvqDim - 1; j >= 0 && k > 0; --j) {
    int n = j + 1;
    if (idx < pvq_[k][n - 1]) continue;           // element is zero
    idx -= pvq_[k][n - 1];
    int m = 1;
    for (;;) {
      uint64_t half = pvq_[k - m][n - 1];
      if (idx < 2 * half || m == k) break;
      idx -= 2 * half; ++m;
    }
    uint64_t half = pvq_[k - m][n - 1];
    bool neg = false;
    if (idx >= half) { neg = true; idx -= half; }
    tmp[kPvqDim - 1 - j] = neg ? -m : m;           // original stores in reverse order
    k -= m;
  }
  // scale: v * G / K  (truncating division) and place every third sample, starting at 'phase'
  for (int i = 0; i < kSubLen; ++i) out60[i] = 0;
  for (int i = 0; i < kPvqDim; ++i) out60[phase_[sb] + 3 * i] = mul32(tmp[i], G) / K;
}

// ---- long-term (pitch) predictor [65f82b20, 65f82a50, 65f82a10] -----------------------------------
void detail::ltpSubBlock(int32_t hist[kHistLen], const int32_t lastOut[kBlockLen], int lag, int gain, const int32_t* e, int32_t* d) {
  if (gain > 0) {
    int32_t work[kSubLen];
    int L = lag;
    // The original indexes a flat int32 array that is the 120-sample history immediately followed by the
    // previous block's final output buffer; indices >= 120 are only reachable for lag indices > 90.
    auto H = [&](int i) -> int32_t { return i < kHistLen ? hist[i] : lastOut[i - kHistLen]; };
    if (L <= kSubLen) {
      for (int i = 0; i < kSubLen; ++i) work[i] = H(L + i);
    } else {
      int D = kHistLen - L;                       // pitch delay in samples (periodic extension)
      for (int i = 0; i < D; ++i) work[i] = H(L + i);
      for (int i = 0; i < kSubLen - D; ++i) work[D + i] = H(L + i);
    }
    for (int i = 0; i < kSubLen; ++i) d[i] = add32(e[i], (add32(mul32(work[i], gain), 8)) >> 4);
  } else {
    for (int i = 0; i < kSubLen; ++i) d[i] = e[i];
  }
  // slide history: [hist[60..119], d]
  std::memmove(hist, hist + kSubLen, kSubLen * sizeof(int32_t));
  std::memcpy(hist + kSubLen, d, kSubLen * sizeof(int32_t));
}

void Decoder::ltp(int32_t* exc, int32_t* dest) {
  for (int sb = 0; sb < kSubBlocks; ++sb)
    ltpSubBlock(hist_, lastOut_, sub_[sb].lag, sub_[sb].gain, exc + sb * kSubLen, dest + sb * kSubLen);
}

// ---- comfort noise excitation [65f83640, 65f836d0] ------------------------------------------------
void Decoder::noiseExcitation(int32_t* dest) {
  int32_t gain = kNoiseGain[noiseLevel_];
  uint32_t x = seed_;
  uint32_t raw[kBlockLen];
  for (int i = 0; i < kBlockLen; ++i) { x = x * 0x19660du + 0x7fffu; raw[i] = x; }
  seed_ = raw[kBlockLen - 1];
  for (int i = 0; i < kBlockLen; ++i) dest[i] = mulShiftRound(gain, (int32_t)raw[i], 31);
  if (!noiseFlag_) std::memcpy(lsfCur_, lsfNoise_, sizeof lsfCur_);
}

// ---- LSP -> LPC [65f82d30] ---------------------------------------------------------------------
void detail::lsp2lpc(int32_t out[8], const int32_t in[8]) {
  int32_t W[32] = {0};
  W[6] = -in[0]; W[7] = 0x8000; W[1] = -in[1]; W[2] = 0x8000;
  const int ebx = 4;
  const int32_t* pin = in + 2;
  for (int ebp = 4; ebp - 3 < ebx; ++ebp) {
    int cnt = ebp - 2;
    for (int j = 0; j < cnt; ++j) W[12 + j] = W[1 + j];
    for (int j = 0; j < cnt; ++j) W[19 + j] = W[6 + j];
    int32_t cP = (pin[0] + 4) >> 3, cQ = (pin[1] + 4) >> 3;
    for (int jj = 0; jj < ebp - 1; ++jj) {
      int32_t t = mul32(W[19 + jj], cP); t = (add32(t, 0x800) >> 11) & ~1;
      int32_t p = sub32(W[20 + jj], t); p = add32(p, W[18 + jj]);
      int32_t u = mul32(W[12 + jj], cQ); u = (add32(u, 0x800) >> 11) & ~1;
      int32_t q = sub32(W[13 + jj], u); q = add32(q, W[11 + jj]);
      W[6 + jj] = p; W[1 + jj] = q;
    }
    W[7] = add32(W[7], W[19]);
    W[2] = add32(W[2], W[12]);
    pin += 2;
  }
  for (int e = 0; e <= ebx; ++e) { W[11 + e] = W[10 - e]; W[18 + e] = W[5 - e]; }
  W[11 + ebx] <<= 1; W[18 + ebx] <<= 1;
  for (int e = 0; e < ebx; ++e) {
    int32_t sum = add32(W[12 + e], W[11 + e]);
    int32_t dif = sub32(W[19 + e], W[18 + e]);
    W[6 + e] = sum; W[1 + e] = dif;
  }
  for (int e = 0; e < ebx; ++e) {
    int32_t s = W[6 + e], d = W[1 + e];
    out[e] = (add32(add32(s, d), 1)) >> 1;
    out[7 - e] = (add32(sub32(s, d), 1)) >> 1;
  }
}

// ---- LPC synthesis filter 1/A(z), in place [65f810f0] --------------------------------------------
void detail::synthFilter(int32_t mem[8], int32_t* buf, const int32_t coef[8], int count) {
  for (int j = 0; j < count; ++j) {
    int32_t acc = (int32_t)((uint32_t)buf[j] << 15);
    for (int k = 0; k < 8; ++k) acc = sub32(acc, mul32((add32(mem[k], 0x4000)) >> 15, coef[k]));
    int32_t y = add32(acc, 0x4000);
    int32_t hi = y & (int32_t)0xffff8000;
    int32_t out;
    if (hi > 0x3fff8000) out = 0x7fff;
    else if (hi < (int32_t)0xc0000000) out = -32768;
    else out = y >> 15;
    buf[j] = out;
    for (int k = 7; k > 0; --k) mem[k] = mem[k - 1];
    mem[0] = (int32_t)((uint32_t)out << 15);
  }
}

void Decoder::synthesize(const int32_t* exc, int32_t* out) {
  if (active_ || noiseFlag_) {
    // dequantise LSP indices to cos-domain Q15 values [65f82bf0]
    int32_t edi = -32768;
    for (int i = 7; i >= 0; --i) {
      if (!(edi > kLsfMin[i])) edi = kLsfMin[i];
      int32_t den = (1 << kLsfBits[i]) - 1;
      int32_t num = (int32_t)((uint32_t)sub32(kLsfMax[i], edi) << 15);
      int32_t q = num / den;
      q = add32(q, 0x4000) >> 15;
      edi = add32(edi, mul32(q, lsfIdx_[i]));
      lsfCur_[i] = edi;
    }
  }
  if (!active_ && noiseFlag_) std::memcpy(lsfNoise_, lsfCur_, sizeof lsfNoise_);
  int32_t lpc[8], lsf[8];
  for (int h = 0; h < 2; ++h) {                    // first two sub-blocks: interpolated LSPs [65f82c80]
    for (int i = 0; i < 8; ++i)
      lsf[i] = (h == 0) ? (3 * lsfPrev_[i] + 5 * lsfCur_[i] + 4) >> 3 : (7 * lsfCur_[i] + lsfPrev_[i] + 4) >> 3;
    lsp2lpc(lpc, lsf);
    int32_t tmp[kSubLen];
    std::memcpy(tmp, exc + h * kSubLen, sizeof tmp);
    synthFilter(synthMem_, tmp, lpc, kSubLen);
    std::memcpy(out + h * kSubLen, tmp, sizeof tmp);
  }
  lsp2lpc(lpc, lsfCur_);
  int32_t tmp2[2 * kSubLen];
  std::memcpy(tmp2, exc + 2 * kSubLen, sizeof tmp2);
  synthFilter(synthMem_, tmp2, lpc, 2 * kSubLen);
  std::memcpy(out + 2 * kSubLen, tmp2, sizeof tmp2);
  std::memcpy(lsfPrev_, lsfCur_, sizeof lsfPrev_);
}

void Decoder::decodeBlock(BitReader& br, int32_t out[kBlockLen]) {
  size_t p0 = br.pos;
  unpackBlock(br);
  lastBits_ = (int)(br.pos - p0);
  int32_t exc[kBlockLen];
  if (active_) {
    int32_t raw[kBlockLen];
    for (int sb = 0; sb < kSubBlocks; ++sb) subExcitation(sb, raw + sb * kSubLen);
    std::memcpy(dbg.raw, raw, sizeof raw);
    if (!noLtp_) ltp(raw, exc);
    else std::memcpy(exc, raw, sizeof exc);
  } else {
    noiseExcitation(exc);
  }
  std::memcpy(dbg.exc, exc, sizeof exc);
  int32_t syn[kBlockLen];
  synthesize(exc, syn);
  std::memcpy(dbg.synth, syn, sizeof syn);
  // de-emphasis y[n] = x[n] + 0.86*y[n-1] (original 65f81080) then scale x2, clear 3 LSBs, saturate
  for (int i = 0; i < kBlockLen; ++i) {
    int32_t x = (int16_t)syn[i];
    int32_t y = add32(x, add32(mul32(28180, deemph_), 0x4000) >> 15);
    y = sat16(y);
    deemph_ = y;
    int32_t v;
    if (((y + 0x4000) & 0x8000) != 0) v = (y >= 0) ? 0x7ff8 : -32768;
    else v = (y + y) & ~7;
    out[i] = v;
  }
  std::memcpy(lastOut_, out, sizeof lastOut_);
}

int Decoder::decodePacket(const uint8_t* data, size_t len, int32_t* pcm) {
  if (len > 0x100) len = 0x100;
  BitReader br{data, len * 8, 0};
  // block-count symbol [65f81dd0]
  uint32_t top8 = 0;
  { BitReader t = br; top8 = t.get(8); }
  uint32_t sym = 0xffff; int nb = 0;
  for (auto& e : kSymTable) {
    if ((e[1] << (8 - e[2])) == (kMaskTab[e[2]] & top8)) { sym = e[0]; nb = (int)e[2]; break; }
  }
  if (sym == 0xffff) return -1;
  br.get(nb);
  if (sym == 0x8000) return 0;
  int count = 0;
  if (sym == 1) count = nBlocksDefault_;
  else if (sym == 3) count = (int)br.get(8);
  else if (sym == 8) { count = nBlocksDefault_; br.get(3); }
  if (count > kMaxBlocksPerPacket) count = kMaxBlocksPerPacket;
  for (int b = 0; b < count; ++b) decodeBlock(br, pcm + b * kBlockLen);
  return count;
}

}  // namespace rtv
