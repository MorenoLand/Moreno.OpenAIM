#include "buddy_list_window.h"
#include "menu_bar.h"
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <functional>
#include <utility>
#include <algorithm>

namespace {
class ExitConfirmationWindow final : public WindowBase {
public:
  using Handler = std::function<void(bool, bool)>;
  ExitConfirmationWindow(QWindow *parent, Handler handler) : WindowBase(QStringLiteral("AOL Instant Messenger (SM)"), QSize(287, 138), parent), warning_(QStringLiteral(":/aim/warning.gif")), handler_(std::move(handler)) { setCaptionButtons(false, false, true); }
protected:
  void paintContent(QPainter &p) override {
    p.fillRect(QRect(1, TitleBarHeight, canvasWidth() - 2, canvasHeight() - TitleBarHeight - 1), QColor(240, 240, 240));
    if (!warning_.isNull()) p.drawImage(QRect(10, 34, 32, 32), warning_);
    QFont font(QStringLiteral("MS Sans Serif"), 8);
    drawText(p, QPoint(57, 57), QStringLiteral("Are you sure you want to end your session?"), QColor(20, 20, 20), font);
    p.setBrush(QColor(250, 250, 250)); p.setPen(QColor(0, 120, 215)); p.drawRect(QRect(93, 76, 48, 26)); p.setPen(QColor(130, 130, 130)); p.drawRect(QRect(149, 76, 48, 26));
    drawText(p, QPoint(111, 93), QStringLiteral("Yes"), QColor(20, 20, 20), font); drawText(p, QPoint(166, 93), QStringLiteral("No"), QColor(20, 20, 20), font);
    p.fillRect(QRect(8, 115, 13, 13), Qt::white); p.setPen(QColor(100, 100, 100)); p.drawRect(QRect(8, 115, 13, 13));
    if (suppress_) { p.setPen(QPen(QColor(0, 86, 180), 2)); p.drawLine(10, 122, 13, 125); p.drawLine(13, 125, 19, 117); }
    drawText(p, QPoint(25, 126), QStringLiteral("Do not ask me this again."), QColor(20, 20, 20), font);
  }
  void contentMousePress(const QPoint &point, Qt::MouseButton button) override {
    if (button != Qt::LeftButton) return;
    if (QRect(8, 115, 13, 13).contains(point)) { suppress_ = !suppress_; renderNow(); }
    else if (QRect(93, 76, 48, 26).contains(point)) finish(true);
    else if (QRect(149, 76, 48, 26).contains(point)) finish(false);
  }
  void contentKeyPress(QKeyEvent *event) override { if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_N) finish(false); else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Y) finish(true); }
  void closeRequested() override { finish(false); }
private:
  void finish(bool yes) { if (handler_) handler_(yes, suppress_); close(); }
  QImage warning_;
  Handler handler_;
  bool suppress_ = false;
};
inline QString groupName(const QVector<aim::oscar::FeedbagItem> &items, quint16 id) { for (const auto &item : items) if (item.classId == 1 && item.groupId == id) return item.name; return {}; }
// Reference geometry is the 142x443 canvas. Extra width/height stretches the tabs and list; the logo and banner stay centred and the action area stays on the bottom edge.
// The anchoring is inferred from the reference layout until the Buddy List CTLGROUP placement data is decoded.
constexpr QSize ReferenceCanvas(142, 443), MinimumCanvas(142, 350);
constexpr int MenuRowHeight = PaintedMenuBar::RowHeight, MenuTop = 28, MenuPadding = PaintedMenuBar::Padding;
QFont menuFont() { return PaintedMenuBar::font(); }
// The reference layout has a two-row (wrapped) menu bar ending at MenuTop + 2 rows; content follows the real bar bottom.
int menuBottom(const QVector<QRect> &menus) { return menus.isEmpty() ? MenuTop + 2 * MenuRowHeight : PaintedMenuBar::bottom(menus, MenuTop); }
struct Layout {
  int dx, dy;
  QRect logo, onlineTab, setupTab, list, banner, buttons[6];
  int rowLimit() const { return list.bottom() - 5; }
  explicit Layout(const QSize &canvas, int menuEnd = MenuTop + 2 * MenuRowHeight) : dx(std::max(0, canvas.width() - ReferenceCanvas.width())), dy(canvas.height() - ReferenceCanvas.height()) {
    const int top = menuEnd - (MenuTop + 2 * MenuRowHeight);
    logo = QRect(11 + dx / 2, 75 + top, 120, 60);
    onlineTab = QRect(3, 139 + top, 65 + dx / 2, 28); setupTab = QRect(onlineTab.right() + 1, 139 + top, 69 + dx - dx / 2, 28);
    list = QRect(4, 169 + top, 134 + dx, 139 + dy - top);
    banner = QRect(6 + dx / 2, 390 + dy, 130, 20);
    const QRect reference[6] = {QRect(4,311,23,32),QRect(31,311,27,32),QRect(61,315,24,24),QRect(4,352,47,32),QRect(53,357,24,24),QRect(80,357,24,24)};
    for (int i = 0; i < 6; ++i) buttons[i] = reference[i].translated(0, dy);
  }
};
inline QVector<quint16> memberOrder(const QVector<aim::oscar::FeedbagItem> &items,quint16 id) { for(const auto &item:items)if(item.classId==1&&item.groupId==id)for(const auto &attribute:item.attributes)if(attribute.tag==0xc8){QVector<quint16> result;for(qsizetype i=0;i+1<attribute.value.size();i+=2)result.append((quint16(quint8(attribute.value[i]))<<8)|quint8(attribute.value[i+1]));return result;}return {}; }
}
BuddyListWindow::BuddyListWindow(OscarClient *client) : WindowBase(QString(), QSize(142, 443)), client_(client), logo_(QStringLiteral(":/aim/buddy-header.gif")), messageIcon_(transparentBitmap(QStringLiteral(":/aim/buddy-message.bmp"))), talkIcon_(transparentBitmap(QStringLiteral(":/aim/buddy-talk.bmp"))), infoIcon_(transparentBitmap(QStringLiteral(":/aim/buddy-info.bmp"))), todayIcon_(transparentBitmap(QStringLiteral(":/aim/buddy-today.bmp"))), notesIcon_(transparentBitmap(QStringLiteral(":/aim/buddy-notes.bmp"))), setupIcon_(transparentBitmap(QStringLiteral(":/aim/setup-wrench.bmp"))), banner_(QStringLiteral(":/aim/today-banner.gif")) {
  connect(client_, &OscarClient::rosterChanged, this, [this] { renderNow(); });
  const int buttonIds[6][3]={{110,111,112},{137,193,192},{253,255,254},{261,264,267},{260,263,266},{259,262,265}};for(int button=0;button<6;++button)for(int state=0;state<3;++state)buttonStates_[button][state]=transparentBitmap(QStringLiteral(":/aim/button-%1.bmp").arg(buttonIds[button][state]));
  setResizable(MinimumCanvas);
  menuBar_ = loadMenuResource(103);
}
void BuddyListWindow::paintContent(QPainter &p) {
  p.fillRect(QRect(1, TitleBarHeight, canvasWidth() - 2, canvasHeight() - TitleBarHeight - 1), QColor(235, 235, 235));
  const QVector<QRect> menus = menuRects();
  for (int i = 0; i < menus.size(); ++i) {
    if (i == openMenu_ || i == hoveredMenu_) { p.fillRect(menus[i], QColor(229, 243, 255)); p.setPen(QColor(204, 232, 255)); p.drawRect(menus[i].adjusted(0, 0, -1, -1)); }
    p.setFont(menuFont()); p.setPen(QColor(20, 20, 20)); p.drawText(menus[i].adjusted(MenuPadding, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, menuBar_[i].label());
  }
  const Layout layout(QSize(canvasWidth(), canvasHeight()), menuBottom(menus)); const int rowLimit = layout.rowLimit(), rowWidth = layout.list.width() - 3;
  if (!logo_.isNull()) p.drawImage(layout.logo, logo_);
  p.fillRect(layout.onlineTab, listSetup_ ? QColor(220, 220, 220) : Qt::white); p.fillRect(layout.setupTab, listSetup_ ? Qt::white : QColor(220, 220, 220));
  p.setPen(QColor(130, 130, 130)); p.drawRect(layout.onlineTab); p.drawRect(layout.setupTab);
  drawText(p, QPoint(layout.onlineTab.left() + 7, layout.onlineTab.top() + 18), QStringLiteral("Online"), QColor(20, 20, 20), QFont(QStringLiteral("MS Sans Serif"), 8, listSetup_ ? QFont::Normal : QFont::Bold));
  drawText(p, QPoint(layout.setupTab.left() + 7, layout.setupTab.top() + 18), QStringLiteral("List Setup"), QColor(20, 20, 20), QFont(QStringLiteral("MS Sans Serif"), 8, listSetup_ ? QFont::Bold : QFont::Normal));
  const QRect listRect = layout.list; p.fillRect(listRect, Qt::white); p.setPen(QColor(130, 130, 130)); p.drawRect(listRect);
  const QVector<aim::oscar::FeedbagItem> items = client_ ? client_->roster() : QVector<aim::oscar::FeedbagItem>{};
  rows_.clear();
  QVector<quint16> groupIds;
  for (const auto &item : items) if (item.classId == 1 && item.groupId != 0 && !item.name.isEmpty() && !groupIds.contains(item.groupId)) groupIds.append(item.groupId);
  const auto groupOrder=memberOrder(items,0);if(!groupOrder.isEmpty())std::stable_sort(groupIds.begin(),groupIds.end(),[&](quint16 left,quint16 right){int a=groupOrder.indexOf(left),b=groupOrder.indexOf(right);return (a<0?groupOrder.size():a)<(b<0?groupOrder.size():b);});
  int y = listRect.top() + 12;
  const QFont small(QStringLiteral("MS Sans Serif"), 8);
  for (quint16 groupId : groupIds) {
    int total = 0, online = 0;
    for (const auto &item : items) if (item.classId == 0 && item.groupId == groupId) { ++total; if (client_ && client_->isOnline(item.name)) ++online; }
    if(y>=rowLimit)break;const bool collapsed=collapsedGroups_.contains(groupId);p.setPen(QColor(20, 20, 20)); p.setBrush(QColor(60, 60, 60)); const QPoint tri[] = {collapsed?QPoint(9,y-8):QPoint(8, y - 6),collapsed?QPoint(9,y):QPoint(15, y - 6),collapsed?QPoint(13,y-4):QPoint(11, y - 2)}; p.drawPolygon(tri, 3);
    const QString groupLabel=listSetup_?groupName(items,groupId):QStringLiteral("%1 (%2/%3)").arg(groupName(items, groupId)).arg(online).arg(total);QFont groupFont(QStringLiteral("MS Sans Serif"),8,QFont::Bold);if(selectedGroup_&&selectedGroupId_==groupId)p.fillRect(QRect(19,y-12,QFontMetrics(groupFont).horizontalAdvance(groupLabel),14),QColor(255,255,0));rows_.append({QRect(5,y-12,rowWidth,16),groupId,0,{},true});drawText(p,QPoint(19,y),groupLabel,QColor(20,20,20),groupFont);
    y += 17;
    if(collapsed)continue;QVector<aim::oscar::FeedbagItem> buddies;for(const auto &item:items)if(item.classId==0&&item.groupId==groupId&&(listSetup_||client_->isOnline(item.name)))buddies.append(item);const auto buddyOrder=memberOrder(items,groupId);if(!buddyOrder.isEmpty())std::stable_sort(buddies.begin(),buddies.end(),[&](const auto &left,const auto &right){int a=buddyOrder.indexOf(left.itemId),b=buddyOrder.indexOf(right.itemId);return (a<0?buddyOrder.size():a)<(b<0?buddyOrder.size():b);});
    for (const auto &item : buddies) if(y<rowLimit) {
      if(!selectedGroup_&&selectedGroupId_==item.groupId&&selectedItemId_==item.itemId)p.fillRect(QRect(23,y-12,QFontMetrics(small).horizontalAdvance(item.name),14),QColor(255,255,0));rows_.append({QRect(18,y-12,rowWidth-13,16),item.groupId,item.itemId,item.name,false});
      drawText(p, QPoint(23, y), item.name, QColor(30,30,30), small);
      y += 16;
    }
    if (y >= rowLimit) break;
  }
  if(!listSetup_&&y<rowLimit){int total=0,offline=0;for(const auto &item:items)if(item.classId==0){++total;if(!client_->isOnline(item.name))++offline;}const bool collapsed=collapsedGroups_.contains(0);p.setPen(QColor(20,20,20));p.setBrush(QColor(60,60,60));const QPoint triangle[]={collapsed?QPoint(9,y-8):QPoint(8,y-6),collapsed?QPoint(9,y):QPoint(15,y-6),collapsed?QPoint(13,y-4):QPoint(11,y-2)};p.drawPolygon(triangle,3);rows_.append({QRect(5,y-12,rowWidth,16),0,0,{},true});drawText(p,QPoint(19,y),QStringLiteral("Offline (%1/%2)").arg(offline).arg(total),QColor(20,20,20),small);y+=17;QFont offlineFont=small;offlineFont.setItalic(true);if(!collapsed)for(const auto &item:items)if(item.classId==0&&!client_->isOnline(item.name)&&y<rowLimit){if(!selectedGroup_&&selectedGroupId_==item.groupId&&selectedItemId_==item.itemId)p.fillRect(QRect(23,y-12,QFontMetrics(offlineFont).horizontalAdvance(item.name),14),QColor(255,255,0));rows_.append({QRect(18,y-12,rowWidth-13,16),item.groupId,item.itemId,item.name,false});drawText(p,QPoint(23,y),item.name,QColor(120,120,120),offlineFont);y+=16;}}
  const int dy = layout.dy;
  p.setPen(QColor(150, 150, 150)); p.drawLine(5, 310 + dy, layout.list.right() - 2, 310 + dy);
  for(int button=0;button<6;++button){int state=pressedAction_==button+1&&hoveredAction_==button+1?2:hoveredAction_==button+1?1:0;p.drawImage(layout.buttons[button].topLeft(),buttonStates_[button][state]);}
  if (!banner_.isNull()) p.drawImage(layout.banner, banner_);
  if (!messageIcon_.isNull()) p.drawImage(QRect(3, 414 + dy, 15, 15), messageIcon_);
  drawText(p, QPoint(19, 424 + dy), QStringLiteral("Don't receive data if your"), QColor(30, 30, 30), QFont(QStringLiteral("MS Sans Serif"), 6));
  drawText(p, QPoint(19, 436 + dy), QStringLiteral("Prices delayed at least 15 minutes"), QColor(30, 30, 30), QFont(QStringLiteral("MS Sans Serif"), 6));
}
void BuddyListWindow::contentMousePress(const QPoint &point, Qt::MouseButton button) {
  if (button != Qt::LeftButton) return;
  {const QVector<QRect> menus=menuRects();for(int i=0;i<menus.size();++i)if(menus[i].contains(point)){openMenu(i);return;}}
  contentMouseMove(point);if(hoveredAction_){pressedAction_=hoveredAction_;renderNow();return;}
  for(const auto &row:rows_)if(row.rect.contains(point)){selectedGroupId_=row.groupId;selectedItemId_=row.itemId;selectedGroup_=row.group;selectedName_=row.name;if(row.group&&point.x()<18){if(collapsedGroups_.contains(row.groupId))collapsedGroups_.remove(row.groupId);else collapsedGroups_.insert(row.groupId);}renderNow();return;}
  const Layout layout(QSize(canvasWidth(), canvasHeight()), menuBottom(menuRects()));
  if (layout.setupTab.contains(point)) listSetup_ = true;
  else if (layout.onlineTab.contains(point)) listSetup_ = false;
  else return;
  renderNow();
}
void BuddyListWindow::contentMouseMove(const QPoint &point) { const QVector<QRect> menus=menuRects();int menu=-1;for(int i=0;i<menus.size();++i)if(menus[i].contains(point))menu=i;if(menu!=hoveredMenu_){hoveredMenu_=menu;renderNow();}const Layout layout(QSize(canvasWidth(),canvasHeight()),menuBottom(menus));int action=0;for(int i=0;i<6;++i)if(layout.buttons[i].contains(point)){action=i+1;break;}if(action!=hoveredAction_){hoveredAction_=action;renderNow();} }
void BuddyListWindow::contentLeave() { if(hoveredAction_||hoveredMenu_>=0){hoveredAction_=0;hoveredMenu_=-1;renderNow();} }
void BuddyListWindow::contentMouseDoubleClick(const QPoint &point,Qt::MouseButton button) { if(button!=Qt::LeftButton)return;for(const auto &row:rows_)if(!row.group&&row.rect.contains(point)){emit actionRequested(139,row.name);return;} }
void BuddyListWindow::contentKeyPress(QKeyEvent *event) { if((event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter)&&!selectedName_.isEmpty()){emit actionRequested(139,selectedName_);event->accept();return;}if(event->modifiers().testFlag(Qt::AltModifier)&&event->key()==Qt::Key_I){emit actionRequested(139,selectedName_);event->accept();return;}WindowBase::contentKeyPress(event); }
void BuddyListWindow::contentMouseRelease(const QPoint &point,Qt::MouseButton button) { if(button!=Qt::LeftButton||!pressedAction_)return;int action=pressedAction_;pressedAction_=0;contentMouseMove(point);renderNow();if(action!=hoveredAction_)return;const int ids[]={139,529,138,174,24000,20002};emit actionRequested(ids[action-1],selectedName_); }
void BuddyListWindow::closeRequested() { hide(); }
void BuddyListWindow::requestExit() {
#ifdef Q_OS_WIN
  bool suppress=false;if(settings_.value(QStringLiteral("preferences/confirmExit"),true).toBool()&&!confirmExit(suppress))return;if(suppress)settings_.setValue(QStringLiteral("preferences/confirmExit"),false);if(client_)client_->signOff();emit exitAccepted();return;
#endif
  if (confirmation_) { confirmation_->show(); confirmation_->requestActivate(); return; }
  if (!settings_.value(QStringLiteral("preferences/confirmExit"), true).toBool()) { if (client_) client_->signOff(); emit exitAccepted(); return; }
  auto *dialog = new ExitConfirmationWindow(this, [this](bool yes, bool suppress) {
    if (suppress) settings_.setValue(QStringLiteral("preferences/confirmExit"), false);
    confirmation_.clear();
    if (yes) { if (client_) client_->signOff(); emit exitAccepted(); }
  });
  confirmation_ = dialog;
  connect(dialog, &QObject::destroyed, this, [this] { confirmation_.clear(); });
  dialog->show();
}
QVector<QRect> BuddyListWindow::menuRects() const { return PaintedMenuBar::layout(menuBar_, 1, MenuTop, canvasWidth() - 2); } // RT_MENU 103 laid out like a native bar
QList<MenuItem> BuddyListWindow::preparedMenu(const MenuItem &top) const {
  // Placeholder entries are filled at run time by their owning modules; mirror what the original tray menu shows.
  std::function<QList<MenuItem>(const QList<MenuItem> &)> prepare = [&](const QList<MenuItem> &items) {
    QList<MenuItem> result;
    for (MenuItem item : items) {
      if (item.id == 660) { const QVariantList saved = settings_.value(QStringLiteral("nativePreferences/274/417/items")).toList(); for (int i = 0; i < saved.size(); ++i) { MenuItem away; away.id = AwaySavedBase + i; away.text = saved[i].toMap().value(QStringLiteral("label")).toString(); away.text.replace(QLatin1Char('&'), QStringLiteral("&&")); result.append(away); } if (!saved.isEmpty()) result.append(MenuItem{}); result.append(MenuItem{QStringLiteral("New Message..."), 24000}); continue; }
      if (item.id == 1001) { result.append(MenuItem{QStringLiteral("Add New POP3 Mailbox"), 1003}); continue; }
      if (item.id == 997) continue;
      item.text.replace(QStringLiteral("%s"), selectedName_.isEmpty() || selectedGroup_ ? QStringLiteral("Buddy") : selectedName_);
      item.children = prepare(item.children); if (!item.isSeparator() && item.id == 0 && item.children.isEmpty()) item.grayed = true;
      result.append(item);
    }
    return result;
  };
  return prepare(top.children);
}
void BuddyListWindow::openMenu(int index) {
  const QVector<QRect> menus = menuRects(); if (index < 0 || index >= menus.size()) return;
  openMenu_ = index; renderNow();
  const int command = popupMenu(this, preparedMenu(menuBar_[index]), canvasToGlobal(menus[index].bottomLeft() + QPoint(0, 1)));
  openMenu_ = -1; hoveredMenu_ = -1; renderNow();
  if (command) emit actionRequested(command, selectedGroup_ ? QString() : selectedName_);
}
