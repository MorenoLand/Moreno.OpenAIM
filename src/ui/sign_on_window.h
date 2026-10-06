#pragma once
#include "window_base.h"
#include "../oscar/client.h"
#include <QImage>
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <QAbstractNativeEventFilter>
#include <memory>

class BuddyListWindow;
class PreferencesWindow;
struct CtlObject;

class SignOnWindow final : public WindowBase, public QAbstractNativeEventFilter {
  Q_OBJECT
public:
  explicit SignOnWindow(OscarClient *client);
  ~SignOnWindow() override;
  void showClient();
  void signOffFromTray();
  void exitFromTray();
  void showPreferences(int category=293,int commandId=0);
  bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
signals:
  void actionRequested(int id,const QString &screenName);
protected:
  bool event(QEvent *event) override;
  void paintContent(QPainter &painter) override;
  void contentMousePress(const QPoint &point, Qt::MouseButton button) override;
  void contentMouseMove(const QPoint &point) override;
  void contentMouseRelease(const QPoint &point, Qt::MouseButton button) override;
  void contentLeave() override;
  void contentKeyPress(QKeyEvent *event) override;
  void closeRequested() override;
private:
  void signOn();
  void reloadPreferences();
  void initializeNativeControls();
  void setLoginStage(int stage);
  void cancelSignOn();
  void drawField(QPainter &painter, const QRect &rect, const QString &text, bool password, bool active);
  void drawCheckbox(QPainter &painter, const QRect &rect, bool checked);
  // CTLGROUP 111 (sign-on form) / 119 (connecting state) laid out in the client area; objects in record order.
  QList<CtlObject *> formObjects(bool connecting);
  QRect formRect(quint32 id, bool connecting = false, int occurrence = 0);
  QSize formCanvas(bool connecting) const;
  int actionAt(const QPoint &point);
  std::shared_ptr<CtlObject> form_, connecting_;
  void *nativeFontEdit_ = nullptr;
  void *nativeFontCombo_ = nullptr;
  OscarClient *client_ = nullptr;
  QSettings settings_;
  QImage logo_;
  QImage screenNameLabel_;
  QImage helpIcon_;
  QImage setupIcon_;
  QImage signOnIcon_;
  QImage buttonStates_[3][3];
  QString screenName_;
  QString password_;
  QString host_;
  QString status_;
  quint16 port_ = 5190;
  bool savePassword_ = false;
  bool autoLogin_ = false;
  bool nameActive_ = true;
  bool caretVisible_ = true;
  QPointer<PreferencesWindow> preferencesWindow_;
  QPointer<BuddyListWindow> buddyWindow_;
  QTimer caretTimer_;
  void *nativeName_ = nullptr;
  void *nativePassword_ = nullptr;
  void *nativeSave_ = nullptr;
  void *nativeAuto_ = nullptr;
  void *nativeFont_ = nullptr;
  void *nativeCancel_ = nullptr;
  int loginStage_ = 0;
  int hoveredAction_ = 0;
  int pressedAction_ = 0;
};
