#pragma once
#include "window_base.h"
#include <QPointer>
#include <QSettings>

class ConnectionWindow;
class NativePreferences;

class PreferencesWindow final : public WindowBase {
  Q_OBJECT
public:
  explicit PreferencesWindow(QWindow *parent);
  void show();
  void showCategory(int id,int commandId=0);
  void requestActivate();
  bool close();
signals:
  void settingsChanged();
  void dismissed();
protected:
  void paintContent(QPainter &painter) override;
  void contentMousePress(const QPoint &point, Qt::MouseButton button) override;
  void contentKeyPress(QKeyEvent *event) override;
private:
  void save();
  void saveAndClose();
  void openConnection();
  QSettings settings_;
  NativePreferences *native_ = nullptr;
  QPointer<ConnectionWindow> connectionWindow_;
  bool savePassword_ = false;
  bool autoLogin_ = false;
  bool startWithWindows_ = false;
  bool lan_ = true;
  bool autoUpgrade_ = true;
  bool showToday_ = true;
  bool reconnect_ = true;
  bool showReconnectDialog_ = true;
  bool dirty_ = false;
};
