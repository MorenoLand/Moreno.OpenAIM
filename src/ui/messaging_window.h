#pragma once
#include "../oscar/client.h"
#include <QObject>
#include <QString>
#include <memory>
class QWindow;
class MessagingWindows final : public QObject {
  Q_OBJECT
public:
  explicit MessagingWindows(OscarClient *client, QWindow *owner, QObject *parent = nullptr);
  ~MessagingWindows() override;
  void openMessage(const QString &recipient = QString());
private:
  struct State;
  std::unique_ptr<State> state_;
};
