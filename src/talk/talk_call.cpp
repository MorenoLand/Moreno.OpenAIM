#include "talk_call.h"
#include <QRandomGenerator>
#include <QtEndian>

namespace {
constexpr int PduSize = 18;
constexpr quint16 Greeting = 0, Hello = 1, MediaPort = 2, TcpMediaPort = 3, TcpMediaRequest = 4, UdpOk = 5, UseTcp = 6;
constexpr quint8 Duplex = 1, Start = 4, Stop = 5, Hold = 6, Resume = 7, Bye = 8;
QByteArray be16(quint16 v) { QByteArray b(2, 0); qToBigEndian(v, b.data()); return b; }
// Port allocator (jgtkaol 0x10005e2f): 1112..3333, up to 32 attempts.
template <typename Bind> quint16 bindRandom(Bind bind) {
  for (int attempt = 0; attempt < 32; ++attempt) { const quint16 port = quint16(0x0458 + QRandomGenerator::global()->bounded(0x0D05 - 0x0458 + 1)); if (bind(port)) return port; }
  return 0;
}
}

TalkCall::TalkCall(quint64 cookie, Role role, bool forceHalfDuplex, QObject *parent) : QObject(parent), cookie_(cookie), role_(role) {
  audioOpen_ = audio_.open();
  localDuplex_ = !forceHalfDuplex && audio_.fullDuplexCapable();
  encoder_.init(); decoderReady_ = false;
  probeTimer_.setSingleShot(true); probeTimer_.setInterval(10000); // option b: UDP probe 10 s
  connect(&probeTimer_, &QTimer::timeout, this, [this] { if (probing_) { probing_ = false; sendPdu(UseTcp); finishMedia(Media::Tcp); } });
  connectTimer_.setSingleShot(true); connectTimer_.setInterval(120000); // option a: 120 s
  connect(&connectTimer_, &QTimer::timeout, this, [this] { if (!connected_) { emit failed(QStringLiteral("timeout")); close(false); } });
  connect(&listener_, &QTcpServer::newConnection, this, [this] {
    while (QTcpSocket *incoming = listener_.nextPendingConnection()) { if (signalling_) { incoming->abort(); incoming->deleteLater(); continue; } adoptSignalling(incoming, false); }
  });
  connect(&mediaListener_, &QTcpServer::newConnection, this, [this] {
    while (QTcpSocket *incoming = mediaListener_.nextPendingConnection()) { if (mediaSocket_) { incoming->abort(); incoming->deleteLater(); continue; } mediaSocket_ = incoming; connect(incoming, &QTcpSocket::readyRead, this, &TalkCall::readTcpMedia); }
  });
  udpPort_ = bindRandom([this](quint16 port) { return udp_.bind(QHostAddress::AnyIPv4, port); });
  connect(&udp_, &QUdpSocket::readyRead, this, &TalkCall::readUdp);
  connect(&audio_, &TalkAudio::captured, this, &TalkCall::sendMedia);
  connectTimer_.start();
}
TalkCall::~TalkCall() { closing_ = true; audio_.close(); }

quint16 TalkCall::listen() {
  if (listener_.isListening()) return listener_.serverPort();
  return bindRandom([this](quint16 port) { return listener_.listen(QHostAddress::AnyIPv4, port); });
}
void TalkCall::connectTo(const QList<QHostAddress> &addresses, quint16 port) {
  if (signalling_ || !port) return;
  for (const QHostAddress &address : addresses) {
    if (address.isNull()) continue;
    auto *socket = new QTcpSocket(this); connectors_.append(socket);
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(socket, &QTcpSocket::connected, this, [this, socket] { if (signalling_) { socket->abort(); return; } adoptSignalling(socket, true); });
    connect(socket, &QTcpSocket::errorOccurred, this, [socket](QAbstractSocket::SocketError) { socket->deleteLater(); });
    socket->connectToHost(address, port);
  }
}
void TalkCall::adoptSignalling(QTcpSocket *socket, bool weConnected) {
  signalling_ = socket; weConnected_ = weConnected; listener_.close();
  for (const auto &other : connectors_) if (other && other != socket) { other->disconnect(this); other->abort(); other->deleteLater(); }
  peer_ = socket->peerAddress(); // media goes to the address the signalling connection actually uses
  socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
  connect(socket, &QTcpSocket::readyRead, this, &TalkCall::readSignalling);
  connect(socket, &QTcpSocket::disconnected, this, [this] { if (!closing_) { emit ended(true); close(true); } });
  // Case 1 (the caller connected): HELLO(cookie) + MEDIA_PORT. Case 2 (the caller accepted): GREETING.
  if (role_ == Role::Caller) { if (weConnected) { sendPdu(Hello, 0, true); sendPdu(MediaPort, udpPort_); sentType2_ = true; } else sendPdu(Greeting); }
  readSignalling();
}

void TalkCall::sendPdu(quint16 type, quint16 port, bool withCookie) {
  if (!signalling_) return;
  QByteArray pdu(PduSize, 0); qToBigEndian(type, pdu.data()); if (withCookie) qToBigEndian(cookie_, pdu.data() + 2); qToBigEndian(port, pdu.data() + 10);
  signalling_->write(pdu);
}
void TalkCall::sendControl(quint8 type, const QByteArray &payload) {
  if (!signalling_ || !handshakeDone_) return;
  QByteArray frame("JgPc"); frame.append(char(6 + payload.size())); frame.append(char(type)); frame.append(payload); signalling_->write(frame);
}

void TalkCall::readSignalling() {
  if (!signalling_) return;
  signallingBuffer_.append(signalling_->readAll());
  while (!signallingBuffer_.isEmpty()) {
    if (!handshakeDone_) {
      if (signallingBuffer_.size() < PduSize) return;
      const QByteArray pdu = signallingBuffer_.left(PduSize); signallingBuffer_.remove(0, PduSize); handlePdu(pdu);
      if (closing_) return;
      continue;
    }
    if (signallingBuffer_.size() < 6) return;
    if (!signallingBuffer_.startsWith("JgPc")) { emit failed(QStringLiteral("protocol")); close(false); return; }
    const int length = quint8(signallingBuffer_[4]); if (length < 6) { close(false); return; }
    if (signallingBuffer_.size() < length) return;
    const quint8 type = quint8(signallingBuffer_[5]); const QByteArray payload = signallingBuffer_.mid(6, length - 6); signallingBuffer_.remove(0, length);
    handleControl(type, payload); if (closing_) return;
  }
}

void TalkCall::handlePdu(const QByteArray &pdu) {
  const quint16 type = qFromBigEndian<quint16>(pdu.constData()), port = qFromBigEndian<quint16>(pdu.constData() + 10);
  switch (type) {
  case Greeting: // callee connected to the caller's listener: answer HELLO + MEDIA_PORT
    if (role_ != Role::Callee) break;
    sendPdu(Hello, 0, true); sendPdu(MediaPort, udpPort_); sentType2_ = true; return;
  case Hello:
    if (qFromBigEndian<quint64>(pdu.constData() + 2) != cookie_) { emit failed(QStringLiteral("cookie")); close(false); return; }
    if (!sentType2_) { sendPdu(MediaPort, udpPort_); sentType2_ = true; }
    return;
  case MediaPort:
    peerUdpPort_ = port; gotType2_ = true;
    udp_.connectToHost(peer_, peerUdpPort_); // connected UDP socket (send/recv)
    if (!sentType2_) { sendPdu(MediaPort, udpPort_); sentType2_ = true; }
    // The caller picks the TCP-media fallback side: its own listener (type 3) if it connected, else ask (type 4).
    if (role_ == Role::Caller) {
      if (weConnected_) { const quint16 mediaPort = bindRandom([this](quint16 p) { return mediaListener_.listen(QHostAddress::AnyIPv4, p); }); sendPdu(TcpMediaPort, mediaPort); for (int i = 0; i < 5; ++i) udp_.write(QByteArray(1, 0)); }
      else sendPdu(TcpMediaRequest);
    }
    return;
  case TcpMediaRequest: {
    const quint16 mediaPort = bindRandom([this](quint16 p) { return mediaListener_.listen(QHostAddress::AnyIPv4, p); });
    sendPdu(TcpMediaPort, mediaPort); for (int i = 0; i < 5; ++i) udp_.write(QByteArray(1, 0)); return;
  }
  case TcpMediaPort: { // connect the TCP media socket and probe UDP
    auto *socket = new QTcpSocket(this); mediaSocket_ = socket; connect(socket, &QTcpSocket::readyRead, this, &TalkCall::readTcpMedia); socket->connectToHost(peer_, port);
    startUdpProbe(); return;
  }
  case UdpOk: finishMedia(Media::Udp); return;
  case UseTcp: finishMedia(Media::Tcp); return;
  default: emit failed(QStringLiteral("protocol")); close(false); return;
  }
}
void TalkCall::startUdpProbe() { probing_ = true; for (int i = 0; i < 5; ++i) udp_.write(QByteArray(1, 0)); probeTimer_.start(); }

void TalkCall::finishMedia(Media media) {
  if (connected_) return;
  media_ = media; probeTimer_.stop(); probing_ = false;
  if (media == Media::Udp) { if (mediaSocket_) { mediaSocket_->abort(); mediaSocket_->deleteLater(); } mediaListener_.close(); }
  handshakeDone_ = true; connected_ = true; connectTimer_.stop();
  sendControl(Duplex, QByteArray(1, char(localDuplex_ ? 1 : 0))); // first control frame: our full-duplex capability
  emit connected();
  if (localDuplex_) startSending(); // full duplex talks from the start; half duplex waits for "Push to Talk"
}

void TalkCall::handleControl(quint8 type, const QByteArray &payload) {
  switch (type) {
  case Duplex:
    remoteDuplex_ = !payload.isEmpty() && payload[0] != 0;
    if (!fullDuplex() && sending_ && remoteSending_) stopSending(); // half duplex: only one side talks
    emit stateChanged(); return;
  case Start: remoteSending_ = true; if (!fullDuplex() && sending_) { sending_ = false; audio_.setCapturing(false); } emit stateChanged(); return;
  case Stop: remoteSending_ = false; if (!fullDuplex()) emit remoteStoppedTalking(); emit stateChanged(); return;
  case Hold: remoteHold_ = true; emit stateChanged(); return;
  case Resume: remoteHold_ = false; emit stateChanged(); return;
  case Bye: emit ended(true); close(true); return;
  default: return; // 2, 3, 9, 10, 11: ignored here as in the original's sender set
  }
}

void TalkCall::startSending() {
  if (!connected_ || sending_) return;
  if (!fullDuplex() && remoteSending_) remoteSending_ = false; // taking the floor in half duplex
  sending_ = true; audio_.setCapturing(true); captureBuffer_.clear(); sendControl(Start); emit stateChanged();
}
void TalkCall::stopSending() {
  if (!sending_) return;
  sending_ = false; audio_.setCapturing(false); sendControl(Stop); emit stateChanged();
}
void TalkCall::setHold(bool hold) {
  if (localHold_ == hold || !connected_) return;
  localHold_ = hold; audio_.setPaused(hold); sendControl(hold ? Hold : Resume); emit stateChanged();
}
void TalkCall::hangUp() {
  if (closing_) return;
  if (connected_) sendControl(Bye);
  if (signalling_) signalling_->flush();
  close(false);
}
void TalkCall::close(bool byPeer) {
  Q_UNUSED(byPeer);
  if (closing_) return; closing_ = true;
  connected_ = false; probeTimer_.stop(); connectTimer_.stop(); audio_.setCapturing(false); audio_.close();
  listener_.close(); mediaListener_.close(); udp_.close();
  for (const auto &other : connectors_) if (other && other != signalling_) { other->disconnect(this); other->abort(); other->deleteLater(); }
  for (QTcpSocket *s : {signalling_.data(), mediaSocket_.data()}) if (s) { s->disconnect(this); s->disconnectFromHost(); s->deleteLater(); }
}

void TalkCall::sendMedia(const QByteArray &pcm) {
  if (!connected_ || !sending_ || localHold_ || remoteHold_) return;
  captureBuffer_.append(pcm);
  while (captureBuffer_.size() >= TalkAudio::ChunkBytes) {
    const QByteArray chunk = captureBuffer_.left(TalkAudio::ChunkBytes); captureBuffer_.remove(0, TalkAudio::ChunkBytes);
    uint8_t bits[rtv::kMaxFramePayload]; const int length = encoder_.encodePacket(reinterpret_cast<const int16_t *>(chunk.constData()), bits, sizeof(bits));
    if (length <= 0) continue;
    const std::vector<uint8_t> packet = packetizer_.pack(bits, size_t(length));
    const QByteArray payload(reinterpret_cast<const char *>(packet.data()), int(packet.size()));
    if (media_ == Media::Tcp && mediaSocket_) mediaSocket_->write(be16(quint16(payload.size() + 4)) + be16(counter_) + payload);
    else udp_.write(be16(counter_) + payload);
    ++counter_;
  }
}
void TalkCall::receiveMedia(const QByteArray &payload) {
  if (localHold_ || remoteHold_) return;
  rtv::ParsedPacket parsed; if (!rtv::parsePacket(reinterpret_cast<const uint8_t *>(payload.constData()), size_t(payload.size()), parsed)) return;
  if (parsed.hasStreamHeader) decoderReady_ = decoder_.init(parsed.header);
  else if (!decoderReady_) decoderReady_ = decoder_.init(rtv::StreamHeader());
  if (!decoderReady_) return;
  int32_t samples[rtv::kMaxBlocksPerPacket * rtv::kBlockLen];
  const int blocks = decoder_.decodePacket(reinterpret_cast<const uint8_t *>(payload.constData()) + parsed.payloadOff, parsed.payloadLen, samples);
  if (blocks <= 0) return;
  QByteArray pcm(blocks * rtv::kBlockLen * 2, 0); auto *out = reinterpret_cast<qint16 *>(pcm.data());
  for (int i = 0; i < blocks * rtv::kBlockLen; ++i) out[i] = qint16(qBound(-32768, int(samples[i]), 32767));
  audio_.play(pcm); emit audioReceived(blocks * rtv::kBlockLen);
}
void TalkCall::readUdp() {
  while (udp_.hasPendingDatagrams()) {
    QByteArray datagram(int(udp_.pendingDatagramSize()), 0); udp_.readDatagram(datagram.data(), datagram.size());
    if (probing_) { probing_ = false; sendPdu(UdpOk); finishMedia(Media::Udp); }
    if (datagram.size() < 3 || !connected_) continue; // 1-byte probes and anything before the call is up
    receiveMedia(datagram.mid(2));
  }
}
void TalkCall::readTcpMedia() {
  if (!mediaSocket_) return;
  mediaBuffer_.append(mediaSocket_->readAll());
  while (mediaBuffer_.size() >= 4) {
    const int length = qFromBigEndian<quint16>(mediaBuffer_.constData()); if (length < 4) { mediaBuffer_.clear(); return; }
    if (mediaBuffer_.size() < length) return;
    const QByteArray payload = mediaBuffer_.mid(4, length - 4); mediaBuffer_.remove(0, length);
    if (connected_ && media_ == Media::Tcp) receiveMedia(payload);
  }
}
