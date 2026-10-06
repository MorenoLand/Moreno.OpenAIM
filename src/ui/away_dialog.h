#pragma once
#include <QObject>
#include <QPointer>

class OscarClient;
class QWindow;

// Standalone away-message flow: RT_DIALOG 148 "Edit Away Message" (locateui.ocm 0x1148215b, opened without the
// Preferences host) and RT_DIALOG 147 "Current Away Message" while away. Saved messages are shared with the
// Preferences "Away Message" page (QSettings nativePreferences/274/417/items).
class AwayMessages final : public QObject {
public:
  AwayMessages(OscarClient *client, QWindow *owner, QObject *parent = nullptr);
  ~AwayMessages() override;
  void newMessage();
  void useSaved(int index); // Away Message submenu entry for a saved message
private:
  void showCurrent(const QString &label, const QString &text);
  void closeCurrent();
  OscarClient *client_ = nullptr;
  QPointer<QWindow> owner_;
  void *current_ = nullptr; // HWND of dialog 147
  void *currentFont_ = nullptr;
};
