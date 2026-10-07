#pragma once
#include "../oscar/client.h"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <functional>

class TalkWindow;
class TalkCall;

// Talk (Research/talk_protocol.md): ICBM channel-2 rendezvous with the voice capability, the start/receive dialogs
// 244/245, the accept policy of Preferences > Talk, and one TalkWindow (CTLGROUP 1) per call.
class TalkSessions final : public QObject {
public:
  using OpenIm = std::function<void(const QString &screenName)>;
  TalkSessions(OscarClient *client, OpenIm openIm, QObject *parent = nullptr);
  ~TalkSessions() override;
  void start(const QString &screenName);   // IM window Talk button, People > Connect to Talk, Buddy List
  static void preview();                   // developer preview of the Talk window layout
  struct Session;
private:
  void incoming(const aim::oscar::Rendezvous &rv);
  void send(Session *session, quint16 type, quint16 port, quint16 sequence);
  void cancel(Session *session, quint16 reason);
  void finish(Session *session);
  Session *find(const QString &name) const;
  void attachCall(Session *session);
  OscarClient *client_;
  OpenIm openIm_;
  QList<Session *> sessions_;
};
