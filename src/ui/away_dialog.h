#pragma once
#include <QPair>
#include <QList>
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
  void useSaved(int index); // Away Message submenu entry (index into menuMessages())
  // CreateAwayMenu entries after "New Message...": the stored messages (STRING 281 default when none) and the
  // built-in STRING 910 "Playing Game" unless a stored label equals it. Pairs of label and text.
  static QList<QPair<QString, QString>> menuMessages();
private:
  void showCurrent(const QString &label, const QString &text);
  void closeCurrent();
  OscarClient *client_ = nullptr;
  QPointer<QWindow> owner_;
  void *current_ = nullptr; // HWND of dialog 147
  void *currentFont_ = nullptr;
};
