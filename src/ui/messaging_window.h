#pragma once
#include "../oscar/client.h"
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
class QWindow;
class MessagingWindows final : public QObject {
  Q_OBJECT
public:
  explicit MessagingWindows(OscarClient *client, QWindow *owner, QObject *parent = nullptr);
  ~MessagingWindows() override;
  void openMessage(const QString &recipient = QString());
  void previewConversation(); // developer preview (--ui-preview=im-conversation)
  void setChatHandler(std::function<void(const QString &)> handler); // IM window &Chat / People > Send Chat Invitation
private:
  struct State;
  std::unique_ptr<State> state_;
};
