#pragma once
#include <QObject>
#include <QSet>
#include <QTimer>

class OscarClient;

// Away / Idle behaviour from Preferences: auto responses to incoming IMs (Away Message "Auto respond", Idle Message
// "When Idle auto respond with"), and reporting idle time to the server (Privacy "Allow users to see how long I've
// been idle").
class AutoResponder final : public QObject {
public:
  explicit AutoResponder(OscarClient *client, QObject *parent = nullptr);
  bool idle() const { return idle_; }
private:
  void incoming(const QString &sender, bool autoResponse);
  void poll();
  OscarClient *client_;
  QTimer timer_;
  bool idle_ = false;
  QSet<QString> answered_; // senders already answered during the current away/idle period
  QString answeredText_;
};
