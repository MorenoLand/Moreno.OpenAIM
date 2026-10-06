#pragma once
#include "../oscar/client.h"
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <functional>

class ChatRoomWindow;
class ChatInviteReceiveWindow;

// Group chat (ChatUI.ocm, Research/chat_windows.md): Chat Invitation (CTLGROUP 106), invitation received
// (CTLGROUP 105 / Decline 107) and Chat Room windows (CTLGROUP 104, RT_MENU 102), driven through OscarClient.
class ChatWindows final : public QObject {
public:
  using Action = std::function<void(int id, const QString &screenName)>; // 139 = IM, 138 = Get Info
  ChatWindows(OscarClient *client, Action action, QObject *parent = nullptr);
  ~ChatWindows() override;
  void invite(const QStringList &names);                         // Buddy List Chat button / IM window &Chat
  void inviteToRoom(ChatRoomWindow *room, const QStringList &names);
  bool startRoom(const QString &name, const QStringList &names, const QString &message); // false = too many rooms
  void accept(const aim::oscar::ChatInvitation &invitation);
  void roomClosed(ChatRoomWindow *room);
  OscarClient *client() const { return client_; }
  const Action &action() const { return action_; }
  ChatRoomWindow *findRoom(const QString &name) const;
  void preview();
private:
  OscarClient *client_;
  Action action_;
  QList<QPointer<ChatRoomWindow>> rooms_;
};
