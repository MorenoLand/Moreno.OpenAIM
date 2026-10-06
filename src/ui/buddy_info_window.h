#pragma once
#include "window_base.h"
#include "../oscar/client.h"
#include <QAbstractNativeEventFilter>
#include <QElapsedTimer>
#include <QTextDocument>
#include <QTimer>
#include <functional>

// "Buddy Info" window of locateui.ocm (Research/buddy_info.md): hand-built layout, combo + OK/Close, info lines,
// profile pane and IM / Add Buddy / Directory Info art buttons.
class BuddyInfoWindow final : public WindowBase, public QAbstractNativeEventFilter {
public:
  using Action = std::function<void(int id, const QString &screenName)>; // 139 = send IM
  static void open(OscarClient *client, const QString &screenName, const Action &action);
  static void preview(OscarClient *client); // developer preview with a sample reply (--ui-preview=info)
  ~BuddyInfoWindow() override;
  bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
protected:
  bool event(QEvent *event) override;
  void paintContent(QPainter &painter) override;
  void contentMousePress(const QPoint &point, Qt::MouseButton button) override;
  void contentMouseRelease(const QPoint &point, Qt::MouseButton button) override;
  void contentMouseMove(const QPoint &point) override;
  void contentLeave() override;
  void closeRequested() override;
  void resizeEvent(QResizeEvent *event) override;
  void moveEvent(QMoveEvent *event) override;
private:
  enum class State { Idle, Waiting, Shown, Error };
  struct Button { int id; quint32 art[3]; QRect rect; };
  BuddyInfoWindow(OscarClient *client, const QString &screenName, const Action &action);
  void createControls();
  void layoutControls();
  void request(bool silent);
  void expandPanel();
  void cancel();
  void nameEdited();
  void showReply(const aim::oscar::UserInfo &info);
  void showError(const QString &reason);
  void setCloseMode(bool close);
  void updateTitle();
  QString currentName() const;
  QRect clientRect() const;
  int panelTop() const;
  QString onlineText() const;
  OscarClient *client_ = nullptr;
  Action action_;
  QString name_;
  State state_ = State::Idle;
  bool panel_ = false, closeMode_ = true, errorShown_ = false;
  aim::oscar::UserInfo info_;
  QElapsedTimer received_;
  QTextDocument profile_;
  QTimer refresh_;
  QList<Button> buttons_;
  int hovered_ = 0, pressed_ = 0;
  int buttonW_ = 0, buttonH_ = 0, labelW_ = 0, labelH_ = 0, avgCharW_ = 8, charH_ = 16;
  QSize initialOuter_;
  void *combo_ = nullptr, *ok_ = nullptr, *close_ = nullptr;
};
