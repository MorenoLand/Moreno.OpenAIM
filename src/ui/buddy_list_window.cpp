#include "buddy_list_window.h"
#include "art.h"
#include "ctl_group.h"
#include "gdi_text.h"
#include "native_dialog.h"
#include <QGuiApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWheelEvent>
#include <algorithm>
#include <functional>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
enum : quint32 { Logo = 134, Tabs = 135, OnlinePage = 136, OnlineBody = 137, SetupPage = 142, SetupBody = 151, WebSearch = 1487, Banner = 148, Ticker = 907, TickerCaption = 908,
                 SendIm = 139, Chat = 561, Info = 138, Today = 1140, AwayMessages = 1488, Preferences = 174, AddBuddy = 143, AddGroup = 145, Delete = 146, Find = 141 };
constexpr int MenuRowHeight = 19, MenuPadding = 7, RowHeight = 16;
const GdiFont MenuFont{QStringLiteral("MS Sans Serif"), -13};      // matches the reference screenshot's menu bar
const GdiFont TreeFont{QStringLiteral("Arial"), -12};              // Buddy List tree and tab captions (reference screenshot)
const QColor Selection(255, 255, 0), OfflineText(128, 128, 128);
QString aimString(quint32 id) { return aimEnvironment().string(id); }

class ExitConfirmationWindow final : public WindowBase {
public:
  using Handler = std::function<void(bool, bool)>;
  ExitConfirmationWindow(QWindow *parent, Handler handler) : WindowBase(QStringLiteral("AOL Instant Messenger (SM)"), QSize(287, 138), parent), warning_(QStringLiteral(":/aim/warning.gif")), handler_(std::move(handler)) { setCaptionButtons(false, false, true); }
protected:
  void paintContent(QPainter &p) override {
    p.fillRect(QRect(1, TitleBarHeight, canvasWidth() - 2, canvasHeight() - TitleBarHeight - 1), art::Face);
    if (!warning_.isNull()) p.drawImage(QRect(10, 34, 32, 32), warning_);
    drawGdiText(p, QRect(57, 47, 225, 14), QStringLiteral("Are you sure you want to end your session?"), GdiFont::dialog(), Qt::black, art::Face);
    p.setBrush(QColor(250, 250, 250)); p.setPen(QColor(0, 120, 215)); p.drawRect(QRect(93, 76, 48, 26)); p.setPen(QColor(130, 130, 130)); p.drawRect(QRect(149, 76, 48, 26));
    drawGdiText(p, QRect(94, 77, 47, 25), QStringLiteral("Yes"), GdiFont::dialog(), Qt::black, QColor(250, 250, 250), GdiSingleLine | GdiCenter | GdiVCenter);
    drawGdiText(p, QRect(150, 77, 47, 25), QStringLiteral("No"), GdiFont::dialog(), Qt::black, QColor(250, 250, 250), GdiSingleLine | GdiCenter | GdiVCenter);
    p.fillRect(QRect(8, 115, 13, 13), Qt::white); p.setPen(QColor(100, 100, 100)); p.drawRect(QRect(8, 115, 13, 13));
    if (suppress_) { p.setPen(QPen(QColor(0, 86, 180), 2)); p.drawLine(10, 122, 13, 125); p.drawLine(13, 125, 19, 117); }
    drawGdiText(p, QRect(25, 115, 200, 14), QStringLiteral("Do not ask me this again."), GdiFont::dialog(), Qt::black, art::Face);
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
QString groupName(const QVector<aim::oscar::FeedbagItem> &items, quint16 id) { for (const auto &item : items) if (item.classId == 1 && item.groupId == id) return item.name; return {}; }
QVector<quint16> memberOrder(const QVector<aim::oscar::FeedbagItem> &items, quint16 id) {
  for (const auto &item : items) if (item.classId == 1 && item.groupId == id) for (const auto &attribute : item.attributes) if (attribute.tag == 0xc8) { QVector<quint16> result; for (qsizetype i = 0; i + 1 < attribute.value.size(); i += 2) result.append((quint16(quint8(attribute.value[i])) << 8) | quint8(attribute.value[i + 1])); return result; }
  return {};
}
// Outer window size the original computes: min track = CtlGroupGetIdealSize + 2*SM_CXFRAME / + 2*SM_CYFRAME + SM_CYMENU + SM_CYCAPTION
// (buddyui 0x112859f2); the default window is min-track width x 450 at the top right of the screen (0x11284568).
QSize canvasForOuter(const QSize &outer) {
#ifdef Q_OS_WIN
  RECT frame{0, 0, 0, 0}; AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
  return QSize(outer.width() - (frame.right - frame.left) + 2, outer.height() - (frame.bottom - frame.top) + 24 + 3);
#else
  return outer;
#endif
}
QSize minimumOuter(const QSize &ideal) {
#ifdef Q_OS_WIN
  return QSize(ideal.width() + 2 * GetSystemMetrics(SM_CXFRAME), ideal.height() + 2 * GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CYMENU) + GetSystemMetrics(SM_CYCAPTION));
#else
  return ideal + QSize(8, 46);
#endif
}
}

BuddyListWindow::BuddyListWindow(OscarClient *client) : WindowBase(aimString(167), QSize(142, 443)), client_(client), group_(loadCtlGroup(101)), logo_(QStringLiteral(":/aim/buddy-header.gif")), banner_(QStringLiteral(":/aim/today-banner.gif")) {
  setIcon(QIcon(QStringLiteral(":/aim/icons/101")));
  connect(client_, &OscarClient::rosterChanged, this, [this] { requestUpdate(); });
  menuBar_ = loadMenuResource(103);
  QSize outer(142, 450);
  if (group_) {
    ctlShowControl(*group_, WebSearch, false); // VALUERES 138 = 1 hides the web search group (0x112844e1)
    if (CtlObject *tabs = group_->find(Tabs)) ctlSetPage(*tabs, OnlinePage);
    const QSize minimum = minimumOuter(ctlIdealSize(*group_, aimEnvironment()));
    setResizable(canvasForOuter(minimum));
    outer = QSize(minimum.width(), std::max(450, minimum.height()));
  } else setResizable(QSize(142, 350));
  setCanvasSize(canvasForOuter(outer));
  if (QScreen *screen = QGuiApplication::primaryScreen()) setFramePosition(QPoint(screen->geometry().right() + 1 - outer.width(), 0));
  connect(&tickerTimer_, &QTimer::timeout, this, [this] { ++tickerOffset_; requestUpdate(); });
  tickerTimer_.start(50);
}
BuddyListWindow::~BuddyListWindow() = default;

QRect BuddyListWindow::client() const {
#ifdef Q_OS_WIN
  return QRect(1, TitleBarHeight, width(), height());
#else
  return QRect(0, TitleBarHeight, width(), height() - TitleBarHeight);
#endif
}
QVector<QRect> BuddyListWindow::menuRects() const {
  // RT_MENU 103 items flow and wrap like a menu bar; metrics measured from the reference screenshot.
  QVector<QRect> rects; const QRect c = client(); int x = c.left(), y = c.top();
  for (const MenuItem &item : menuBar_) {
    const int w = gdiTextSize(MenuFont, item.text.section(QLatin1Char('\t'), 0, 0)).width() + MenuPadding * 2;
    if (x > c.left() && x + w > c.left() + c.width()) { x = c.left(); y += MenuRowHeight; }
    rects.append(QRect(x, y, w, MenuRowHeight)); x += w;
  }
  return rects;
}
void BuddyListWindow::layout() {
  menuRects_ = menuRects();
  const QRect c = client(); int top = c.top(); for (const QRect &r : menuRects_) top = std::max(top, r.bottom() + 2);
  if (group_) ctlMove(*group_, QRect(QPoint(c.left(), top), QPoint(c.right(), c.bottom())), aimEnvironment());
}

void BuddyListWindow::paintContent(QPainter &p) {
  layout();
  const QRect c = client(); p.fillRect(c, art::Face);
  // Menu bar: white band, black text with underlined mnemonics.
  int menuBottom = c.top(); for (const QRect &r : menuRects_) menuBottom = std::max(menuBottom, r.bottom() + 1);
  p.fillRect(QRect(c.left(), c.top(), c.width(), menuBottom - c.top() + 1), Qt::white);
  for (int i = 0; i < menuRects_.size(); ++i) {
    const bool hot = i == openMenu_ || i == hoveredMenu_; const QColor background = hot ? QColor(229, 243, 255) : QColor(Qt::white);
    if (hot) { p.fillRect(menuRects_[i], background); p.setPen(QColor(204, 232, 255)); p.drawRect(menuRects_[i].adjusted(0, 0, -1, -1)); }
    drawGdiText(p, menuRects_[i].adjusted(MenuPadding, 1, 0, -1), menuBar_[i].text.section(QLatin1Char('\t'), 0, 0), MenuFont, Qt::black, background, GdiSingleLine | GdiVCenter);
  }
  if (group_) paintObject(p, *group_);
}

void BuddyListWindow::paintObject(QPainter &p, CtlObject &o) {
  if (!o.shown()) return;
  const QRect r = o.windowRect();
  switch (o.kind) {
  case CtlObject::Kind::Group:
    if (o.flags & CtlObject::Padding) art::drawEtched(p, r);
    for (const auto &child : o.children) paintObject(p, *child);
    return;
  case CtlObject::Kind::TabGroup:
    paintTabs(p, o);
    for (const auto &child : o.children) paintObject(p, *child);
    return;
  case CtlObject::Kind::Ate:
    if (o.id == Logo && !logo_.isNull()) p.drawImage(QPoint(r.center().x() - logo_.width() / 2 + 1, r.top() + (r.height() - logo_.height()) / 2), logo_);
    else if (o.id == Banner && !banner_.isNull()) p.drawImage(QPoint(r.center().x() - banner_.width() / 2 + 1, r.top() + (r.height() - banner_.height()) / 2), banner_);
    else if (o.id == Ticker) {
      // Stock ticker with no data source scrolls STRING 22.
      const QString text = aimString(22) + QStringLiteral("        "); const int width = std::max(1, gdiTextSize(GdiFont::dialog(), text, GdiSingleLine | GdiNoPrefix).width());
      const int offset = tickerOffset_ % width;
      p.save(); p.setClipRect(r);
      for (int x = r.left() - offset; x < r.right(); x += width) drawGdiText(p, QRect(x, r.top(), width, r.height()), text, GdiFont::dialog(), Qt::black, art::Face, GdiSingleLine | GdiVCenter | GdiNoPrefix);
      p.restore();
    }
    return;
  case CtlObject::Kind::ArtButton: {
    if (!o.art[0]) return;
    const int state = (pressed_ == o.id && hovered_ == o.id) ? 2 : hovered_ == o.id ? 1 : 0;
    p.drawImage(r.topLeft() + QPoint(1, 1), art::image(o.art[state] ? o.art[state] : o.art[0]));
    return;
  }
  case CtlObject::Kind::Separator: art::drawEtchedLine(p, r); return;
  case CtlObject::Kind::Static: {
    const GdiFont font = GdiFont::fromFontDesc(o.fontId);
    p.save(); p.setClipRect(r); drawGdiText(p, QRect(r.topLeft(), QSize(std::max(r.width(), gdiTextSize(font, aimString(o.textId), GdiSingleLine | GdiNoPrefix).width()), r.height())), aimString(o.textId), font, Qt::black, art::Face, GdiSingleLine | GdiNoPrefix); p.restore();
    return;
  }
  case CtlObject::Kind::TabBody:
    treeArea_ = r.adjusted(2, 2, -2, -2);
    p.fillRect(r, Qt::white); art::drawSunken(p, r);
    paintTree(p, treeArea_);
    return;
  default: return;
  }
}

QVector<QRect> BuddyListWindow::tabRects(const CtlObject &tabs) const {
  // _Oscar_TabGroup window origin is (pos + margins - (5, 27)) (0x122055d9); captions come from the page group titles.
  QVector<QRect> rects; int x = tabs.pos.x() + tabs.margins.left() - 5 + 2; const int y = tabs.pos.y() + tabs.margins.top() - 27 + 2;
  for (const auto &page : tabs.children) { const int w = gdiTextSize(TreeFont.bold(), aimString(page->titleId)).width() + 14; rects.append(QRect(x, y, w, 20)); x += w; }
  return rects;
}
void BuddyListWindow::paintTabs(QPainter &p, CtlObject &tabs) {
  const QRect window(tabs.pos.x() + tabs.margins.left() - 5, tabs.pos.y() + tabs.margins.top() - 27, tabs.size.width() - tabs.margins.left() - tabs.margins.right() + 5, tabs.size.height() - tabs.margins.top() - tabs.margins.bottom() + 31);
  const QRect page(window.left(), window.top() + 22, window.width(), window.height() - 22);
  // Page frame (raised): highlight left/top, shadows right/bottom.
  p.setPen(art::Highlight); p.drawLine(page.left(), page.bottom(), page.left(), page.top()); p.drawLine(page.left(), page.top(), page.right(), page.top());
  p.setPen(art::DarkShadow); p.drawLine(page.right(), page.top(), page.right(), page.bottom()); p.drawLine(page.right(), page.bottom(), page.left(), page.bottom());
  p.setPen(art::Shadow); p.drawLine(page.right() - 1, page.top() + 1, page.right() - 1, page.bottom() - 1); p.drawLine(page.right() - 1, page.bottom() - 1, page.left() + 1, page.bottom() - 1);
  const QVector<QRect> rects = tabRects(tabs);
  for (int i = 0; i < rects.size(); ++i) {
    const bool selected = !tabs.children[i]->hidden(); QRect t = rects[i];
    if (selected) t.adjust(-2, -2, 2, 2);
    p.fillRect(t.adjusted(1, 1, -1, 0), art::Face);
    p.setPen(art::Highlight); p.drawLine(t.left(), t.bottom(), t.left(), t.top() + 2); p.drawLine(t.left() + 1, t.top() + 1, t.left() + 1, t.top() + 1); p.drawLine(t.left() + 2, t.top(), t.right() - 2, t.top());
    p.setPen(art::DarkShadow); p.drawLine(t.right(), t.top() + 2, t.right(), t.bottom()); p.drawPoint(t.right() - 1, t.top() + 1);
    p.setPen(art::Shadow); p.drawLine(t.right() - 1, t.top() + 2, t.right() - 1, t.bottom());
    if (!selected) { p.setPen(art::Highlight); p.drawLine(t.left(), t.bottom() + 1, t.right(), t.bottom() + 1); }
    drawGdiText(p, t.adjusted(2, 3, -2, -1), aimString(tabs.children[i]->titleId), selected ? TreeFont.bold() : TreeFont, Qt::black, art::Face, GdiSingleLine | GdiCenter | GdiVCenter | GdiNoPrefix);
  }
}

void BuddyListWindow::paintTree(QPainter &p, const QRect &area) {
  const QVector<aim::oscar::FeedbagItem> items = client_ ? client_->roster() : QVector<aim::oscar::FeedbagItem>{};
  rows_.clear();
  QVector<quint16> groupIds;
  for (const auto &item : items) if (item.classId == 1 && item.groupId != 0 && !item.name.isEmpty() && !groupIds.contains(item.groupId)) groupIds.append(item.groupId);
  const auto groupOrder = memberOrder(items, 0);
  if (!groupOrder.isEmpty()) std::stable_sort(groupIds.begin(), groupIds.end(), [&](quint16 a, quint16 b) { const int x = groupOrder.indexOf(a), y = groupOrder.indexOf(b); return (x < 0 ? groupOrder.size() : x) < (y < 0 ? groupOrder.size() : y); });
  p.save(); p.setClipRect(area);
  int y = area.top() + 1 - treeScroll_;
  const bool active = isActive();
  auto text = [&](int x, const QString &value, const GdiFont &font, const QColor &color, bool selected) {
    const QSize size = gdiTextSize(font, value, GdiSingleLine | GdiNoPrefix);
    const QRect rect(x, y, size.width() + 2, RowHeight);
    drawGdiText(p, rect, value, font, color, selected ? Selection : QColor(Qt::white), GdiSingleLine | GdiVCenter | GdiNoPrefix | GdiCenter);
    if (selected && active) { QPen pen(Qt::black); pen.setStyle(Qt::DotLine); p.setPen(pen); p.setBrush(Qt::NoBrush); p.drawRect(rect.adjusted(0, 0, -1, -1)); }
  };
  auto triangle = [&](int x, bool collapsed) {
    p.setPen(Qt::NoPen); p.setBrush(Qt::black); const int cy = y + RowHeight / 2;
    if (collapsed) { const QPoint t[] = {QPoint(x + 2, cy - 4), QPoint(x + 6, cy), QPoint(x + 2, cy + 4)}; p.drawPolygon(t, 3); }
    else { const QPoint t[] = {QPoint(x, cy - 2), QPoint(x + 8, cy - 2), QPoint(x + 4, cy + 2)}; p.drawPolygon(t, 3); }
  };
  const int left = area.left();
  auto buddiesOf = [&](quint16 groupId, bool onlineOnly) {
    QVector<aim::oscar::FeedbagItem> buddies; for (const auto &item : items) if (item.classId == 0 && item.groupId == groupId && (!onlineOnly || (client_ && client_->isOnline(item.name)))) buddies.append(item);
    const auto order = memberOrder(items, groupId);
    if (!order.isEmpty()) std::stable_sort(buddies.begin(), buddies.end(), [&](const auto &a, const auto &b) { const int x = order.indexOf(a.itemId), z = order.indexOf(b.itemId); return (x < 0 ? order.size() : x) < (z < 0 ? order.size() : z); });
    return buddies;
  };
  if (!listSetup_) {
    for (quint16 groupId : groupIds) {
      int total = 0, online = 0; for (const auto &item : items) if (item.classId == 0 && item.groupId == groupId) { ++total; if (client_ && client_->isOnline(item.name)) ++online; }
      const bool collapsed = collapsedGroups_.contains(groupId);
      triangle(left + 3, collapsed);
      text(left + 13, QStringLiteral("%1 (%2/%3)").arg(groupName(items, groupId)).arg(online).arg(total), TreeFont.bold(), Qt::black, selectedGroup_ && selectedGroupId_ == groupId);
      rows_.append({QRect(left, y, area.width(), RowHeight), groupId, 0, {}, true}); y += RowHeight;
      if (collapsed) continue;
      for (const auto &item : buddiesOf(groupId, true)) { text(left + 21, item.name, TreeFont, Qt::black, !selectedGroup_ && selectedGroupId_ == item.groupId && selectedItemId_ == item.itemId); rows_.append({QRect(left, y, area.width(), RowHeight), item.groupId, item.itemId, item.name, false}); y += RowHeight; }
    }
    int total = 0, offline = 0; for (const auto &item : items) if (item.classId == 0) { ++total; if (!client_ || !client_->isOnline(item.name)) ++offline; }
    const bool collapsed = collapsedGroups_.contains(0);
    triangle(left + 3, collapsed);
    text(left + 13, QStringLiteral("Offline (%1/%2)").arg(offline).arg(total), TreeFont.bold(), OfflineText, selectedGroup_ && selectedGroupId_ == 0);
    rows_.append({QRect(left, y, area.width(), RowHeight), 0, 0, {}, true}); y += RowHeight;
    if (!collapsed) { GdiFont italic = TreeFont; italic.italic = true; for (const auto &item : items) if (item.classId == 0 && (!client_ || !client_->isOnline(item.name))) { text(left + 21, item.name, italic, OfflineText, !selectedGroup_ && selectedGroupId_ == item.groupId && selectedItemId_ == item.itemId); rows_.append({QRect(left, y, area.width(), RowHeight), item.groupId, item.itemId, item.name, false}); y += RowHeight; } }
  } else {
    // List Setup: folders with dotted connectors to their buddies.
    const QImage open = art::image(124), closed = art::image(125);
    for (quint16 groupId : groupIds) {
      const bool collapsed = collapsedGroups_.contains(groupId); const QImage &folder = collapsed ? closed : open;
      p.drawImage(QPoint(left + 1, y + (RowHeight - folder.height()) / 2), folder);
      text(left + 22, groupName(items, groupId), TreeFont.bold(), Qt::black, selectedGroup_ && selectedGroupId_ == groupId);
      rows_.append({QRect(left, y, area.width(), RowHeight), groupId, 0, {}, true});
      const int lineX = left + 9, groupBottom = y + RowHeight - 1; y += RowHeight;
      if (collapsed) continue;
      QPen dotted(QColor(128, 128, 128)); dotted.setStyle(Qt::DotLine);
      int lastY = groupBottom;
      for (const auto &item : buddiesOf(groupId, false)) {
        const int cy = y + RowHeight / 2; p.setPen(dotted); p.drawLine(lineX, lastY, lineX, cy); p.drawLine(lineX, cy, left + 20, cy); lastY = cy;
        text(left + 22, item.name, TreeFont, Qt::black, !selectedGroup_ && selectedGroupId_ == item.groupId && selectedItemId_ == item.itemId);
        rows_.append({QRect(left, y, area.width(), RowHeight), item.groupId, item.itemId, item.name, false}); y += RowHeight;
      }
    }
  }
  p.restore();
  const int content = y + treeScroll_ - area.top();
  treeScroll_ = std::clamp(treeScroll_, 0, std::max(0, content - area.height()));
}

CtlObject *BuddyListWindow::buttonAt(const QPoint &point) const {
  CtlObject *found = nullptr;
  std::function<void(CtlObject &)> visit = [&](CtlObject &o) { if (!o.shown()) return; if (o.kind == CtlObject::Kind::ArtButton && o.art[0] && o.windowRect().contains(point)) found = &o; for (const auto &child : o.children) visit(*child); };
  if (group_) visit(*group_); return found;
}

void BuddyListWindow::contentMousePress(const QPoint &point, Qt::MouseButton button) {
  if (button != Qt::LeftButton) return;
  layout();
  for (int i = 0; i < menuRects_.size(); ++i) if (menuRects_[i].contains(point)) { openMenu(i); return; }
  if (CtlObject *tabs = group_ ? group_->find(Tabs) : nullptr) {
    const QVector<QRect> rects = tabRects(*tabs);
    for (int i = 0; i < rects.size(); ++i) if (rects[i].contains(point)) { listSetup_ = tabs->children[i]->id == SetupPage; ctlSetPage(*tabs, tabs->children[i]->id); treeScroll_ = 0; requestUpdate(); return; }
  }
  if (CtlObject *target = buttonAt(point)) { pressed_ = target->id; requestUpdate(); return; }
  if (!treeArea_.contains(point)) return;
  for (const auto &row : rows_) if (row.rect.contains(point)) {
    selectedGroupId_ = row.groupId; selectedItemId_ = row.itemId; selectedGroup_ = row.group; selectedName_ = row.name;
    if (row.group && point.x() < treeArea_.left() + (listSetup_ ? 20 : 12)) { if (collapsedGroups_.contains(row.groupId)) collapsedGroups_.remove(row.groupId); else collapsedGroups_.insert(row.groupId); }
    requestUpdate(); return;
  }
}
void BuddyListWindow::contentMouseMove(const QPoint &point) {
  int menu = -1; for (int i = 0; i < menuRects_.size(); ++i) if (menuRects_[i].contains(point)) menu = i;
  CtlObject *target = buttonAt(point); const quint32 hovered = target ? target->id : 0;
  if (menu != hoveredMenu_ || hovered != hovered_) { hoveredMenu_ = menu; hovered_ = hovered; requestUpdate(); }
}
void BuddyListWindow::contentLeave() { if (hovered_ || hoveredMenu_ >= 0) { hovered_ = 0; hoveredMenu_ = -1; requestUpdate(); } }
void BuddyListWindow::contentMouseRelease(const QPoint &point, Qt::MouseButton button) {
  if (button != Qt::LeftButton || !pressed_) return;
  const quint32 pressed = pressed_; pressed_ = 0; requestUpdate();
  CtlObject *target = buttonAt(point); if (target && target->id == pressed) command(int(pressed));
}
void BuddyListWindow::contentMouseDoubleClick(const QPoint &point, Qt::MouseButton button) {
  if (button != Qt::LeftButton || !treeArea_.contains(point)) return;
  for (const auto &row : rows_) if (!row.group && row.rect.contains(point)) { emit actionRequested(SendIm, row.name); return; }
}
void BuddyListWindow::contentKeyPress(QKeyEvent *event) {
  if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !selectedName_.isEmpty() && !selectedGroup_) { emit actionRequested(SendIm, selectedName_); event->accept(); return; }
  if (event->modifiers().testFlag(Qt::AltModifier) && event->key() == Qt::Key_I) { emit actionRequested(SendIm, selectedGroup_ ? QString() : selectedName_); event->accept(); return; }
  WindowBase::contentKeyPress(event);
}
void BuddyListWindow::wheelEvent(QWheelEvent *event) {
  if (!treeArea_.contains(canvasPoint(event->position().toPoint()))) { event->ignore(); return; }
  treeScroll_ = std::max(0, treeScroll_ - event->angleDelta().y() / 120 * RowHeight * 3); requestUpdate(); event->accept();
}
void BuddyListWindow::command(int id) {
  switch (id) {
  case AwayMessages: { CtlObject *button = group_ ? group_->find(AwayMessages) : nullptr; showAwayMenu(button ? button->windowRect() : QRect()); return; }
  default: emit actionRequested(id, selectedGroup_ ? QString() : selectedName_); return; // 139 IM, 561 chat, 138 info, 1140 Today, 174 Preferences, list setup buttons
  }
}
void BuddyListWindow::showAwayMenu(const QRect &anchor) {
  MenuItem away; away.id = 0; away.children = {MenuItem{QStringLiteral("PLACEHOLDER"), 660}};
  const int id = popupMenu(this, preparedMenu(away), canvasToGlobal(anchor.bottomLeft() + QPoint(0, 1)));
  if (id) emit actionRequested(id, QString());
}

void BuddyListWindow::closeRequested() { hide(); showClosedNotice(); }
void BuddyListWindow::showClosedNotice() {
#ifdef Q_OS_WIN
  // RT_DIALOG 305, shown when the Buddy List window is closed while signed on.
  if (settings_.value(QStringLiteral("preferences/hideBuddyListClosedNotice"), false).toBool()) return;
  const QJsonObject dialog = originalDialog(305); if (dialog.isEmpty()) return;
  const QByteArray bytes = nativeDialogTemplate(dialog, true);
  struct State { HFONT font = nullptr; HICON icon = nullptr; HBITMAP bitmap = nullptr; bool suppress = false; } state;
  auto proc = [](HWND window, UINT message, WPARAM wParam, LPARAM lParam) -> INT_PTR {
    auto *s = reinterpret_cast<State *>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
      s = reinterpret_cast<State *>(lParam); SetWindowLongPtrW(window, DWLP_USER, lParam);
      LOGFONTW logical{}; HFONT dialogFont = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)); if (dialogFont && GetObjectW(dialogFont, sizeof(logical), &logical)) s->font = CreateFontIndirectW(&logical);
      createNativeControls(window, originalDialog(305), s->font);
      for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const LONG_PTR type = GetWindowLongPtrW(child, GWL_STYLE) & SS_TYPEMASK; wchar_t name[16]{}; GetClassNameW(child, name, 16);
        if (_wcsicmp(name, L"STATIC") != 0) continue;
        if (type == SS_ICON) { s->icon = QIcon(QStringLiteral(":/aim/icons/108")).pixmap(32, 32).toImage().toHICON(); SendMessageW(child, STM_SETICON, reinterpret_cast<WPARAM>(s->icon), 0); }
        else if (type == SS_BITMAP) { s->bitmap = QImage(QStringLiteral(":/aim/art/1036")).toHBITMAP(); SendMessageW(child, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(s->bitmap)); }
      }
      SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1))));
      return TRUE;
    }
    if (message == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)) { s->suppress = IsDlgButtonChecked(window, 362) == BST_CHECKED; EndDialog(window, IDOK); return TRUE; }
    if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
    return FALSE;
  };
  DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATE>(bytes.constData()), nullptr, proc, reinterpret_cast<LPARAM>(&state));
  if (state.font) DeleteObject(state.font); if (state.icon) DestroyIcon(state.icon); if (state.bitmap) DeleteObject(state.bitmap);
  if (state.suppress) settings_.setValue(QStringLiteral("preferences/hideBuddyListClosedNotice"), true);
#endif
}

void BuddyListWindow::requestExit() {
#ifdef Q_OS_WIN
  bool suppress = false; if (settings_.value(QStringLiteral("preferences/confirmExit"), true).toBool() && !confirmExit(suppress)) return; if (suppress) settings_.setValue(QStringLiteral("preferences/confirmExit"), false); if (client_) client_->signOff(); emit exitAccepted(); return;
#endif
  if (confirmation_) { confirmation_->show(); confirmation_->requestActivate(); return; }
  if (!settings_.value(QStringLiteral("preferences/confirmExit"), true).toBool()) { if (client_) client_->signOff(); emit exitAccepted(); return; }
  auto *dialog = new ExitConfirmationWindow(this, [this](bool yes, bool suppress) { if (suppress) settings_.setValue(QStringLiteral("preferences/confirmExit"), false); confirmation_.clear(); if (yes) { if (client_) client_->signOff(); emit exitAccepted(); } });
  confirmation_ = dialog; connect(dialog, &QObject::destroyed, this, [this] { confirmation_.clear(); }); dialog->show();
}
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
  if (index < 0 || index >= menuRects_.size()) return;
  openMenu_ = index; renderNow();
  const int command = popupMenu(this, preparedMenu(menuBar_[index]), canvasToGlobal(menuRects_[index].bottomLeft() + QPoint(0, 1)));
  openMenu_ = -1; hoveredMenu_ = -1; requestUpdate();
  if (command) emit actionRequested(command, selectedGroup_ ? QString() : selectedName_);
}
