#pragma once
#include "window_base.h"
#include "menu_template.h"
#include "../oscar/client.h"
#include <QImage>
#include <QPointer>
#include <QSettings>
#include <QSet>
#include <QTimer>
#include <memory>

struct CtlObject;

// Buddy List window of buddyui.ocm: CTLGROUP 101 laid out by the ported engine, RT_MENU 103 bar, Online / List Setup
// tab pages, Today/Away/Preferences box, Today banner and stock ticker.
class BuddyListWindow final : public WindowBase {
  Q_OBJECT
public:
  explicit BuddyListWindow(OscarClient *client);
  ~BuddyListWindow() override;
  static constexpr int AwaySavedBase = 24100; // + index of a saved away message (Away Message submenu)
  void requestExit();
signals:
  void exitAccepted();
  void actionRequested(int id, const QString &screenName);
protected:
  void paintContent(QPainter &painter) override;
  void contentMousePress(const QPoint &point, Qt::MouseButton button) override;
  void contentMouseRelease(const QPoint &point, Qt::MouseButton button) override;
  void contentMouseMove(const QPoint &point) override;
  void contentLeave() override;
  void contentMouseDoubleClick(const QPoint &point, Qt::MouseButton button) override;
  void contentKeyPress(QKeyEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void closeRequested() override;
private:
  struct Row { QRect rect; quint16 groupId; quint16 itemId; QString name; bool group; };
  QRect client() const;
  void layout();
  QVector<QRect> menuRects() const;
  void openMenu(int index);
  QList<MenuItem> preparedMenu(const MenuItem &top) const;
  void paintObject(QPainter &painter, CtlObject &object);
  void paintTabs(QPainter &painter, CtlObject &tabs);
  void paintTree(QPainter &painter, const QRect &area);
  QVector<QRect> tabRects(const CtlObject &tabs) const;
  QRect tabWindow(const CtlObject &tabs) const;
  CtlObject *buttonAt(const QPoint &point) const;
  void command(int id);
  void showAwayMenu(const QRect &anchor);
  void showClosedNotice();
  OscarClient *client_ = nullptr;
  QSettings settings_;
  std::shared_ptr<CtlObject> group_;
  QImage logo_, banner_;
  QPointer<QWindow> confirmation_;
  bool listSetup_ = false;
  quint32 hovered_ = 0, pressed_ = 0;
  QString selectedName_;
  QVector<Row> rows_;
  QSet<quint16> collapsedGroups_;
  quint16 selectedGroupId_ = 0, selectedItemId_ = 0;
  bool selectedGroup_ = false;
  int treeScroll_ = 0;
  QRect treeArea_;
  QList<MenuItem> menuBar_;
  QVector<QRect> menuRects_;
  int hoveredMenu_ = -1, openMenu_ = -1;
  QTimer tickerTimer_;
  int tickerOffset_ = 0;
};
