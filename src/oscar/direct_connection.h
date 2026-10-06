#pragma once
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

// "IM Image" direct connection (Research/direct_im.md section 2): plain TCP on port 4443 carrying ODC2 frames, a
// 76-byte big-endian header (magic, header length, type 1, rendezvous cookie, payload length, encoding, flags,
// sender screen name) followed by the payload.
class DirectConnection final : public QObject {
  Q_OBJECT
public:
  static constexpr quint16 Port = 4443;            // both bind and connect use 4443 (icbmui 0x1138b9d0)
  enum Flag : quint8 { AutoResponse = 0x01, TypingFrame = 0x02, Typed = 0x04, Typing = 0x08, Recording = 0x10 };
  DirectConnection(quint64 cookie, const QString &ownScreenName, QObject *parent = nullptr);
  ~DirectConnection() override;
  bool listen();                                    // DirectListen: the proposer (or the reverse-connect acceptor)
  void connectTo(const QList<QHostAddress> &addresses); // DirectConnect: each address in turn, 20 s each
  bool isConnected() const { return socket_ && socket_->state() == QAbstractSocket::ConnectedState && ready_; }
  bool isListening() const { return server_.isListening(); }
  quint64 cookie() const { return cookie_; }
  void sendMessage(const QByteArray &payload, quint16 encoding, bool autoResponse = false);
  void sendTyping(quint8 state);                     // 0x02 idle, 0x06 typed, 0x0E typing, 0x16 recording
  void close();
signals:
  void connected();
  void connectFailed();                              // every address failed (event 0xA)
  void messageReceived(const QByteArray &payload, quint16 encoding, quint8 flags);
  void typingChanged(quint8 flags);
  void closed();
private:
  void adopt(QTcpSocket *socket);
  void tryNext();
  void readFrames();
  QByteArray header(quint32 payloadLength, quint16 encoding, quint8 flags) const;
  quint64 cookie_;
  QByteArray ownName_;
  QTcpServer server_;
  QPointer<QTcpSocket> socket_;
  QList<QHostAddress> addresses_;
  QTimer connectTimer_;
  QByteArray buffer_;
  bool ready_ = false, closing_ = false;
};
