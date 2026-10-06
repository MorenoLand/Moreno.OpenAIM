#pragma once
#include <QObject>
#include <QAbstractNativeEventFilter>
#include <QList>
#include <QPointer>
#include <QString>
#include <QWindow>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif
class SystemTray final : public QObject, public QAbstractNativeEventFilter {
  Q_OBJECT
public:
  struct Action { int id; QString text; bool enabled = true; QList<Action> children; };
  explicit SystemTray(QWindow *callbackWindow, QObject *parent = nullptr);
  ~SystemTray() override;
  void setActions(const QList<Action> &actions);
  void setToolTip(const QString &text);
  void setOnline(bool online);
  void show();
  void hide();
  bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
signals:
  void activated();
  void triggered(int id);
private:
  QPointer<QWindow> callbackWindow_;
  QList<Action> actions_;
  bool visible_ = false;
  QString tooltip_;
  bool online_ = false;
#ifdef Q_OS_WIN
  static constexpr UINT callbackMessage_ = WM_APP + 0x5a1;
  NOTIFYICONDATAW icon_{};
  UINT taskbarCreated_ = 0;
  bool registered_ = false;
  bool version4_ = false;
  void registerIcon();
  void popup(int x, int y);
#endif
};
