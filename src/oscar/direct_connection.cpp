#include "direct_connection.h"
#include <QtEndian>

namespace {
constexpr int HeaderSize = 0x4C;
void put16(QByteArray &out, quint16 value) { out.append(char(value >> 8)); out.append(char(value)); }
void put32(QByteArray &out, quint32 value) { put16(out, quint16(value >> 16)); put16(out, quint16(value)); }
quint16 get16(const QByteArray &in, int at) { return quint16((quint8(in[at]) << 8) | quint8(in[at + 1])); }
quint32 get32(const QByteArray &in, int at) { return (quint32(get16(in, at)) << 16) | get16(in, at + 2); }
}

DirectConnection::DirectConnection(quint64 cookie, const QString &ownScreenName, QObject *parent) : QObject(parent), cookie_(cookie), ownName_(ownScreenName.toLatin1().left(31)) {
  if (ownName_.isEmpty()) ownName_ = QByteArrayLiteral("[Remote]");
  connectTimer_.setSingleShot(true);
  connect(&connectTimer_, &QTimer::timeout, this, [this] { if (socket_ && !ready_) { socket_->abort(); socket_->deleteLater(); socket_.clear(); tryNext(); } });
  connect(&server_, &QTcpServer::newConnection, this, [this] {
    while (QTcpSocket *incoming = server_.nextPendingConnection()) {
      if (ready_) { incoming->abort(); incoming->deleteLater(); continue; } // first connection wins
      server_.close(); adopt(incoming);
    }
  });
}
DirectConnection::~DirectConnection() { closing_ = true; if (socket_) socket_->abort(); }

bool DirectConnection::listen() {
  if (server_.isListening()) return true;
  return server_.listen(QHostAddress::AnyIPv4, Port);
}
void DirectConnection::connectTo(const QList<QHostAddress> &addresses) {
  addresses_ = addresses; tryNext();
}
void DirectConnection::tryNext() {
  if (ready_) return;
  if (addresses_.isEmpty()) { emit connectFailed(); return; }
  const QHostAddress address = addresses_.takeFirst();
  auto *socket = new QTcpSocket(this); socket_ = socket;
  connect(socket, &QTcpSocket::connected, this, [this, socket] { connectTimer_.stop(); if (ready_) { socket->abort(); return; } server_.close(); adopt(socket); });
  connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) { if (ready_ || socket != socket_) return; connectTimer_.stop(); socket->deleteLater(); socket_.clear(); tryNext(); });
  connectTimer_.start(20000); // 0x4e20
  socket->connectToHost(address, Port);
}
void DirectConnection::adopt(QTcpSocket *socket) {
  socket_ = socket; socket->setParent(this); ready_ = true; buffer_.clear();
  socket->disconnect(this);
  connect(socket, &QTcpSocket::readyRead, this, &DirectConnection::readFrames);
  connect(socket, &QTcpSocket::disconnected, this, [this] { if (!closing_ && ready_) { ready_ = false; emit closed(); } });
  emit connected();
  readFrames();
}

QByteArray DirectConnection::header(quint32 payloadLength, quint16 encoding, quint8 flags) const {
  QByteArray out; out.reserve(HeaderSize);
  out.append("ODC2", 4); put16(out, HeaderSize); put16(out, 1); put16(out, 6); put16(out, 0);
  QByteArray cookie(8, 0); qToBigEndian(cookie_, cookie.data()); out.append(cookie);
  out.append(QByteArray(8, 0)); put32(out, payloadLength); put16(out, encoding); put16(out, 0);
  put32(out, flags); put32(out, 0);
  QByteArray name = ownName_; name.resize(32, 0); out.append(name);
  return out;
}
void DirectConnection::sendMessage(const QByteArray &payload, quint16 encoding, bool autoResponse) {
  if (!isConnected()) return;
  socket_->write(header(quint32(payload.size()), encoding, autoResponse ? AutoResponse : 0) + payload);
}
void DirectConnection::sendTyping(quint8 state) { if (isConnected()) socket_->write(header(0, 0, state)); }
void DirectConnection::close() {
  closing_ = true; connectTimer_.stop(); server_.close();
  if (socket_) { socket_->disconnectFromHost(); socket_->deleteLater(); socket_.clear(); }
  const bool was = ready_; ready_ = false; if (was) emit closed();
  closing_ = false;
}

void DirectConnection::readFrames() {
  if (!socket_) return;
  buffer_.append(socket_->readAll());
  for (;;) {
    if (buffer_.size() < 6) return;
    if (!buffer_.startsWith("ODC2")) { close(); return; } // receive check of the magic (0x1138b4d6)
    const int headerLength = get16(buffer_, 4);
    if (headerLength < HeaderSize || headerLength > HeaderSize + 0x100) { close(); return; }
    if (buffer_.size() < headerLength) return;
    if (qFromBigEndian<quint64>(buffer_.constData() + 0x0C) != cookie_) { close(); return; } // cookie memcmp (0x1138b50b)
    const quint32 payloadLength = get32(buffer_, 0x1C);
    if (quint64(buffer_.size()) < quint64(headerLength) + payloadLength) return;
    const quint16 type = get16(buffer_, 6), encoding = get16(buffer_, 0x20); const quint8 flags = quint8(buffer_[0x27]);
    const QByteArray payload = buffer_.mid(headerLength, int(payloadLength));
    buffer_.remove(0, headerLength + int(payloadLength));
    if (type == 2) continue; // ignored by the original (0x1138b6e0)
    if (flags & TypingFrame) emit typingChanged(flags);
    if (!payload.isEmpty()) emit messageReceived(payload, encoding, flags);
  }
}
