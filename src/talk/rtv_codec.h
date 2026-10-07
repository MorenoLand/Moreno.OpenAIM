// rtv_codec.h - clean-room C++20 reimplementation of the Johnson-Grace "Rtv" speech codec
// as used by AOL Instant Messenger 4.7.2480 "Talk" (jgs2aol.dll encoder / jgs3aol.dll decoder).
//
// Derived by static analysis of the original DLLs and verified against them (see rtv_codec.md).
// No Windows dependencies.  8 kHz, 16-bit mono, 240-sample (30 ms) blocks, up to 6 blocks per packet.
//
//   Decoder  : bit-exact with jgs3aol.dll (verified, see spec section 9).
//   Encoder  : analysis-by-synthesis encoder producing streams the original decoder accepts; it is NOT
//              bit-identical to jgs2aol.dll (the original analysis stages were not reverse engineered).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rtv {

inline constexpr int kBlockLen = 240;        // samples per block (30 ms at 8 kHz)
inline constexpr int kSubLen = 60;           // samples per sub-block
inline constexpr int kSubBlocks = 4;         // sub-blocks per block
inline constexpr int kLpcOrder = 8;
inline constexpr int kPvqDim = 20;           // pulse positions per sub-block (every 3rd sample of 60)
inline constexpr int kHistLen = 120;         // long-term-predictor history (samples)
inline constexpr int kStreamHeaderBytes = 19;
inline constexpr int kMaxBlocksPerPacket = 6;

// 19-byte stream header: 0xF8 followed by nine big-endian 16-bit words (spec section 3.1).
struct StreamHeader {
  uint16_t w[9] = {0x1f40, 0x0100, 0x000c, 0x1f40, 0x0006, 0x0000, 0x1c04, 0x0000, 0x0000};
  int sampleRate() const { return w[0]; }
  int mode() const { return w[2]; }               // 12 = the only mode used by AIM Talk
  int blocksPerPacket() const { return w[4] & 0xF; }
  bool noLtp() const { return (w[4] >> 8) & 1; }  // true: no long-term predictor fields in the stream
  int flags() const { return w[5]; }
  int kMax() const { return w[6] >> 8; }          // pulse-count range: K(val) = kMin + ((val+1)*(kMax-kMin) >> 5)
  int kMin() const { return w[6] & 0xFF; }
};
bool parseStreamHeader(const uint8_t* p, size_t n, StreamHeader& out);  // lenient, like RtvDecodeCreate
bool isCanonicalHeader(const StreamHeader& h);                           // what the original validator (65f828b0) checks
void serializeStreamHeader(const StreamHeader& h, uint8_t out[kStreamHeaderBytes]);

struct EncoderImpl;

// ---------------------------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------------------------
class Decoder {
  friend struct EncoderImpl;
 public:
  Decoder();
  bool init(const StreamHeader& hdr);             // false if the K range (header word 6) is outside 1..38
  // Decode one packet payload (the Rtv bitstream inside a JG frame).  Writes up to
  // kMaxBlocksPerPacket*kBlockLen samples to pcm and returns the number of blocks decoded
  // (negative on a bitstream error).  Output equals the 'int32' output of RtvDecodeBlock
  // (values are always within int16 range).
  int decodePacket(const uint8_t* data, size_t len, int32_t* pcm);

  // MSB-first bit reader (reads zeros past the end).
  struct BitReader {
    const uint8_t* p; size_t nbits; size_t pos;
    uint32_t get(int n);
  };
  // Decode one block from a reader positioned at its first bit (used by decodePacket and by tests).
  void decodeBlock(BitReader& br, int32_t out[kBlockLen]);

  // Parsed parameters of the last decoded block (analysis / tests).
  struct BlockInfo {
    int active, noiseFlag, noiseLevel;
    int lsfIdx[8];
    int phase[4], val[4], lag[4], gain[4];
    uint64_t pvq[4];
    int bits;
  };
  BlockInfo lastBlock() const {
    BlockInfo b{};
    b.active = active_; b.noiseFlag = noiseFlag_; b.noiseLevel = noiseLevel_;
    for (int i = 0; i < 8; ++i) b.lsfIdx[i] = lsfIdx_[i];
    for (int i = 0; i < 4; ++i) { b.phase[i] = phase_[i]; b.val[i] = sub_[i].val; b.lag[i] = sub_[i].lag; b.gain[i] = sub_[i].gain; b.pvq[i] = sub_[i].index; }
    b.bits = lastBits_;
    return b;
  }
  // Intermediate buffers of the last decoded block (tests/debug).
  struct Debug { int32_t raw[kBlockLen]; int32_t exc[kBlockLen]; int32_t synth[kBlockLen]; } dbg;

 private:
  struct SubParams { int val, nbits; uint64_t index; int lag, gain; };
  StreamHeader hdr_;
  int nBlocksDefault_ = 6;
  bool noLtp_ = false;
  int kMaxP_ = 28, kMinP_ = 4;
  int kTab_[32];
  uint64_t pvq_[39][kPvqDim + 1];
  // per-block parsed state
  int active_ = 0, noiseFlag_ = 0, noiseLevel_ = 0;
  int lsfIdx_[kLpcOrder];
  int lsfIdxPrev_[kLpcOrder];          // previous block's indices (re-used by mode 4 when flagged)
  bool mode4_ = false;                 // header word 2 == 4: alternate field order, every block active
  int phase_[kSubBlocks];
  SubParams sub_[kSubBlocks];
  // persistent synthesis state
  int32_t lsfPrev_[kLpcOrder], lsfCur_[kLpcOrder], lsfNoise_[kLpcOrder];
  int32_t synthMem_[kLpcOrder];
  int32_t deemph_ = 0;
  int32_t hist_[kHistLen];
  int32_t lastOut_[kBlockLen] = {};   // previous block's final output (read by the LTP for lag index > 90)
  uint32_t seed_ = 0x55555555;
  int lastBits_ = 0;

  void unpackBlock(BitReader& br);
  void unpackMode4(BitReader& br);
  void readPvq(BitReader& br, int sb);
  void readPitch(BitReader& br, int sb);
  void subExcitation(int sb, int32_t* out60) const;
  void ltp(int32_t* excIn240, int32_t* dest240);
  void noiseExcitation(int32_t* dest240);
  void synthesize(const int32_t* exc, int32_t* out);
};

// ---------------------------------------------------------------------------------------------
// Encoder (compatible with the original decoder; not bit-identical to the original encoder)
// ---------------------------------------------------------------------------------------------
class Encoder {
 public:
  Encoder();
  ~Encoder();
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;

  bool init(const StreamHeader& hdr = StreamHeader());   // only mode 12 with LTP is supported
  // Encode blocksPerPacket*240 samples (int16, 8 kHz) into one packet payload (Rtv bitstream, without the
  // 2-byte JG frame header).  Returns the number of bytes written (<= maxOut), or -1 if it cannot fit.
  int encodePacket(const int16_t* pcm, uint8_t* out, size_t maxOut);
  const StreamHeader& header() const { return hdr_; }
  // Tuning (defaults give roughly 4-6 kbit/s on speech).
  void setLambda(double l);             // rate/distortion trade-off (higher = fewer bits)
  void setMaxVal(int v);                // cap on the 5-bit gain/pulse-count index (0..31)
  void setNoiseThreshold(double rmsY);  // blocks quieter than this (rms of input/2) are sent as comfort noise
  void setPerceptualWeighting(double g1, double g2);   // search in the domain of W(z)=A(z/g1)/A(z/g2); g1<=0 disables (pure MSE)
  int algorithmicDelay() const;         // samples of delay between input and decoded output (40)

 private:
  StreamHeader hdr_;
  EncoderImpl* impl_ = nullptr;
};

// ---------------------------------------------------------------------------------------------
// JG container framing as seen on the wire in AIM Talk media payloads (see spec section 4).
// One media datagram/segment payload (after the 2-byte packet counter) is:
//   first packet : kStreamPreamble (39 bytes) + frame
//   later packets: frame
// frame = [0x80 | (len & 0x3F)] [0x01 | ((len >> 6) << 5)] [len bytes of Rtv bitstream]
// ---------------------------------------------------------------------------------------------
inline constexpr size_t kPreambleBytes = 39;
extern const uint8_t kStreamPreamble[kPreambleBytes];
inline constexpr size_t kMaxFramePayload = 250;       // the original sender limits its output to 252 bytes incl. the 2-byte frame header

std::vector<uint8_t> makeFrame(const uint8_t* payload, size_t len);   // len <= 255

struct ParsedPacket {
  bool hasPreamble = false;      // "JG" stream header + stream-info items present
  bool hasStreamHeader = false;  // the 19-byte Rtv header was found
  StreamHeader header;
  size_t payloadOff = 0, payloadLen = 0;   // first audio frame
  size_t consumed = 0;                     // bytes up to the end of that frame
};
// Parse a media payload.  Returns false if no valid audio frame is found.
bool parsePacket(const uint8_t* p, size_t n, ParsedPacket& out);

// Sender-side convenience: the first call prepends the preamble.
class Packetizer {
 public:
  std::vector<uint8_t> pack(const uint8_t* rtvPayload, size_t len);
  void reset() { first_ = true; }
 private:
  bool first_ = true;
};

}  // namespace rtv
