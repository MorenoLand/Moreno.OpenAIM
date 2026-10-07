// rtv_encoder.cpp - analysis-by-synthesis encoder producing streams the original jgs3aol decoder accepts.
// NOT bit-identical to the original jgs2aol encoder (whose analysis stages were not reverse engineered);
// bitstream syntax, quantiser tables and decoder-side state are exact (see rtv_codec.md).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "rtv_codec.h"
#include "rtv_internal.h"

namespace rtv {
using namespace detail;

namespace {

struct BitWriter {
  std::vector<uint8_t> b;
  size_t nb = 0;
  void put(uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i) {
      if ((nb & 7) == 0) b.push_back(0);
      if ((v >> i) & 1) b.back() |= (uint8_t)(0x80 >> (nb & 7));
      ++nb;
    }
  }
  void putIndex(uint64_t idx, int nbits) {  // chunks of <=16 bits, least-significant limb first
    int rem = nbits;
    while (rem > 0) {
      int w = rem > 16 ? 16 : rem;
      put((uint32_t)(idx & ((1u << w) - 1)), w);
      idx >>= 16; rem -= w;
    }
  }
};

struct PvqTable {
  uint64_t v[39][kPvqDim + 1];
  PvqTable() {
    for (int k = 0; k < 39; ++k)
      for (int n = 0; n <= kPvqDim; ++n) {
        if (k == 0) v[k][n] = 1;
        else if (n == 0) v[k][n] = 0;
        else v[k][n] = v[k - 1][n - 1] + v[k - 1][n] + v[k][n - 1];
      }
  }
};
const PvqTable& pvqTable() { static const PvqTable t; return t; }   // thread-safe initialisation
int bitsForCount(uint64_t cnt) { int nb = 0; uint64_t c = cnt - 1; while (c) { ++nb; c >>= 1; } return nb; }

// inverse of the decoder's enumerative PVQ decode: v[0..19] (integer, sum|v| == K) -> index
uint64_t pvqEncode(const int* v, int K) {
  uint64_t idx = 0; int k = K;
  for (int i = 0; i < kPvqDim && k > 0; ++i) {
    int n = kPvqDim - i;                         // remaining positions including this one
    int m = v[i] < 0 ? -v[i] : v[i];
    if (m == 0) continue;
    idx += pvqTable().v[k][n - 1];
    for (int mm = 1; mm < m; ++mm) idx += 2 * pvqTable().v[k - mm][n - 1];
    if (v[i] < 0) idx += pvqTable().v[k - m][n - 1];
    k -= m;
  }
  return idx;
}

// ---- LPC analysis helpers (double precision) -----------------------------------------------------------
bool levinson(const double* r, int order, double* a) {   // a[0]=1, a[1..order]
  double tmp[16];
  a[0] = 1.0; for (int i = 1; i <= order; ++i) a[i] = 0.0;
  double err = r[0];
  if (err <= 0) return false;
  for (int i = 1; i <= order; ++i) {
    double acc = r[i];
    for (int j = 1; j < i; ++j) acc += a[j] * r[i - j];
    double k = -acc / err;
    for (int j = 1; j < i; ++j) tmp[j] = a[j] + k * a[i - j];
    for (int j = 1; j < i; ++j) a[j] = tmp[j];
    a[i] = k;
    err *= (1.0 - k * k);
    if (err <= 1e-9) return false;
  }
  return true;
}

double chebF(const double* f, double x) {  // f[0..4] symmetric polynomial coefficients (order 8 => degree 4 in cos)
  double t0 = 1.0, t1 = x, t2 = 2 * x * t1 - t0, t3 = 2 * x * t2 - t1, t4 = 2 * x * t3 - t2;
  return 2 * (f[0] * t4 + f[1] * t3 + f[2] * t2 + f[3] * t1) + f[4];
}

// LPC (A(z)=1+sum a_k z^-k) -> 8 LSP values in the cos domain, descending (i.e. increasing frequency). false on failure.
bool lpc2lsp(const double* a, double* cosv) {
  double f1[5], f2[5];
  f1[0] = 1; f2[0] = 1;
  for (int i = 1; i <= 4; ++i) { f1[i] = a[i] + a[9 - i] - f1[i - 1]; f2[i] = a[i] - a[9 - i] + f2[i - 1]; }
  const int GRID = 512;
  int found = 0;
  const double* f = f1;
  double xlow = 1.0, ylow = chebF(f, xlow);
  for (int j = 1; j <= GRID && found < 8; ++j) {
    double xhigh = xlow, yhigh = ylow;
    double xl = std::cos(3.14159265358979323846 * j / GRID), yl = chebF(f, xl);
    if (yl * yhigh <= 0) {
      double xa = xl, ya = yl, xb = xhigh;
      for (int it = 0; it < 30; ++it) {
        double xm = 0.5 * (xa + xb), ym = chebF(f, xm);
        if (ya * ym <= 0) xb = xm; else { xa = xm; ya = ym; }
      }
      double root = 0.5 * (xa + xb);
      cosv[found++] = root;
      f = (f == f1) ? f2 : f1;
      xlow = root; ylow = chebF(f, xlow);
    } else { xlow = xl; ylow = yl; }
  }
  return found == 8;
}

struct Quant { int idx[8]; int32_t cur[8]; };
// sequential LSP quantiser mirroring the decoder's dequantisation (65f82bf0)
void quantLsp(const double* cosv, Quant& q) {
  int32_t prev = -32768;
  for (int i = 7; i >= 0; --i) {
    int32_t lo = std::max<int32_t>(prev, kLsfMin[i]);
    int32_t den = (1 << kLsfBits[i]) - 1;
    int32_t num = (int32_t)((uint32_t)sub32(kLsfMax[i], lo) << 15);
    int32_t step = num / den;
    step = add32(step, 0x4000) >> 15;
    double target = cosv[i] * 32768.0;
    int best = 0; double bd = 1e30;
    for (int k = 0; k <= den; ++k) {
      double d = std::fabs(target - (double)(lo + k * step));
      if (d < bd) { bd = d; best = k; }
    }
    q.idx[i] = best; q.cur[i] = lo + best * step; prev = q.cur[i];
  }
}

}  // namespace

// =============================================================================================
struct EncoderImpl {
  StreamHeader hdr;
  Decoder dec;
  double lambda = 0.0012;
  int maxVal = 31;
  int LA = 40;                      // look-ahead = algorithmic delay
  std::vector<double> yv;           // y-domain input (high-passed s/2), LA zeros prepended
  size_t T = 0;                     // start index (in yv) of the next block
  double hpX = 0, hpY = 0;
  int lastBitsBlock = 0;
  double noiseThr = 22.0;           // y-domain rms below which a block is coded as comfort noise
  bool weighting = true;            // perceptual weighting W(z) = A(z/g1) / A(z/g2) of the search error
  double gam1 = 0.9, gam2 = 0.6;
  double eyH[8] = {0}, ewH[8] = {0};   // past (unweighted / weighted) error samples, most recent first

  double Y(long i) const { return (i < 0 || (size_t)i >= yv.size()) ? 0.0 : yv[(size_t)i]; }

  bool init(const StreamHeader& h) {
    hdr = h;
    if (!dec.init(h) || h.noLtp()) return false;   // only the AIM configuration (mode 12, with LTP) is supported
    yv.assign((size_t)LA, 0.0); T = 0; hpX = hpY = 0;
    std::memset(eyH, 0, sizeof eyH); std::memset(ewH, 0, sizeof ewH);
    return true;
  }

  // ---- one sub-block search ------------------------------------------------------------------------
  struct SubChoice { int val, phase, lag, gain; int vec[kPvqDim]; uint64_t index; int nbits; double cost; double predE = 0, e0 = 0; };

  void searchSub(const double* yref, const int32_t* coefQ, const int32_t* synMem, int32_t yPrev, const int32_t* hist, const int32_t* lastOut,
                 SubChoice& out) {
    double a[8]; for (int k = 0; k < 8; ++k) a[k] = coefQ[k] / 32768.0;
    // zero-input response of cascade (1/A, de-emphasis) and its impulse response
    double zs[kSubLen + 8], yz[kSubLen];
    for (int k = 0; k < 8; ++k) zs[7 - k] = (double)(synMem[k] >> 15);   // zs[8+n-1-k] ... index offset 8
    double yp = (double)yPrev;
    for (int n = 0; n < kSubLen; ++n) {
      double s = 0; for (int k = 1; k <= 8; ++k) s -= a[k - 1] * zs[8 + n - k];
      zs[8 + n] = s; yp = s + 0.86 * yp; yz[n] = yp;
    }
    double h1[kSubLen], hF[kSubLen];
    for (int n = 0; n < kSubLen; ++n) {
      double s = (n == 0) ? 1.0 : 0.0;
      for (int k = 1; k <= 8 && k <= n; ++k) s -= a[k - 1] * h1[n - k];
      h1[n] = s; hF[n] = s + (n ? 0.86 * hF[n - 1] : 0.0);
    }
    double t[kSubLen]; double e0 = 0;
    for (int n = 0; n < kSubLen; ++n) t[n] = yref[n] - yz[n];
    if (weighting) {
      // work in the perceptually weighted domain: e_w = W e,  W(z) = A(z/g1)/A(z/g2)
      double b[8], c[8], g1 = 1, g2 = 1;
      for (int k = 0; k < 8; ++k) { g1 *= gam1; g2 *= gam2; b[k] = a[k] * g1; c[k] = a[k] * g2; }
      double hw[kSubLen], ewz[kSubLen], tw[kSubLen], hwF[kSubLen];
      for (int n = 0; n < kSubLen; ++n) {
        double v = (n == 0) ? 1.0 : (n <= 8 ? b[n - 1] : 0.0);
        for (int k = 1; k <= 8 && k <= n; ++k) v -= c[k - 1] * hw[n - k];
        hw[n] = v;
      }
      for (int n = 0; n < kSubLen; ++n) {
        double sacc = 0;
        for (int k = 1; k <= 8; ++k) {
          int m = n - k;
          double ePast = (m < 0) ? eyH[-m - 1] : 0.0, ewPrev = (m < 0) ? ewH[-m - 1] : ewz[m];
          sacc += b[k - 1] * ePast - c[k - 1] * ewPrev;
        }
        ewz[n] = sacc;
      }
      for (int n = 0; n < kSubLen; ++n) {
        double st = ewz[n], sh = 0;
        for (int m = 0; m <= n; ++m) { st += hw[n - m] * t[m]; sh += hw[n - m] * hF[m]; }
        tw[n] = st; hwF[n] = sh;
      }
      std::memcpy(t, tw, sizeof t); std::memcpy(hF, hwF, sizeof hF);
    }
    for (int n = 0; n < kSubLen; ++n) e0 += t[n] * t[n];
    SubChoice best; best.cost = 1e300; best.val = 0;
    // ---- long-term predictor search
    int bestL = -1, bestG = 0; double bestRed = 0; double bestYw[kSubLen];
    if (e0 > 1.0) {
      int32_t work[kSubLen]; double yw[kSubLen];
      auto H = [&](int i) -> int32_t { return i < kHistLen ? hist[i] : lastOut[i - kHistLen]; };
      for (int L = 0; L <= 90; ++L) {
        if (L <= kSubLen) { for (int i = 0; i < kSubLen; ++i) work[i] = H(L + i); }
        else { int D = kHistLen - L; for (int i = 0; i < D; ++i) work[i] = H(L + i); for (int i = 0; i < kSubLen - D; ++i) work[D + i] = H(L + i); }
        double c = 0, e = 0;
        for (int n = 0; n < kSubLen; ++n) {
          double s = 0; for (int m = 0; m <= n; ++m) s += hF[n - m] * (double)work[m];
          yw[n] = s; c += t[n] * s; e += s * s;
        }
        if (c <= 0 || e <= 1e-9) continue;
        int G = (int)std::lround(16.0 * c / e); if (G < 1) G = 1; if (G > 32) G = 32;
        double g = G / 16.0, red = 2 * g * c - g * g * e;
        if (red > bestRed) { bestRed = red; bestL = L; bestG = G; std::memcpy(bestYw, yw, sizeof yw); }
      }
    }
    const double ltpBits = 12, noLtpBits = 2;
    for (int useLtp = 0; useLtp < (bestL >= 0 ? 2 : 1); ++useLtp) {
      double r[kSubLen];
      for (int n = 0; n < kSubLen; ++n) r[n] = t[n] - (useLtp ? (bestG / 16.0) * bestYw[n] : 0.0);
      double er = 0; for (int n = 0; n < kSubLen; ++n) er += r[n] * r[n];
      for (int ph = 0; ph < 3; ++ph) {
        double cc[kPvqDim], M[kPvqDim][kPvqDim];
        for (int j = 0; j < kPvqDim; ++j) {
          int pj = ph + 3 * j; double c = 0; for (int n = pj; n < kSubLen; ++n) c += r[n] * hF[n - pj]; cc[j] = c;
        }
        for (int j = 0; j < kPvqDim; ++j) for (int k = j; k < kPvqDim; ++k) {
          int pj = ph + 3 * j, pk = ph + 3 * k; double s = 0;
          for (int n = pk; n < kSubLen; ++n) s += hF[n - pj] * hF[n - pk];
          M[j][k] = M[k][j] = s;
        }
        for (int val = 0; val <= maxVal; ++val) {
          int K = dec.kTab_[val]; double alpha = (double)kGain[val] / K;
          int v[kPvqDim] = {0}; double Mv[kPvqDim] = {0}; double E = er;
          for (int p = 0; p < K; ++p) {
            int bj = 0, bs = 1; double bd = 1e300;
            for (int j = 0; j < kPvqDim; ++j) for (int s = -1; s <= 1; s += 2) {
              if ((v[j] > 0 && s < 0) || (v[j] < 0 && s > 0)) continue;   // magnitudes may only grow (L1 norm must be exactly K)
              double d = -2 * alpha * s * cc[j] + alpha * alpha * (2 * s * Mv[j] + M[j][j]);
              if (d < bd) { bd = d; bj = j; bs = s; }
            }
            v[bj] += bs; E += bd;
            for (int k = 0; k < kPvqDim; ++k) Mv[k] += bs * M[k][bj];
          }
          int nbits = 5 + bitsForCount(pvqTable().v[K][kPvqDim]) + (useLtp ? (int)ltpBits : (int)noLtpBits);
          double cost = E / (e0 + 1e3) + lambda * nbits;
          if (cost < best.cost) {
            best.cost = cost; best.predE = E; best.e0 = e0; best.val = val; best.phase = ph; best.lag = useLtp ? bestL : 0; best.gain = useLtp ? bestG : 0;
            std::memcpy(best.vec, v, sizeof v);
          }
        }
      }
    }
    best.index = pvqEncode(best.vec, dec.kTab_[best.val]);
    best.nbits = bitsForCount(pvqTable().v[dec.kTab_[best.val]][kPvqDim]);
    out = best;
  }

  // after a sub-block has been finalised: advance the weighting-filter memories with the real error
  void updateWeightHist(const int32_t* coefQ, const double* yref, const double* yact) {
    if (!weighting) return;
    double a[8], b[8], c[8], g1 = 1, g2 = 1;
    for (int k = 0; k < 8; ++k) { a[k] = coefQ[k] / 32768.0; g1 *= gam1; g2 *= gam2; b[k] = a[k] * g1; c[k] = a[k] * g2; }
    double ey[kSubLen + 8], ew[kSubLen + 8];
    for (int k = 0; k < 8; ++k) { ey[7 - k] = eyH[k]; ew[7 - k] = ewH[k]; }
    for (int n = 0; n < kSubLen; ++n) {
      double e = yref[n] - yact[n], v = e;
      for (int k = 1; k <= 8; ++k) v += b[k - 1] * ey[8 + n - k] - c[k - 1] * ew[8 + n - k];
      ey[8 + n] = e; ew[8 + n] = v;
    }
    for (int k = 0; k < 8; ++k) { eyH[k] = ey[8 + kSubLen - 1 - k]; ewH[k] = ew[8 + kSubLen - 1 - k]; }
  }

  // ---- encode one block into bw ----------------------------------------------------------------------
  void encodeBlock(BitWriter& bw) {
    const int WL = 256, WC = 144;
    double xp[WL], w[WL];
    for (int n = 0; n < WL; ++n) {
      long i = (long)T + WC - WL / 2 + n;
      w[n] = 0.54 - 0.46 * std::cos(2 * 3.14159265358979323846 * (n + 0.5) / WL);
      xp[n] = (Y(i) - 0.86 * Y(i - 1)) * w[n];
    }
    double r[9];
    for (int k = 0; k <= 8; ++k) { double s = 0; for (int n = k; n < WL; ++n) s += xp[n] * xp[n - k]; r[k] = s; }
    r[0] = r[0] * 1.0001 + 1.0;
    for (int k = 1; k <= 8; ++k) { double x = 2 * 3.14159265358979323846 * 60.0 * k / 8000.0; r[k] *= std::exp(-0.5 * x * x); }
    double a[9]; double cosv[8];
    bool ok = levinson(r, 8, a);
    if (ok) { double aw[9]; double g = 1.0; for (int i = 0; i <= 8; ++i) { aw[i] = a[i] * g; g *= 0.995; } ok = lpc2lsp(aw, cosv); }
    Quant q;
    if (ok) quantLsp(cosv, q);
    else { for (int i = 0; i < 8; ++i) { q.cur[i] = dec.lsfPrev_[i]; q.idx[i] = 0; } /* idx unused below when !ok -> fall back to noise path */ }
    // block energy
    double eb = 0; for (int n = 0; n < kBlockLen; ++n) { double y = Y((long)T + n); eb += y * y; }
    double rms = std::sqrt(eb / kBlockLen);
    bool noise = (rms < noiseThr) || !ok;
    if (noise) {
      // comfort-noise block: A=0, B=(send LSPs), 5-bit level
      int32_t dist = 0;
      if (ok) for (int i = 0; i < 8; ++i) dist += std::abs(q.cur[i] - dec.lsfNoise_[i]);
      bool sendB = ok && (dist > 8 * 1500 || dec.active_);
      int32_t cur[8]; if (sendB) std::memcpy(cur, q.cur, sizeof cur); else std::memcpy(cur, dec.lsfNoise_, sizeof cur);
      int32_t lpc[8]; lsp2lpc(lpc, cur);
      double a8[8]; for (int k = 0; k < 8; ++k) a8[k] = lpc[k] / 32768.0;
      double h1[160], hF[160], pg = 0;
      for (int n = 0; n < 160; ++n) { double s = (n == 0) ? 1.0 : 0.0; for (int k = 1; k <= 8 && k <= n; ++k) s -= a8[k - 1] * h1[n - k]; h1[n] = s; hF[n] = s + (n ? 0.86 * hF[n - 1] : 0.0); pg += hF[n] * hF[n]; }
      double need = rms / std::sqrt(pg) * std::sqrt(3.0);
      int lvl = 0; double bd = 1e30;
      for (int l = 0; l < 32; ++l) { double d = std::fabs(std::log((double)kNoiseGain[l]) - std::log(std::max(need, 1.0))); if (d < bd) { bd = d; lvl = l; } }
      std::memset(eyH, 0, sizeof eyH); std::memset(ewH, 0, sizeof ewH);
      bw.put(0, 1); bw.put(sendB ? 1 : 0, 1); bw.put((uint32_t)lvl, 5);
      if (sendB) for (int i = 0; i < 8; ++i) bw.put((uint32_t)q.idx[i], kLsfBits[i]);
      return;
    }
    // ---- active block: per-sub-block search with exact integer state tracking
    int32_t coef[kSubBlocks][8]; int32_t lsf[8];
    for (int sbIdx = 0; sbIdx < 4; ++sbIdx) {
      if (sbIdx < 2) for (int i = 0; i < 8; ++i) lsf[i] = (sbIdx == 0) ? (3 * dec.lsfPrev_[i] + 5 * q.cur[i] + 4) >> 3 : (7 * q.cur[i] + dec.lsfPrev_[i] + 4) >> 3;
      else for (int i = 0; i < 8; ++i) lsf[i] = q.cur[i];
      lsp2lpc(coef[sbIdx], lsf);
    }
    int32_t synMem[8]; std::memcpy(synMem, dec.synthMem_, sizeof synMem);
    int32_t yPrev = dec.deemph_;
    int32_t hist[kHistLen]; std::memcpy(hist, dec.hist_, sizeof hist);
    SubChoice ch[kSubBlocks];
    for (int sbIdx = 0; sbIdx < 4; ++sbIdx) {
      double yref[kSubLen]; for (int n = 0; n < kSubLen; ++n) yref[n] = Y((long)T + sbIdx * kSubLen + n);
      searchSub(yref, coef[sbIdx], synMem, yPrev, hist, dec.lastOut_, ch[sbIdx]);
      // exact integer excitation, synthesis, de-emphasis, history update
      int K = dec.kTab_[ch[sbIdx].val];
      int32_t e[kSubLen] = {0};
      for (int i = 0; i < kPvqDim; ++i) e[ch[sbIdx].phase + 3 * i] = mul32(ch[sbIdx].vec[i], kGain[ch[sbIdx].val]) / K;
      int32_t u[kSubLen];
      ltpSubBlock(hist, dec.lastOut_, ch[sbIdx].lag, ch[sbIdx].gain, e, u);
      int32_t tmp[kSubLen]; std::memcpy(tmp, u, sizeof tmp);
      synthFilter(synMem, tmp, coef[sbIdx], kSubLen);
      double yact[kSubLen];
      for (int n = 0; n < kSubLen; ++n) { yPrev = sat16(add32((int16_t)tmp[n], add32(mul32(28180, yPrev), 0x4000) >> 15)); yact[n] = yPrev; }
      updateWeightHist(coef[sbIdx], yref, yact);
    }
    // ---- write bits
    bw.put(1, 1);
    int v7 = ch[0].phase * 27 + ch[1].phase * 9 + ch[2].phase * 3 + ch[3].phase;
    bw.put((uint32_t)v7, 7);
    for (int sbIdx = 0; sbIdx < 4; ++sbIdx) {
      bw.put((uint32_t)ch[sbIdx].val, 5);
      bw.putIndex(ch[sbIdx].index, ch[sbIdx].nbits);
      if (!hdr.noLtp()) {
        if (ch[sbIdx].gain == 0) bw.put(3, 2);
        else { bw.put((uint32_t)ch[sbIdx].lag, 7); bw.put((uint32_t)(ch[sbIdx].gain - 1), 5); }
      }
    }
    for (int i = 0; i < 8; ++i) bw.put((uint32_t)q.idx[i], kLsfBits[i]);
  }
};

// =============================================================================================
Encoder::Encoder() : impl_(new EncoderImpl) {}
Encoder::~Encoder() { delete impl_; }
bool Encoder::init(const StreamHeader& hdr) { hdr_ = hdr; return impl_->init(hdr); }
void Encoder::setLambda(double l) { impl_->lambda = l; }
void Encoder::setMaxVal(int v) { impl_->maxVal = std::min(31, std::max(0, v)); }
void Encoder::setNoiseThreshold(double rmsY) { impl_->noiseThr = rmsY; }
void Encoder::setPerceptualWeighting(double g1, double g2) { impl_->weighting = (g1 > 0); impl_->gam1 = g1; impl_->gam2 = g2; }
int Encoder::algorithmicDelay() const { return impl_->LA; }

int Encoder::encodePacket(const int16_t* pcm, uint8_t* out, size_t maxOut) {
  EncoderImpl& E = *impl_;
  const int nBlocks = E.hdr.blocksPerPacket();
  // append input (DC removal + scale to the codec's internal 14-bit domain)
  for (int i = 0; i < nBlocks * kBlockLen; ++i) {
    double x = pcm[i];
    double y = x - E.hpX + 0.995 * E.hpY; E.hpX = x; E.hpY = y;
    E.yv.push_back(0.5 * y);
  }
  // encode with a rate cap; retry with a lower pulse-count cap if the packet is too long
  int savedMax = E.maxVal;
  Decoder snapshot = E.dec; size_t T0 = E.T; int lastBits0 = E.lastBitsBlock;
  for (int attempt = 0; attempt < 8; ++attempt) {
    BitWriter bw; bw.put(0x30, 6);   // symbol 8 ('110') + 3 ignored bits '000'  => default block count
    E.dec = snapshot; E.T = T0; E.lastBitsBlock = lastBits0;
    for (int b = 0; b < nBlocks; ++b) {
      size_t bits0 = bw.nb;
      E.encodeBlock(bw);
      // commit: run the real decoder on the block we just produced
      Decoder::BitReader br{bw.b.data(), bw.b.size() * 8, bits0};
      int32_t o[kBlockLen]; E.dec.decodeBlock(br, o);
      E.lastBitsBlock = (int)(bw.nb - bits0);
      E.T += kBlockLen;
    }
    if (bw.b.size() <= maxOut) {
      std::memcpy(out, bw.b.data(), bw.b.size());
      E.maxVal = savedMax;
      if (E.T > 4000) { size_t cut = E.T - 1000; E.yv.erase(E.yv.begin(), E.yv.begin() + (long)cut); E.T -= cut; }
      return (int)bw.b.size();
    }
    E.maxVal = std::max(0, E.maxVal - 4);
  }
  // could not fit: drop the packet; the decoder state returns to what the receiver has (it never saw this packet)
  E.dec = snapshot; E.T = T0 + (size_t)nBlocks * kBlockLen; E.lastBitsBlock = lastBits0;
  E.maxVal = savedMax;
  return -1;
}

}  // namespace rtv
