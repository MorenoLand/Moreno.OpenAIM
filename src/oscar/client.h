#pragma once
#include "protocol.h"
#include <QSet>
#include <QStringList>
#include <QTcpSocket>
#include <QHash>
#include <QSharedPointer>
#include <QTimer>

class OscarClient final : public QObject {
  Q_OBJECT
public:
  explicit OscarClient(QObject *parent = nullptr);
  ~OscarClient() override;
  void signOn(const QString &host, quint16 port, const QString &screenName, const QString &password);
  void signOff();
  const QVector<aim::oscar::FeedbagItem> &roster() const;
  bool isOnline(const QString &screenName) const;
  int idleMinutes(const QString &screenName) const; // 0 when not idle
  bool isBuddyAway(const QString &screenName) const;
  QString screenName() const;
  bool connected() const;
  bool sendMessage(const QString &recipient, const QString &text);
  bool sendAutoResponse(const QString &recipient, const QString &text); // away/idle reply, flagged with TLV 4
  void setIdle(quint32 seconds);                                          // SNAC(01,11); 0 = no longer idle
  QString awayText() const { return awayText_; }
  void requestUserInfo(const QString &name);
  void createChatRoom(const QString &name, quint16 exchange = 4);
  void joinChatRoom(const aim::oscar::ChatRoom &room);
  bool sendChatMessage(const QString &roomCookie, const QString &text);
  bool inviteToChat(const QString &recipient, const QString &roomCookie, const QString &text);
  void respondToChatInvitation(const aim::oscar::ChatInvitation &invitation, bool accept);
  void leaveChatRoom(const QString &cookie);
  bool applyRoster(const QVector<aim::oscar::FeedbagItem> &desiredFullRoster);
  bool addGroup(const QString &name);
  bool renameGroup(quint16 groupId, const QString &name);
  bool removeGroup(quint16 groupId);
  bool addBuddy(quint16 groupId, const QString &name);
  bool blockUser(const QString &name);            // adds a deny (class 3) feedbag item
  bool unblockUser(const QString &name);          // removes the deny item
  bool isBlocked(const QString &name) const;
  // Privacy: PDINFO mode (1 allow all, 2 block all, 3 allow listed, 4 block listed, 5 allow Buddy List) and the lists.
  quint8 privacyMode() const;
  QStringList privacyList(quint16 classId) const; // 2 = allow, 3 = block
  bool setPrivacy(quint8 mode, const QStringList &allow, const QStringList &block);
  bool warnUser(const QString &name, bool anonymous);
  bool renameBuddy(quint16 groupId, quint16 itemId, const QString &name);
  bool moveBuddy(quint16 groupId, quint16 itemId, quint16 destinationGroupId);
  bool removeBuddy(quint16 groupId, quint16 itemId);
  bool rosterEditPending() const;
  bool setAway(const QString &text);
  bool clearAway();
  bool away() const;
signals:
  void statusChanged(const QString &status);
  void failed(const QString &reason);
  void rosterChanged();
  void buddyPresenceChanged(const QString &screenName, bool online); // only on an offline<->online transition
  void rosterReady();
  void loginStageChanged(int stage);
  void messageReceived(const QString &sender, const QString &text, bool autoResponse = false);
  void messageAccepted(const QString &recipient, quint64 cookie);
  void userInfoReceived(const aim::oscar::UserInfo &info);
  void operationFailed(const QString &operation, const QString &reason);
  void chatRoomReady(const aim::oscar::ChatRoom &room);
  void chatParticipantsChanged(const QString &cookie, const QVector<aim::oscar::UserInfo> &participants);
  void chatMessageReceived(const QString &cookie, const QString &sender, const QString &text);
  void chatInvitationReceived(const aim::oscar::ChatInvitation &invitation);
  void chatRoomClosed(const QString &cookie, const QString &reason);
  void rosterEditFinished(bool success);
  void awayChanged(bool away);
  void warnCompleted(const QString &screenName, quint16 delta, quint16 newLevel); // newLevel in tenths of a percent
private slots:
  void onConnected();
  void onReadyRead();
  void onSocketError(QAbstractSocket::SocketError error);
private:
  enum class Phase { Idle, AuthConnecting, AuthChallenge, AuthLogin, BosConnecting, BosHost, BosRoster, Online, Failed };
  void sendFrame(quint8 channel, const QByteArray &payload);
  void sendSnac(quint16 family, quint16 subgroup, const QByteArray &body = {});
  void handleFrame(const aim::oscar::FlapFrame &frame);
  void handleSnac(const aim::oscar::Snac &snac);
  void beginBos(const QString &host, quint16 port, const QByteArray &cookie);
  void beginRoster();
  void sendBuddyRequests();
  void setBuddyOnline(const QString &screenName, bool online);
  void fail(const QString &reason);
  struct Pending { QString operation; quint16 family = 0; aim::oscar::ChatRoom room; };
  struct Service { QTcpSocket *socket = nullptr; QString key; quint16 family = 0; quint16 sequence = 0; quint16 osVersion = 4; QByteArray buffer; QByteArray cookie; aim::oscar::ChatRoom room; QHash<QString,aim::oscar::UserInfo> participants; QHash<quint32,QString> requests; bool ready = false; bool announced = false; bool closing = false; };
  quint32 request(quint16 family, quint16 subgroup, const QByteArray &body, const QString &operation, const aim::oscar::ChatRoom &room = {});
  void requestService(quint16 family, const aim::oscar::ChatRoom &room = {});
  void openService(quint16 family, const aim::oscar::ChatRoom &room, const QString &host, quint16 port, const QByteArray &cookie);
  void readService(const QSharedPointer<Service> &service);
  void handleService(const QSharedPointer<Service> &service, const aim::oscar::Snac &snac);
  bool sendService(const QSharedPointer<Service> &service, quint16 family, quint16 subgroup, const QByteArray &body = {}, const QString &operation = {});
  void closeService(const QSharedPointer<Service> &service, const QString &reason);
  void closeServices();
  void flushChatCreates();
  bool handleMessaging(const aim::oscar::Snac &snac);
  struct RosterStep { quint16 subgroup; aim::oscar::FeedbagItem item; };
  bool handleRosterEdits(const aim::oscar::Snac &snac);
  void advanceRosterEdit();
  void finishRosterCluster(bool success);
  void cancelRosterEdit();
  void syncBuddySubscriptions(const QVector<aim::oscar::FeedbagItem> &before);
  bool rosterError(const QString &reason);
  QTcpSocket socket_;
  QByteArray receiveBuffer_;
  QString authHost_;
  quint16 authPort_ = 0;
  QString bosHost_;
  quint16 bosPort_ = 0;
  QByteArray cookie_;
  QString screenName_;
  QByteArray password_;
  quint16 sequence_ = 0;
  quint32 requestId_ = 1;
  Phase phase_ = Phase::Idle;
  bool switchingSocket_ = false;
  QVector<aim::oscar::FeedbagItem> roster_;
  QSet<QString> onlineBuddies_;
  struct Presence { aim::oscar::UserInfo info; qint64 received = 0; };
  QHash<QString, Presence> presence_;
  QHash<quint32,Pending> pending_;
  QHash<QString,QSharedPointer<Service>> services_;
  QVector<aim::oscar::ChatRoom> chatCreates_;
  quint16 maxMessageLength_ = 0;
  bool chatNavRequested_ = false;
  bool away_ = false;
  QString awayText_;
  bool rosterEditing_ = false;
  bool rosterRefreshing_ = false;
  bool rosterEditOk_ = false;
  bool incomingCluster_ = false;
  QVector<aim::oscar::FeedbagItem> incomingRoster_;
  QVector<RosterStep> rosterSteps_;
  qsizetype rosterStep_ = 0;
  quint32 rosterRequest_ = 0;
  quint32 rosterRefreshRequest_ = 0;
  QTimer rosterTimeout_;
};
