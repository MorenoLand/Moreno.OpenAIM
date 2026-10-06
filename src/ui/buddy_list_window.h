#pragma once
#include "window_base.h"
#include "menu_template.h"
#include "../oscar/client.h"
#include <QImage>
#include <QPointer>
#include <QSettings>
#include <QSet>

class BuddyListWindow final : public WindowBase {
  Q_OBJECT
public:
  explicit BuddyListWindow(OscarClient *client);
  static constexpr int AwaySavedBase = 24100; // + index of a saved away message (Away Message submenu)
  void requestExit();
signals:
  void exitAccepted();
  void actionRequested(int id,const QString &screenName);
protected:
  void paintContent(QPainter &painter) override;
  void contentMousePress(const QPoint &point, Qt::MouseButton button) override;
  void contentMouseRelease(const QPoint &point, Qt::MouseButton button) override;
  void contentMouseMove(const QPoint &point) override;
  void contentLeave() override;
  void contentMouseDoubleClick(const QPoint &point, Qt::MouseButton button) override;
  void contentKeyPress(QKeyEvent *event) override;
  void closeRequested() override;
private:
  OscarClient *client_ = nullptr;
  QSettings settings_;
  QImage logo_;
  QImage messageIcon_;
  QImage talkIcon_;
  QImage infoIcon_;
  QImage todayIcon_;
  QImage notesIcon_;
  QImage setupIcon_;
  QImage banner_;
  QPointer<QWindow> confirmation_;
  bool listSetup_ = false;
  QImage buttonStates_[6][3];
  int hoveredAction_ = 0;
  int pressedAction_ = 0;
  QString selectedName_;
  struct Row { QRect rect;quint16 groupId;quint16 itemId;QString name;bool group; };
  QVector<Row> rows_;
  QSet<quint16> collapsedGroups_;
  quint16 selectedGroupId_ = 0;
  quint16 selectedItemId_ = 0;
  bool selectedGroup_ = false;
  QList<MenuItem> menuBar_;
  int hoveredMenu_ = -1;
  int openMenu_ = -1;
  QVector<QRect> menuRects() const;
  void openMenu(int index);
  QList<MenuItem> preparedMenu(const MenuItem &top) const;
};
