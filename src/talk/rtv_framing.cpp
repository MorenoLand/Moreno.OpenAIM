// rtv_framing.cpp - JG container framing used on the AIM Talk wire (see rtv_codec.md section 4).
#include "rtv_codec.h"

namespace rtv {

const uint8_t kStreamPreamble[kPreambleBytes] = {
    0x4A, 0x47, 0x03, 0x0E, 0x00, 0x00, 0x00, 0x00,                     // "JG", version 3.14, 4 reserved bytes
    0x15, 0x00, 0x78, 0x01,                                             // item: length 21, type 0, {0x78 0x01}
    0xF8, 0x1F, 0x40, 0x01, 0x00, 0x00, 0x0C, 0x1F, 0x40, 0x00, 0x06,  // 19-byte Rtv stream header
    0x00, 0x00, 0x1C, 0x04, 0x00, 0x00, 0x00, 0x00,
    0xE7, 0x05, 0x00, 0x79, 0x00, 0x00, 0x00, 0x00};                    // marker item (ignored by the original decoder)

std::vector<uint8_t> makeFrame(const uint8_t* payload, size_t len) {
  std::vector<uint8_t> f;
  f.reserve(len + 2);
  f.push_back((uint8_t)(0x80 | (len & 0x3F)));
  f.push_back((uint8_t)(0x01 | (((len >> 6) & 3) << 5)));
  f.insert(f.end(), payload, payload + len);
  return f;
}

bool parsePacket(const uint8_t* p, size_t n, ParsedPacket& out) {
  out = ParsedPacket{};
  size_t pos = 0;
  if (n >= 8 && p[0] == 'J' && p[1] == 'G') {
    out.hasPreamble = true;
    pos = 8;
    // Items before the first audio frame.  "Plain" items have bit 7 of the first byte clear:
    // [length][type][length bytes].  Item type 0 with first body byte 0x78 carries the Rtv stream header.
    while (pos + 2 <= n && (p[pos] & 0x80) == 0) {
      size_t len = p[pos];
      if (pos + 2 + len > n) return false;
      if (len >= 2 + (size_t)kStreamHeaderBytes && p[pos + 2] == 0x78 &&
          parseStreamHeader(p + pos + 4, len - 2, out.header))
        out.hasStreamHeader = true;
      pos += 2 + len;
    }
    // Marker items (first byte >= 0xC0), 8 bytes in practice; the original decoder does not need them.
    while (pos + 8 <= n && (p[pos] & 0xC0) == 0xC0) pos += 8;
  }
  if (pos + 2 > n || (p[pos] & 0xC0) != 0x80) return false;
  size_t len = (size_t)(p[pos] & 0x3F) | ((size_t)((p[pos + 1] >> 5) & 3) << 6);
  if ((p[pos + 1] & 0x1F) != 0x01 || pos + 2 + len > n) return false;
  out.payloadOff = pos + 2;
  out.payloadLen = len;
  out.consumed = pos + 2 + len;
  return true;
}

std::vector<uint8_t> Packetizer::pack(const uint8_t* rtvPayload, size_t len) {
  std::vector<uint8_t> pkt;
  if (first_) { pkt.assign(kStreamPreamble, kStreamPreamble + kPreambleBytes); first_ = false; }
  std::vector<uint8_t> f = makeFrame(rtvPayload, len);
  pkt.insert(pkt.end(), f.begin(), f.end());
  return pkt;
}

}  // namespace rtv
