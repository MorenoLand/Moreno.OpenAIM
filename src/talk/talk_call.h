#pragma once
#include "rtv_codec.h"
#include "talk_audio.h"
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <memory>

// One Talk call, the JGTkAol engine (Research/talk_protocol.md sections 3-7):
//  - a TCP listener on a random port 1112..3333 and a TCP connector, first connection wins;
//  - the 18-byte handshake PDUs (0 greeting, 1 hello+cookie, 2 UDP port, 3 TCP-media port, 4 TCP-media request,
//    5 UDP ok, 6 use TCP);
//  - media: UDP datagrams "u16 counter | payload" (TCP fallback "u16 length | u16 counter | payload"), payload =
//    one Rtv frame (the first one preceded by the JG stream preamble), 180 ms of 8 kHz speech each;
//  - control frames "JgPc" on the signalling socket: 1 duplex, 4 start, 5 stop, 6 hold, 7 resume, 8 hang up.
class TalkCall final : public QObject {
  Q_OBJECT
public:
  enum class Role { Caller, Callee };
  TalkCall(quint64 cookie, Role role, bool forceHalfDuplex, QObject *parent = nullptr);
  ~TalkCall() override;
  quint16 listen();                                   // TCP signalling listener, 0 on failure
  void connectTo(const QList<QHostAddress> &addresses, quint16 port); // all addresses at once, first connection wins
  void connectTo(const QHostAddress &address, quint16 port) { connectTo(QList<QHostAddress>{address}, port); }
  void setPeerAddress(const QHostAddress &address) { peer_ = address; }
  bool isConnected() const { return connected_; }
  bool fullDuplex() const { return localDuplex_ && remoteDuplex_; }
  bool sending() const { return sending_; }
  bool remoteSending() const { return remoteSending_; }
  bool paused() const { return localHold_; }
  bool remotePaused() const { return remoteHold_; }
  void startSending();                                // START (4)
  void stopSending();                                 // STOP (5)
  void setHold(bool hold);                            // HOLD (6) / RESUME (7)
  void hangUp();                                      // BYE (8), then close
  TalkAudio &audio() { return audio_; }
  bool audioAvailable() const { return audioOpen_; }
signals:
  void connected();
  void stateChanged();                                // duplex / sending / hold changes
  void remoteStoppedTalking();                        // half duplex: the buddy clicked Push to Listen (sound talkstop)
  void audioReceived(int samples);                    // decoded speech handed to the speaker
  void ended(bool byPeer);
  void failed(const QString &reason);
private:
  enum class Media { Undecided, Udp, Tcp };
  void adoptSignalling(QTcpSocket *socket, bool weConnected);
  void readSignalling();
  void handlePdu(const QByteArray &pdu);
  void handleControl(quint8 type, const QByteArray &payload);
  void sendPdu(quint16 type, quint16 port = 0, bool withCookie = false);
  void sendControl(quint8 type, const QByteArray &payload = {});
  void startUdpProbe();
  void finishMedia(Media media);
  void sendMedia(const QByteArray &pcm);
  void receiveMedia(const QByteArray &payload);
  void readUdp();
  void readTcpMedia();
  void close(bool byPeer);
  quint64 cookie_;
  Role role_;
  QTcpServer listener_, mediaListener_;
  QList<QPointer<QTcpSocket>> connectors_;
  QPointer<QTcpSocket> signalling_, mediaSocket_;
  QUdpSocket udp_;
  QHostAddress peer_;
  quint16 udpPort_ = 0, peerUdpPort_ = 0;
  QByteArray signallingBuffer_, mediaBuffer_;
  bool weConnected_ = false, handshakeDone_ = false, connected_ = false, closing_ = false, sentType2_ = false, gotType2_ = false;
  Media media_ = Media::Undecided;
  QTimer probeTimer_, connectTimer_;
  bool probing_ = false;
  bool localDuplex_ = true, remoteDuplex_ = true, sending_ = false, remoteSending_ = false, localHold_ = false, remoteHold_ = false;
  TalkAudio audio_;
  bool audioOpen_ = false;
  rtv::Encoder encoder_;
  rtv::Packetizer packetizer_;
  rtv::Decoder decoder_;
  bool decoderReady_ = false;
  quint16 counter_ = 0;
  QByteArray captureBuffer_;
};
