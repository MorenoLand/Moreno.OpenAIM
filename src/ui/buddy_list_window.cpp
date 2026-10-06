#include "buddy_list_window.h"
#include "art.h"
#include "ctl_group.h"
#include "gdi_text.h"
#include "native_dialog.h"
#include "ate_link.h"
#include "away_dialog.h"
#include "preferences.h"
#include <shellapi.h>
#include "user_actions.h"
#include <QClipboard>
#include <QStyleHints>
#include <QTextCursor>
#include <QGuiApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWheelEvent>
#include <QUrl>
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
                 SendIm = 139, Chat = 561, Info = 138, Today = 1140, AwayButton = 1488, Preferences = 174, AddBuddy = 143, AddGroup = 145, Delete = 146, Find = 141 };
constexpr int MenuRowHeight = 19, MenuPadding = 7;
const GdiFont MenuFont{QStringLiteral("MS Sans Serif"), -13};      // matches the reference screenshot's menu bar
const GdiFont TreeFont{QStringLiteral("Arial"), -12};              // tab captions (reference screenshot)
// Tree font: Preferences > Buddy List > Buddy List Font (default Arial 9 pt = -12 px, the reference screenshot).
GdiFont treeFont() { const QFont font = prefs::buddyListFont(); return GdiFont{font.family(), -font.pixelSize()}; }
int rowHeight() { return std::max(16, gdiTextSize(treeFont(), QStringLiteral("Wg")).height() + 2); }
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
using aim::oscar::FeedbagItem;
QVector<quint16> memberOrder(const QVector<aim::oscar::FeedbagItem> &items, quint16 id);
// Display order: the group order attribute (0x00C8) of the master group, then each group's own order attribute.
QVector<quint16> orderedGroups(const QVector<FeedbagItem> &items) {
  QVector<quint16> groupIds;
  for (const auto &item : items) if (item.classId == 1 && item.groupId != 0 && !item.name.isEmpty() && !groupIds.contains(item.groupId)) groupIds.append(item.groupId);
  const auto order = memberOrder(items, 0);
  if (!order.isEmpty()) std::stable_sort(groupIds.begin(), groupIds.end(), [&](quint16 a, quint16 b) { const int x = order.indexOf(a), y = order.indexOf(b); return (x < 0 ? order.size() : x) < (y < 0 ? order.size() : y); });
  return groupIds;
}
QVector<FeedbagItem> orderedBuddies(const QVector<FeedbagItem> &items, quint16 groupId) {
  QVector<FeedbagItem> buddies; for (const auto &item : items) if (item.classId == 0 && item.groupId == groupId) buddies.append(item);
  const auto order = memberOrder(items, groupId);
  if (!order.isEmpty()) std::stable_sort(buddies.begin(), buddies.end(), [&](const auto &a, const auto &b) { const int x = order.indexOf(a.itemId), z = order.indexOf(b.itemId); return (x < 0 ? order.size() : x) < (z < 0 ? order.size() : z); });
  return buddies;
}
bool sameRecord(const FeedbagItem &a, const FeedbagItem &b) { return a.classId == b.classId && a.groupId == b.groupId && a.itemId == b.itemId; }
// The whole list in display order; OscarClient::applyRoster derives the order attributes from the vector order.
QVector<FeedbagItem> orderedRoster(const QVector<FeedbagItem> &items) {
  QVector<FeedbagItem> result; for (const auto &item : items) if (item.classId == 1 && item.groupId == 0) result.append(item);
  for (quint16 group : orderedGroups(items)) { for (const auto &item : items) if (item.classId == 1 && item.groupId == group) result.append(item); result += orderedBuddies(items, group); }
  for (const auto &item : items) if (std::none_of(result.begin(), result.end(), [&](const auto &other) { return sameRecord(item, other); })) result.append(item);
  return result;
}
quint16 freeId(const QVector<FeedbagItem> &items, bool group) { QSet<quint16> used; for (const auto &item : items) used.insert(group ? item.groupId : item.itemId); for (quint32 id = 1; id < 0x8000; ++id) if (!used.contains(quint16(id))) return quint16(id); return 0; }
int groupIndex(const QVector<FeedbagItem> &items, quint16 groupId) { for (int i = 0; i < items.size(); ++i) if (items[i].classId == 1 && items[i].groupId == groupId) return i; return -1; }
int buddyIndex(const QVector<FeedbagItem> &items, quint16 groupId, quint16 itemId) { for (int i = 0; i < items.size(); ++i) if (items[i].classId == 0 && items[i].groupId == groupId && items[i].itemId == itemId) return i; return -1; }
int blockEnd(const QVector<FeedbagItem> &items, quint16 groupId) { int i = groupIndex(items, groupId); if (i < 0) return -1; ++i; while (i < items.size() && items[i].classId == 0 && items[i].groupId == groupId) ++i; return i; }
QString nicknameKey(const QString &name) { QString key = name.toCaseFolded(); key.remove(QLatin1Char(' ')); return key; } // IsSameNickname
// AnalyzeNickname: 1 valid, 2 e-mail address, 3 too short or invalid characters.
int analyzeNickname(const QString &name) {
  if (name.contains(QLatin1Char('@'))) return 2;
  const QString compact = QString(name).remove(QLatin1Char(' '));
  if (compact.size() < 3 || compact.size() > 16) return 3;
  if (std::all_of(compact.begin(), compact.end(), [](QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); })) return 1; // ICQ number
  if (!compact.at(0).isLetter()) return 3;
  for (const QChar c : compact) if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9')))) return 3;
  return 1;
}
#ifdef Q_OS_WIN
HWND ownerHandle(QWindow *owner) { return owner && owner->handle() ? reinterpret_cast<HWND>(owner->winId()) : nullptr; }
void errorBox(QWindow *owner, const QString &text) { MessageBeep(MB_ICONEXCLAMATION); MessageBoxW(ownerHandle(owner), reinterpret_cast<LPCWSTR>(text.utf16()), L"AOL Instant Messenger (SM) Error", MB_OK | MB_ICONEXCLAMATION | MB_TASKMODAL); }
bool queryBox(QWindow *owner, const QString &text) { return MessageBoxW(ownerHandle(owner), reinterpret_cast<LPCWSTR>(text.utf16()), L"AOL Instant Messenger (SM)", MB_YESNO | MB_ICONQUESTION | MB_TASKMODAL) == IDYES; }
// RT_DIALOG 209: OK = correct the item, Delete (or Esc) = delete it (buddyui 0x1128ac41).
bool okOrDelete(QWindow *owner, const QString &text) {
  MessageBeep(MB_ICONEXCLAMATION);
  return runOriginalDialog(ownerHandle(owner), 209, [&](HWND dialog) { SetDlgItemTextW(dialog, 461, reinterpret_cast<LPCWSTR>(text.utf16())); },
    [](HWND dialog, int id, int) { if (id == IDOK) { EndDialog(dialog, IDOK); return true; } if (id == 269 || id == IDCANCEL) { EndDialog(dialog, 269); return true; } return false; }) == IDOK;
}
// RT_DIALOG 254: the bad name in edit 991; Edit = use the corrected text, Delete = delete the item.
bool correctName(QWindow *owner, const QString &message, QString &name) {
  MessageBeep(MB_ICONEXCLAMATION); QString edited = name;
  const INT_PTR result = runOriginalDialog(ownerHandle(owner), 254, [&](HWND dialog) { SetDlgItemTextW(dialog, 461, reinterpret_cast<LPCWSTR>(message.utf16())); SetDlgItemTextW(dialog, 991, reinterpret_cast<LPCWSTR>(name.utf16())); SendDlgItemMessageW(dialog, 991, EM_LIMITTEXT, 32, 0); },
    [&](HWND dialog, int id, int) { if (id == IDOK) { edited = nativeWindowText(GetDlgItem(dialog, 991)); EndDialog(dialog, IDOK); return true; } if (id == 269 || id == IDCANCEL) { EndDialog(dialog, 269); return true; } return false; });
  if (result != IDOK) return false; name = edited; return true;
}
#else
void errorBox(QWindow *, const QString &) {}
bool queryBox(QWindow *, const QString &) { return true; }
bool okOrDelete(QWindow *, const QString &) { return true; }
bool correctName(QWindow *, const QString &, QString &) { return false; }
#endif
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
  // aim.exe is a pre-Vista executable, so Windows gives it 1 px thinner frames on each side than ours: its client is
  // 2 px wider for the same outer size (134 vs 132 px on the reference screenshot).
  return QSize(outer.width() - (frame.right - frame.left) + 2 + 2, outer.height() - (frame.bottom - frame.top) + 24 + 3);
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
  connect(client_, &OscarClient::rosterChanged, this, [this] {
    // A new *New Group* goes into label editing once the server has stored it (Add Group, 0x11289837).
    if (editGroupAfterRoster_ && groupIndex(client_->roster(), editGroupAfterRoster_) >= 0) { selectRow(editGroupAfterRoster_, 0, true, QString()); editGroupAfterRoster_ = 0; scheduleEdit(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 200); }
    requestUpdate();
  });
  connect(client_, &OscarClient::rosterEditFinished, this, [this](bool) { QTimer::singleShot(0, this, [this] { if (!rosterQueue_.isEmpty() && client_ && !client_->rosterEditPending()) rosterQueue_.takeFirst()(); }); });
  connect(client_, &OscarClient::loginStageChanged, this, [this](int stage) { if (stage == 0) { rosterQueue_.clear(); editGroupAfterRoster_ = 0; } });
  editTimer_.setSingleShot(true);
  // Preferences > Buddy List "Flash Buddy List window when buddies sign on or off" (not for the burst right after sign-on).
  connect(client_, &OscarClient::buddyPresenceChanged, this, [this](const QString &, bool) {
#ifdef Q_OS_WIN
    if (!prefs::flashOnSignOnOff() || !handle() || isActive() || created_.elapsed() < 5000) return;
    FLASHWINFO flash{sizeof(flash), reinterpret_cast<HWND>(winId()), FLASHW_ALL, 3, 0}; FlashWindowEx(&flash);
#endif
  });
  created_.start();
  connect(&editTimer_, &QTimer::timeout, this, [this] { beginEdit(); });
  menuBar_ = loadMenuResource(103);
  QSize outer(142, 450);
  if (group_) {
    ctlShowControl(*group_, WebSearch, false); // VALUERES 138 = 1 hides the web search group (0x112844e1)
    ctlShowControl(*group_, Ticker, prefs::showStockTicker()); ctlShowControl(*group_, TickerCaption, prefs::showStockTicker()); // Stock Ticker "Show stock ticker in Buddy List window"
    if (CtlObject *tabs = group_->find(Tabs)) ctlSetPage(*tabs, OnlinePage);
    const QSize minimum = minimumOuter(ctlIdealSize(*group_, aimEnvironment()));
    setResizable(canvasForOuter(minimum));
    outer = QSize(minimum.width(), std::max(450, minimum.height()));
  } else setResizable(QSize(142, 350));
  setCanvasSize(canvasForOuter(outer));
  if (QScreen *screen = QGuiApplication::primaryScreen()) setFramePosition(QPoint(screen->geometry().right() + 1 - outer.width(), 0));
#ifdef Q_OS_WIN
  if (settings_.value(QStringLiteral("preferences/keepBuddyListOnTop"), false).toBool()) QTimer::singleShot(0, this, [this] { SetWindowPos(reinterpret_cast<HWND>(winId()), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); });
#endif
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
    QImage image = art::image(o.art[state] ? o.art[state] : o.art[0]);
    if ((o.id == AddBuddy || o.id == Delete) && !hasRealGroup()) image = art::disabled(art::image(o.art[0])); // 0x11285b6c
    p.drawImage(r.topLeft() + QPoint(1, 1), image);
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

// _Oscar_TabGroup, measured pixel for pixel from the reference screenshot: Arial -12 captions (selected bold), each tab
// caption width + 18 px (+1 when selected); the selected tab rises 2 px above the others; grey (152) left/top edges,
// grey (160) + black right edges; the page frame spans the client width with a grey left/top and black right/bottom.
namespace { const QColor TabEdge(152, 152, 152), TabInner(160, 160, 160); }
QRect BuddyListWindow::tabWindow(const CtlObject &tabs) const {
  const int left = tabs.pos.x() + tabs.margins.left() - 5, top = tabs.pos.y() + tabs.margins.top() - 27; // window origin (0x122055d9)
  return QRect(QPoint(left, top), QPoint(tabs.pos.x() + tabs.size.width() - 1, top + tabs.size.height() - tabs.margins.top() - tabs.margins.bottom() + 30));
}
QVector<QRect> BuddyListWindow::tabRects(const CtlObject &tabs) const {
  QVector<QRect> rects; const QRect window = tabWindow(tabs); int x = window.left();
  for (const auto &page : tabs.children) {
    const bool selected = !page->hidden();
    const int w = gdiTextSize(selected ? TreeFont.bold() : TreeFont, aimString(page->titleId)).width() + 18 + (selected ? 1 : 0);
    rects.append(QRect(x, window.top() + (selected ? 0 : 2), w, selected ? 23 : 20)); x += w;
  }
  return rects;
}
void BuddyListWindow::paintTabs(QPainter &p, CtlObject &tabs) {
  const QRect window = tabWindow(tabs); const QVector<QRect> rects = tabRects(tabs);
  const int pageTop = window.top() + 22;
  int selectedLeft = -1, selectedRight = -1;
  for (int i = 0; i < rects.size(); ++i) if (!tabs.children[i]->hidden()) { selectedLeft = rects[i].left(); selectedRight = rects[i].right(); }
  // Page frame.
  p.setPen(TabEdge); p.drawLine(window.left(), selectedLeft == window.left() ? window.top() + 2 : pageTop, window.left(), window.bottom());
  if (selectedLeft > window.left()) p.drawLine(window.left(), pageTop, selectedLeft, pageTop);
  p.drawLine(selectedRight + 1, pageTop, window.right() - 1, pageTop);
  p.setPen(Qt::black); p.drawLine(window.right(), pageTop + 1, window.right(), window.bottom()); p.drawLine(window.left(), window.bottom(), window.right(), window.bottom());
  for (int i = 0; i < rects.size(); ++i) {
    const QRect t = rects[i]; const bool selected = !tabs.children[i]->hidden();
    const int x = t.left(), r = t.right(), top = t.top(), bottom = selected ? pageTop : pageTop - 1;
    const bool hideLeft = !selected && i > 0 && !tabs.children[i - 1]->hidden();
    p.fillRect(QRect(x + 1, top + 1, t.width() - 2, bottom - top), art::Face);
    // Caption first (DT_CENTER over the whole tab, as the reference shows), then the edges on top of its background.
    drawGdiText(p, QRect(x, window.top() + 2, t.width(), 20), aimString(tabs.children[i]->titleId), selected ? TreeFont.bold() : TreeFont, Qt::black, art::Face, GdiSingleLine | GdiCenter | GdiVCenter | GdiNoPrefix);
    p.setPen(TabEdge);
    if (selected) { p.drawLine(x + 2, top, r - 2, top); p.drawPoint(x + 1, top + 1); p.drawPoint(r - 1, top + 1); }
    else { p.drawLine(x + 1, top, r - 1, top); p.drawPoint(x, top + 1); p.drawPoint(r, top + 1); }
    if (!hideLeft) p.drawLine(x, top + 2, x, bottom);
    p.setPen(TabInner); p.drawLine(r - 1, top + 2, r - 1, bottom);
    p.setPen(Qt::black); p.drawLine(r, top + 2, r, bottom);
  }
}
void BuddyListWindow::paintTree(QPainter &p, const QRect &area) {
  const QVector<aim::oscar::FeedbagItem> items = client_ ? client_->roster() : QVector<aim::oscar::FeedbagItem>{};
  rows_.clear(); editRect_ = QRect();
  const GdiFont TreeFont = treeFont(); const int RowHeight = rowHeight();
  const QVector<quint16> groupIds = orderedGroups(items);
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
  auto buddySelected = [&](const FeedbagItem &item) { return !selectedGroup_ && !selectedPending_ && selectedGroupId_ == item.groupId && selectedItemId_ == item.itemId; };
  if (!listSetup_) {
    for (quint16 groupId : groupIds) {
      int total = 0, online = 0; for (const auto &item : items) if (item.classId == 0 && item.groupId == groupId) { ++total; if (client_ && client_->isOnline(item.name)) ++online; }
      const bool collapsed = collapsedGroups_.contains(groupId);
      triangle(left + 3, collapsed);
      text(left + 13, QStringLiteral("%1 (%2/%3)").arg(groupName(items, groupId)).arg(online).arg(total), TreeFont.bold(), Qt::black, selectedGroup_ && selectedGroupId_ == groupId);
      rows_.append({QRect(left, y, area.width(), RowHeight), groupId, 0, {}, true}); y += RowHeight;
      if (collapsed) continue;
      for (const auto &item : orderedBuddies(items, groupId)) {
        if (!client_ || !client_->isOnline(item.name)) continue;
        const bool dim = prefs::dimIdleBuddies() && client_->idleMinutes(item.name) >= prefs::dimIdleMinutes(); // "Dim buddies after they have been idle for N minutes"
        text(left + 21, item.name, TreeFont, dim ? OfflineText : QColor(Qt::black), buddySelected(item)); rows_.append({QRect(left, y, area.width(), RowHeight), item.groupId, item.itemId, item.name, false}); y += RowHeight;
      }
    }
    int total = 0, offline = 0; for (const auto &item : items) if (item.classId == 0) { ++total; if (!client_ || !client_->isOnline(item.name)) ++offline; }
    const bool collapsed = collapsedGroups_.contains(0);
    triangle(left + 3, collapsed);
    text(left + 13, QStringLiteral("Offline (%1/%2)").arg(offline).arg(total), TreeFont.bold(), OfflineText, selectedGroup_ && selectedGroupId_ == 0);
    rows_.append({QRect(left, y, area.width(), RowHeight), 0, 0, {}, true}); y += RowHeight;
    if (!collapsed) { GdiFont italic = TreeFont; italic.italic = true; for (const auto &item : items) if (item.classId == 0 && (!client_ || !client_->isOnline(item.name))) { text(left + 21, item.name, italic, OfflineText, buddySelected(item)); rows_.append({QRect(left, y, area.width(), RowHeight), item.groupId, item.itemId, item.name, false}); y += RowHeight; } }
  } else {
    // List Setup: folders with dotted connectors to their buddies.
    const QImage open = art::image(124), closed = art::image(125);
    for (quint16 groupId : groupIds) {
      const bool collapsed = collapsedGroups_.contains(groupId); const QImage &folder = collapsed ? closed : open;
      p.drawImage(QPoint(left + 1, y + (RowHeight - folder.height()) / 2), folder);
      if (edit_ && edit_->group && edit_->groupId == groupId) { editRect_ = QRect(left + 21, y, area.right() - left - 21, RowHeight); paintEditor(p, editRect_); }
      else text(left + 22, groupName(items, groupId), TreeFont.bold(), Qt::black, selectedGroup_ && selectedGroupId_ == groupId);
      rows_.append({QRect(left, y, area.width(), RowHeight), groupId, 0, {}, true});
      const int lineX = left + 9, groupBottom = y + RowHeight - 1; y += RowHeight;
      if (collapsed) continue;
      QVector<std::pair<FeedbagItem, bool>> members; for (const auto &item : orderedBuddies(items, groupId)) members.append({item, false});
      if (pendingBuddy_ && pendingBuddy_->groupId == groupId) {
        FeedbagItem placeholder; placeholder.name = aimString(162); placeholder.groupId = groupId; qsizetype at = 0;
        for (qsizetype i = 0; i < members.size(); ++i) if (pendingBuddy_->afterItemId && members[i].first.itemId == pendingBuddy_->afterItemId) at = i + 1;
        members.insert(at, {placeholder, true});
      }
      QPen dotted(QColor(128, 128, 128)); dotted.setStyle(Qt::DotLine);
      int lastY = groupBottom;
      for (const auto &[item, pending] : members) {
        const int cy = y + RowHeight / 2; p.setPen(dotted); p.drawLine(lineX, lastY, lineX, cy); p.drawLine(lineX, cy, left + 20, cy); lastY = cy;
        const bool editing = edit_ && !edit_->group && (pending ? edit_->pending : !edit_->pending && edit_->groupId == item.groupId && edit_->itemId == item.itemId);
        if (editing) { editRect_ = QRect(left + 21, y, area.right() - left - 21, RowHeight); paintEditor(p, editRect_); }
        else text(left + 22, item.name, TreeFont, Qt::black, pending ? selectedPending_ : buddySelected(item));
        rows_.append({QRect(left, y, area.width(), RowHeight), item.groupId, item.itemId, item.name, false, pending}); y += RowHeight;
      }
    }
  }
  p.restore();
  const int content = y + treeScroll_ - area.top();
  treeScroll_ = std::clamp(treeScroll_, 0, std::max(0, content - area.height()));
}
void BuddyListWindow::paintEditor(QPainter &p, const QRect &r) {
  // Child Edit control of the tree: border, 3 px left margin, all text selected at the start.
  p.fillRect(r, Qt::white); p.setPen(Qt::black); p.setBrush(Qt::NoBrush); p.drawRect(r.adjusted(0, 0, -1, -1));
  QFont font(QStringLiteral("Arial")); font.setPixelSize(12); p.setFont(font); const QFontMetrics metrics(font);
  const QString value = edit_->editor->text(); const QTextCursor &cursor = edit_->editor->cursor;
  const int start = cursor.selectionStart(), end = cursor.selectionEnd(), x = r.left() + 3, baseline = r.top() + (r.height() + metrics.ascent() - metrics.descent()) / 2;
  p.save(); p.setClipRect(r.adjusted(1, 1, -1, -1));
  p.drawText(x, baseline, value);
  if (end > start) { const QRect selection(x + metrics.horizontalAdvance(value.left(start)), r.top() + 2, metrics.horizontalAdvance(value.mid(start, end - start)), r.height() - 4); p.fillRect(selection, QColor(0, 120, 215)); p.setPen(Qt::white); p.drawText(selection.left(), baseline, value.mid(start, end - start)); }
  else { const int caret = x + metrics.horizontalAdvance(value.left(cursor.position())); p.drawLine(caret, r.top() + 2, caret, r.bottom() - 2); }
  p.restore();
}

CtlObject *BuddyListWindow::buttonAt(const QPoint &point) const {
  CtlObject *found = nullptr;
  std::function<void(CtlObject &)> visit = [&](CtlObject &o) { if (!o.shown()) return; if (o.kind == CtlObject::Kind::ArtButton && o.art[0] && o.windowRect().contains(point)) found = &o; for (const auto &child : o.children) visit(*child); };
  if (group_) visit(*group_); return found;
}

void BuddyListWindow::contentMousePress(const QPoint &point, Qt::MouseButton button) {
  if (button != Qt::LeftButton && button != Qt::RightButton) return;
  layout();
  if (edit_) {
    if (editRect_.contains(point)) {
      if (button == Qt::LeftButton) {
        QFont font(QStringLiteral("Arial")); font.setPixelSize(12); const QFontMetrics metrics(font); const QString value = edit_->editor->text(); int position = 0;
        while (position < value.size() && editRect_.left() + 3 + metrics.horizontalAdvance(value.left(position + 1)) - metrics.horizontalAdvance(value.at(position)) / 2 < point.x()) ++position;
        edit_->editor->cursor.setPosition(position); requestUpdate();
      }
      return;
    }
    endEdit(true, false); // focus moves away from the editor: commit without the interactive checks
  }
  editTimer_.stop();
  if (button == Qt::RightButton) {
    if (!treeArea_.contains(point)) return;
    for (const auto &row : rows_) if (row.rect.contains(point)) { selectRow(row.groupId, row.itemId, row.group, row.name, row.pending); renderNow(); showContextMenu(point); return; }
    return;
  }
  for (int i = 0; i < menuRects_.size(); ++i) if (menuRects_[i].contains(point)) { openMenu(i); return; }
  if (CtlObject *tabs = group_ ? group_->find(Tabs) : nullptr) {
    const QVector<QRect> rects = tabRects(*tabs);
    for (int i = 0; i < rects.size(); ++i) if (rects[i].contains(point)) { showPage(tabs->children[i]->id == SetupPage); return; }
  }
  if (CtlObject *target = buttonAt(point)) {
    if ((target->id == AddBuddy || target->id == Delete) && !hasRealGroup()) return; // disabled without a real group (0x11285b6c)
    if (target->id == Find || target->id == AwayButton) { command(int(target->id)); return; } // these pop-ups open on mouse down (0xBCD)
    pressed_ = target->id; requestUpdate(); return;
  }
  if (!treeArea_.contains(point)) return;
  for (const auto &row : rows_) if (row.rect.contains(point)) {
    pressedOnSelection_ = listSetup_ && (row.pending ? selectedPending_ : !selectedPending_ && row.group == selectedGroup_ && row.groupId == selectedGroupId_ && row.itemId == selectedItemId_);
    selectRow(row.groupId, row.itemId, row.group, row.name, row.pending);
    if (row.group && point.x() < treeArea_.left() + (listSetup_ ? 20 : 12)) { pressedOnSelection_ = false; if (collapsedGroups_.contains(row.groupId)) collapsedGroups_.remove(row.groupId); else collapsedGroups_.insert(row.groupId); }
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
  if (button != Qt::LeftButton) return;
  if (pressed_) { const quint32 pressed = pressed_; pressed_ = 0; requestUpdate(); CtlObject *target = buttonAt(point); if (target && target->id == pressed) command(int(pressed)); return; }
  // A click on the already selected row starts the label editor after the double-click time + 200 ms (oscarui 0x12212b56).
  if (!pressedOnSelection_) return;
  pressedOnSelection_ = false;
  for (const auto &row : rows_) if (row.rect.contains(point)) { scheduleEdit(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 200); return; }
}
void BuddyListWindow::contentMouseDoubleClick(const QPoint &point, Qt::MouseButton button) {
  editTimer_.stop(); pressedOnSelection_ = false;
  if (button != Qt::LeftButton || !treeArea_.contains(point) || edit_) return;
  // Activate (tree 0x465, event 5): a group toggles, an online buddy opens an IM to it, an offline buddy an empty IM.
  for (const auto &row : rows_) if (row.rect.contains(point)) {
    if (row.group) { if (collapsedGroups_.contains(row.groupId)) collapsedGroups_.remove(row.groupId); else collapsedGroups_.insert(row.groupId); requestUpdate(); }
    else if (!row.pending) emit actionRequested(SendIm, client_ && client_->isOnline(row.name) ? row.name : QString());
    return;
  }
}
void BuddyListWindow::contentKeyPress(QKeyEvent *event) {
  if (edit_) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) endEdit(true, true);
    else if (event->key() == Qt::Key_Escape) endEdit(false, false);
    else {
      edit_->editor->handleKey(event, true, nullptr);
      if (edit_->editor->text().size() > edit_->maxLength) edit_->editor->setText(edit_->editor->text().left(edit_->maxLength)); // EM_LIMITTEXT
    }
    requestUpdate(); event->accept(); return;
  }
  if (listSetup_ && event->key() == Qt::Key_Delete) { command(Delete); event->accept(); return; } // tree key 0x2E -> event 0x1F, List Setup only
  if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !selectedName_.isEmpty() && !selectedGroup_ && !selectedPending_) { emit actionRequested(SendIm, selectedName_); event->accept(); return; }
  if (event->modifiers().testFlag(Qt::AltModifier) && event->key() == Qt::Key_I) { emit actionRequested(SendIm, selectedGroup_ ? QString() : selectedName_); event->accept(); return; }
  WindowBase::contentKeyPress(event);
}
bool BuddyListWindow::event(QEvent *event) {
  // "Hide taskbar button when Buddy List window is minimized": the window leaves the taskbar; the tray icon restores it.
  if (event->type() == QEvent::WindowStateChange && windowStates().testFlag(Qt::WindowMinimized) && prefs::hideTaskbarWhenMinimized()) QTimer::singleShot(0, this, [this] { hide(); setWindowStates(Qt::WindowNoState); });
  if (event->type() == QEvent::FocusOut && edit_ && !committing_) endEdit(true, false); // WM_KILLFOCUS commits
  return WindowBase::event(event);
}
bool BuddyListWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
#ifdef Q_OS_WIN
  // "Buddy List window can be docked on left or right of screen": dropping the window on a screen edge registers it as
  // an application desktop toolbar (full height, reserving its width); moving it away releases the edge.
  MSG *msg = static_cast<MSG *>(message);
  if (msg->message == WM_ENTERSIZEMOVE && dockEdge_ >= 0) undock();
  else if (msg->message == WM_EXITSIZEMOVE && prefs::dockable()) {
    RECT window{}; GetWindowRect(msg->hwnd, &window); MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
    if (window.left <= monitor.rcWork.left + 8) dock(ABE_LEFT); else if (window.right >= monitor.rcWork.right - 8) dock(ABE_RIGHT);
  } else if (msg->message == WM_DESTROY && dockEdge_ >= 0) undock();
  else if (msg->message == DockCallback && msg->wParam == ABN_POSCHANGED && dockEdge_ >= 0) dock(dockEdge_);
#endif
  return WindowBase::nativeEvent(eventType, message, result);
}
#ifdef Q_OS_WIN
void BuddyListWindow::dock(int edge) {
  HWND window = reinterpret_cast<HWND>(winId()); RECT current{}; GetWindowRect(window, &current); const int width = current.right - current.left;
  APPBARDATA bar{sizeof(bar)}; bar.hWnd = window; bar.uCallbackMessage = DockCallback;
  if (dockEdge_ < 0) { if (!SHAppBarMessage(ABM_NEW, &bar)) return; undockedHeight_ = current.bottom - current.top; }
  MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
  bar.uEdge = UINT(edge); bar.rc = monitor.rcMonitor;
  if (edge == ABE_LEFT) bar.rc.right = bar.rc.left + width; else bar.rc.left = bar.rc.right - width;
  SHAppBarMessage(ABM_QUERYPOS, &bar);
  if (edge == ABE_LEFT) bar.rc.right = bar.rc.left + width; else bar.rc.left = bar.rc.right - width;
  SHAppBarMessage(ABM_SETPOS, &bar);
  dockEdge_ = edge; MoveWindow(window, bar.rc.left, bar.rc.top, bar.rc.right - bar.rc.left, bar.rc.bottom - bar.rc.top, TRUE);
}
void BuddyListWindow::undock() {
  if (dockEdge_ < 0) return; HWND window = reinterpret_cast<HWND>(winId());
  APPBARDATA bar{sizeof(bar)}; bar.hWnd = window; SHAppBarMessage(ABM_REMOVE, &bar); dockEdge_ = -1;
  RECT current{}; GetWindowRect(window, &current); if (undockedHeight_ > 0) SetWindowPos(window, nullptr, 0, 0, current.right - current.left, undockedHeight_, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
#endif
void BuddyListWindow::wheelEvent(QWheelEvent *event) {
  if (!treeArea_.contains(canvasPoint(event->position().toPoint()))) { event->ignore(); return; }
  treeScroll_ = std::max(0, treeScroll_ - event->angleDelta().y() / 120 * rowHeight() * 3); requestUpdate(); event->accept();
}
void BuddyListWindow::command(int id) {
  const QString buddy = selectedGroup_ || selectedPending_ ? QString() : selectedName_;
  switch (id) {
  case AwayButton: { CtlObject *button = group_ ? group_->find(AwayButton) : nullptr; showAwayMenu(button ? button->windowRect() : QRect()); return; }
  case Find: showNetFindMenu(); return;
  case AddBuddy: addBuddy(); return;
  case AddGroup: addGroup(); return;
  case Delete: deleteSelection(); return;
  case 664: editName(); return;
  case 181: { // My AIM > Edit Options > Keep Buddy List on Top
    const bool top = !settings_.value(QStringLiteral("preferences/keepBuddyListOnTop"), false).toBool(); settings_.setValue(QStringLiteral("preferences/keepBuddyListOnTop"), top);
#ifdef Q_OS_WIN
    SetWindowPos(reinterpret_cast<HWND>(winId()), top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
    setFlag(Qt::WindowStaysOnTopHint, top); show();
#endif
    return;
  }
  case 1178: { // Sort Group: alphabetical order of the selected group's buddies
    if (!requireOnline() || !selectedGroupId_) return;
    const quint16 groupId = selectedGroupId_;
    rosterEdit([this, groupId] {
      auto items = orderedRoster(client_->roster()); const int first = groupIndex(items, groupId) + 1, last = blockEnd(items, groupId); if (first <= 0 || last <= first) return;
      std::stable_sort(items.begin() + first, items.begin() + last, [](const auto &a, const auto &b) { return nicknameKey(a.name) < nicknameKey(b.name); });
      client_->applyRoster(items);
    });
    return;
  }
  case 152: if (!buddy.isEmpty()) QGuiApplication::clipboard()->setText(buddy); return; // Copy Buddy Name to Clipboard
  case 1011: if (!buddy.isEmpty() && requireOnline()) rosterEdit([this, buddy] { client_->unblockUser(buddy); }); return;
  case 661: ate::openUrl(aimString(177)); return; // NetFind: Search the Web
  case 662: ate::openUrl(aimString(178)); return; // Search the Yellow Pages
  case 663: ate::openUrl(aimString(176)); return; // Search the White Pages
  case Today: ate::openUrl(aimString(1544) + QStringLiteral("?product=9&platform=1&build=2480")); return; // miscui Today page, in the external browser
  case 1198: { // Send IM Greeting (icbmui 0x1138e1b7)
    QString url = aimString(buddy.isEmpty() ? 1800 : 1801); const QString self = client_ ? client_->screenName() : QString();
    url.replace(url.indexOf(QStringLiteral("%s")), 2, self); if (!buddy.isEmpty()) url.replace(url.indexOf(QStringLiteral("%s")), 2, buddy);
    ate::openUrl(url); return;
  }
  case 158: case 156: case 705: case 159: case 902: case 160: runHelpCommand(this, client_ ? client_->screenName() : QString(), id); return;
  default: emit actionRequested(id, buddy); return; // 139 IM, 561 chat, 138 info, 174 Preferences, 390 block ...
  }
}

// ---- List Setup (Research/remaining_buttons.md section 1) ----
bool BuddyListWindow::hasRealGroup() const { return client_ && !orderedGroups(client_->roster()).isEmpty(); }
void BuddyListWindow::selectRow(quint16 groupId, quint16 itemId, bool group, const QString &name, bool pending) {
  selectedGroupId_ = groupId; selectedItemId_ = itemId; selectedGroup_ = group; selectedName_ = name; selectedPending_ = pending; requestUpdate();
}
void BuddyListWindow::showPage(bool listSetup) {
  if (edit_) endEdit(true, false);
  CtlObject *tabs = group_ ? group_->find(Tabs) : nullptr; if (!tabs) return;
  listSetup_ = listSetup; ctlSetPage(*tabs, listSetup ? SetupPage : OnlinePage); treeScroll_ = 0; requestUpdate();
}
bool BuddyListWindow::requireOnline() {
  if (client_ && client_->connected()) return true;
  errorBox(this, aimString(1556)); return false; // "This action can't be performed while disconnected..."
}
void BuddyListWindow::rosterEdit(std::function<void()> change) {
  // OscarClient applies one feedbag transaction at a time; later edits wait for the running one.
  if (!client_) return;
  if (client_->rosterEditPending()) { rosterQueue_.append(std::move(change)); return; }
  change();
}
// Pre-step of Add Buddy / Add Group (0x1128594a): commit the running edit, then the whole list must be valid.
bool BuddyListWindow::validateList() {
  if (edit_) endEdit(true, true);
  if (edit_) return false;
  if (pendingBuddy_) {
    showPage(true); selectRow(pendingBuddy_->groupId, 0, false, aimString(162), true); renderNow();
    if (okOrDelete(this, aimString(211))) return false;
    pendingBuddy_.reset(); requestUpdate();
  }
  if (client_) for (const auto &item : client_->roster()) if (item.classId == 1 && item.groupId != 0 && item.name.compare(aimString(161), Qt::CaseInsensitive) == 0) {
    showPage(true); selectRow(item.groupId, 0, true, QString()); renderNow();
    if (okOrDelete(this, aimString(212))) return false;
    const quint16 groupId = item.groupId; rosterEdit([this, groupId] { auto items = orderedRoster(client_->roster()); items.erase(std::remove_if(items.begin(), items.end(), [&](const auto &i) { return i.groupId == groupId && (i.classId == 0 || i.classId == 1); }), items.end()); client_->applyRoster(items); });
    break;
  }
  return true;
}
void BuddyListWindow::addBuddy() {
  if (!validateList() || !requireOnline() || !hasRealGroup()) return;
  if (selectedPending_ || (!selectedGroup_ && !selectedItemId_) || selectedGroupId_ == 0) return; // no selection: nothing happens
  // A selected group gets the new row as its first child; a selected buddy gets it right after itself.
  pendingBuddy_ = PendingBuddy{selectedGroupId_, selectedGroup_ ? quint16(0) : selectedItemId_};
  collapsedGroups_.remove(selectedGroupId_);
  showPage(true); selectRow(pendingBuddy_->groupId, 0, false, aimString(162), true);
  scheduleEdit(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 200);
}
void BuddyListWindow::addGroup() {
  if (!validateList() || !requireOnline()) return;
  // Unlike buddies, the group is stored at once as *New Group*, after the selected group (or the selected buddy's group).
  const quint16 after = selectedPending_ || selectedGroupId_ ? selectedGroupId_ : 0;
  showPage(true);
  rosterEdit([this, after] {
    auto items = orderedRoster(client_->roster()); const quint16 id = freeId(items, true); if (!id) return;
    FeedbagItem group; group.classId = 1; group.groupId = id; group.name = aimString(161);
    int at = after ? blockEnd(items, after) : -1;
    if (at < 0) { const auto groups = orderedGroups(client_->roster()); at = groups.isEmpty() ? (groupIndex(items, 0) + 1) : blockEnd(items, groups.last()); }
    items.insert(std::clamp(at, 0, int(items.size())), group);
    editGroupAfterRoster_ = id;
    if (!client_->applyRoster(items)) editGroupAfterRoster_ = 0;
  });
}
void BuddyListWindow::deleteSelection() {
  if (edit_) endEdit(false, false);
  if (!requireOnline()) return;
  if (selectedPending_) { pendingBuddy_.reset(); selectRow(0, 0, false, QString()); return; } // a placeholder never reached the server
  if (selectedGroup_) {
    const quint16 groupId = selectedGroupId_; if (!groupId) return;
    bool children = false; for (const auto &item : client_->roster()) children = children || (item.classId == 0 && item.groupId == groupId);
    if (children && !queryBox(this, aimString(164))) return; // only a group with buddies asks (table 1.6)
    if (pendingBuddy_ && pendingBuddy_->groupId == groupId) pendingBuddy_.reset();
    rosterEdit([this, groupId] { auto items = orderedRoster(client_->roster()); items.erase(std::remove_if(items.begin(), items.end(), [&](const auto &i) { return i.groupId == groupId && (i.classId == 0 || i.classId == 1); }), items.end()); client_->applyRoster(items); });
  } else if (selectedItemId_) {
    const quint16 groupId = selectedGroupId_, itemId = selectedItemId_;
    if (pendingBuddy_ && pendingBuddy_->groupId == groupId && pendingBuddy_->afterItemId == itemId) pendingBuddy_->afterItemId = 0;
    rosterEdit([this, groupId, itemId] { auto items = orderedRoster(client_->roster()); const int i = buddyIndex(items, groupId, itemId); if (i < 0) return; items.removeAt(i); client_->applyRoster(items); });
  } else return;
  selectRow(0, 0, false, QString());
}
void BuddyListWindow::editName() {
  if (!requireOnline()) return;
  if (!selectedPending_ && !selectedItemId_ && (!selectedGroup_ || !selectedGroupId_)) return; // no selection: nothing
  showPage(true); scheduleEdit(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 200);
}
void BuddyListWindow::scheduleEdit(int delay) { editTimer_.start(delay); }
void BuddyListWindow::beginEdit(const QString &initial, bool typed) {
  if (!listSetup_ || edit_) return; // editing exists only on the List Setup page
  auto edit = std::make_unique<LabelEdit>();
  if (selectedPending_ && pendingBuddy_) { edit->pending = true; edit->groupId = pendingBuddy_->groupId; edit->original = aimString(162); }
  else if (selectedGroup_ && selectedGroupId_) { edit->group = true; edit->groupId = selectedGroupId_; edit->original = groupName(client_->roster(), selectedGroupId_); edit->maxLength = 48; }
  else if (!selectedGroup_ && selectedItemId_) { edit->groupId = selectedGroupId_; edit->itemId = selectedItemId_; edit->original = selectedName_; }
  else return;
  QFont font(QStringLiteral("Arial")); font.setPixelSize(12);
  edit->editor = std::make_unique<TextEditor>(font);
  if (typed) edit->editor->setText(initial);
  else { edit->editor->setText(edit->original); edit->editor->cursor.setPosition(0); edit->editor->cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor); } // EM_SETSEL(0, 0x7fff)
  edit_ = std::move(edit); requestActivate(); requestUpdate();
}
// End of a label edit (0x12212e5f -> node event 0x10, buddyui 0x11288309). Enter runs the interactive checks; focus loss
// only normalises; Esc keeps the old label.
void BuddyListWindow::endEdit(bool commit, bool interactive) {
  if (!edit_ || committing_) return;
  committing_ = true;
  QString text = edit_->editor->text().trimmed();
  const EditResult result = !commit ? EditResult::Reject : edit_->group ? checkGroup(text, interactive) : checkBuddy(text, interactive);
  const bool group = edit_->group, pending = edit_->pending; const quint16 groupId = edit_->groupId, itemId = edit_->itemId; const QString original = edit_->original;
  edit_.reset(); committing_ = false; requestUpdate();
  if (result == EditResult::Reject) return; // the row keeps its old label (an unnamed *New Buddy* stays)
  if (result == EditResult::Delete) {
    if (pending) { pendingBuddy_.reset(); selectRow(0, 0, false, QString()); return; }
    if (group) rosterEdit([this, groupId] { auto items = orderedRoster(client_->roster()); items.erase(std::remove_if(items.begin(), items.end(), [&](const auto &i) { return i.groupId == groupId && (i.classId == 0 || i.classId == 1); }), items.end()); client_->applyRoster(items); });
    else rosterEdit([this, groupId, itemId] { auto items = orderedRoster(client_->roster()); const int i = buddyIndex(items, groupId, itemId); if (i >= 0) { items.removeAt(i); client_->applyRoster(items); } });
    selectRow(0, 0, false, QString()); return;
  }
  if (group) {
    if (text == original) return;
    rosterEdit([this, groupId, text] { auto items = orderedRoster(client_->roster()); const int i = groupIndex(items, groupId); if (i < 0) return; items[i].name = text; client_->applyRoster(items); });
    return;
  }
  if (!pending && nicknameKey(text) == nicknameKey(original) && text == original) return;
  // A buddy is renamed by removing the old item and adding the new name at the same position (RemoveBuddy + AddBuddy).
  const quint16 after = pending && pendingBuddy_ ? pendingBuddy_->afterItemId : 0;
  if (pending) pendingBuddy_.reset();
  selectedName_ = text; selectedPending_ = false;
  rosterEdit([this, groupId, itemId, after, pending, text] {
    auto items = orderedRoster(client_->roster()); const int groupAt = groupIndex(items, groupId); if (groupAt < 0) return;
    int at = groupAt + 1;
    if (pending) { if (after) { const int i = buddyIndex(items, groupId, after); if (i >= 0) at = i + 1; } }
    else { const int i = buddyIndex(items, groupId, itemId); if (i < 0) return; items.removeAt(i); at = i; }
    FeedbagItem buddy; buddy.classId = 0; buddy.groupId = groupId; buddy.itemId = freeId(items, false); buddy.name = text; if (!buddy.itemId) return;
    items.insert(at, buddy);
    if (selectedName_ == text && !selectedGroup_) selectedItemId_ = buddy.itemId;
    client_->applyRoster(items);
  });
}
BuddyListWindow::EditResult BuddyListWindow::checkGroup(QString &text, bool interactive) {
  const auto items = client_ ? client_->roster() : QVector<FeedbagItem>{};
  if (text.isEmpty() || text.compare(aimString(161), Qt::CaseInsensitive) == 0) {
    if (!interactive) return text.isEmpty() ? EditResult::Reject : EditResult::Accept;
    if (orderedGroups(items).size() <= 1) { errorBox(this, aimString(212)); return EditResult::Reject; } // the only group cannot be deleted here
    return okOrDelete(this, aimString(212)) ? EditResult::Reject : EditResult::Delete;
  }
  if (interactive) for (quint16 id : orderedGroups(items)) if (id != edit_->groupId && groupName(items, id).compare(text, Qt::CaseInsensitive) == 0) { errorBox(this, aimString(218)); return EditResult::Reject; }
  return EditResult::Accept;
}
BuddyListWindow::EditResult BuddyListWindow::checkBuddy(QString &text, bool interactive) {
  const auto items = client_ ? client_->roster() : QVector<FeedbagItem>{};
  for (;;) {
    if (text.isEmpty() || text.compare(aimString(162), Qt::CaseInsensitive) == 0) {
      if (!interactive) return EditResult::Reject;
      return okOrDelete(this, aimString(211)) ? EditResult::Reject : EditResult::Delete;
    }
    if (!edit_->pending && nicknameKey(text) == nicknameKey(edit_->original)) return EditResult::Accept; // IsSameNickname: no change
    if (interactive) for (const auto &item : items) if (item.classId == 0 && item.groupId == edit_->groupId && item.itemId != edit_->itemId && nicknameKey(item.name) == nicknameKey(text))
      return okOrDelete(this, aimString(215)) ? EditResult::Reject : EditResult::Delete;
    const int kind = analyzeNickname(text);
    if (kind == 1) break;
    if (!interactive) return EditResult::Reject; // AddBuddy itself refuses invalid names
    if (kind == 2) return queryBox(this, aimString(214)) ? EditResult::Delete : EditResult::Reject; // Yes: Find a Buddy by E-mail, item deleted
    if (!correctName(this, aimString(213), text)) return EditResult::Delete;
    text = text.trimmed();
  }
  text = text.simplified(); // FixAOLNickname
  return text.isEmpty() ? EditResult::Reject : EditResult::Accept;
}

// Right click (WM_CONTEXTMENU, 0x1128afec): a CreatePopupMenu() menu built per page and node.
void BuddyListWindow::showContextMenu(const QPoint &point) {
  auto label = [this](int id) { std::function<QString(const QList<MenuItem> &)> find = [&](const QList<MenuItem> &items) -> QString { for (const MenuItem &item : items) { if (item.id == id) return item.text.section(QLatin1Char('\t'), 0, 0); const QString inner = find(item.children); if (!inner.isEmpty()) return inner; } return {}; }; return find(menuBar_); };
  QList<MenuItem> items; auto add = [&](int id, const QString &text) { items.append(MenuItem{text, id}); }; auto separator = [&] { if (!items.isEmpty() && !items.last().isSeparator()) items.append(MenuItem{}); };
  const bool buddy = !selectedGroup_ && !selectedPending_ && selectedItemId_;
  const bool blocked = buddy && client_ && client_->isBlocked(selectedName_);
  auto buddyBlock = [&] { if (!buddy) return; separator(); add(152, aimString(152)); add(blocked ? 1011 : 390, aimString(blocked ? 1214 : 1213)); };
  if (!listSetup_) {
    if (buddy && client_ && client_->isOnline(selectedName_)) { add(138, aimString(157)); add(139, aimString(156)); add(561, aimString(686)); }
    buddyBlock();
  } else {
    add(AddGroup, label(AddGroup));
    add(AddBuddy, label(AddBuddy));
    add(664, label(664));
    add(Delete, label(Delete));
    if (selectedGroup_ && selectedGroupId_) add(1178, label(1178));
    buddyBlock();
    separator(); add(Preferences, label(Preferences));
  }
  if (items.isEmpty()) return;
  while (!items.isEmpty() && items.last().isSeparator()) items.removeLast();
  const int id = popupMenu(this, items, canvasToGlobal(point));
  if (id) command(id);
}
// AOL NetFind (0x11285bb1): RT_MENU 104 bottom-aligned at (button right + 5, button top + 7).
void BuddyListWindow::showNetFindMenu() {
  CtlObject *button = group_ ? group_->find(Find) : nullptr; if (!button) return;
  const QList<MenuItem> menu = loadMenuResource(104); if (menu.isEmpty()) return;
  const QRect r = button->windowRect();
  const int id = popupMenu(this, menu.first().children, canvasToGlobal(QPoint(r.right() + 1 + 5, r.top() + 7)), {}, true);
  if (id) command(id);
}
void BuddyListWindow::runHelpCommand(QWindow *owner, const QString &screenName, int command) {
  switch (command) {
  case 158: case 156: case 705: showHelp(owner, command); return;
  case 159: ate::openUrl(aimString(181) + QStringLiteral("?ver=beta&num=4.7.2480&type=WIN32&screenname=%1").arg(QString::fromLatin1(QUrl::toPercentEncoding(screenName)))); return; // LaunchReportBug
  case 902: ate::openUrl(aimString(1103)); return; // Frequently Asked Questions
  case 160: showAbout(owner); return;
  default: return;
  }
}
void BuddyListWindow::showHelp(QWindow *owner, int command) {
#ifdef Q_OS_WIN
  // aim95.hlp through WinHelp (buddyui 0x11286400): How to Use Help = HELP_HELPONHELP, Help Topics = HELP_FINDER,
  // Buddy List Help = HELP_CONTEXT 0x201. Current Windows versions no longer ship a WinHelp viewer.
  const std::wstring file = (QCoreApplication::applicationDirPath() + QLatin1Char('/') + aimString(182)).toStdWString();
  if (command == 158) WinHelpW(ownerHandle(owner), file.c_str(), HELP_HELPONHELP, 0);
  else if (command == 156) WinHelpW(ownerHandle(owner), file.c_str(), HELP_FINDER, 0);
  else WinHelpW(ownerHandle(owner), file.c_str(), HELP_CONTEXT, 0x201);
#else
  Q_UNUSED(owner); Q_UNUSED(command);
#endif
}
// Help > About (RT_DIALOG 111): static 203 = "Version <osclogin ProductVersion>\r\n" + "Copyright (c) ..." (0x112915c5).
void BuddyListWindow::showAbout(QWindow *owner) {
#ifdef Q_OS_WIN
  const QString version = aimString(876) + QStringLiteral(" 4.7.2480") + aimString(1774) + QStringLiteral("\r\n") + aimString(877) + QStringLiteral(" 1996-2001 America Online, Inc.");
  runOriginalDialog(ownerHandle(owner), 111, [&](HWND dialog) { SetDlgItemTextW(dialog, 203, reinterpret_cast<LPCWSTR>(version.utf16())); },
    [](HWND dialog, int id, int) { if (id == IDOK || id == IDCANCEL) { EndDialog(dialog, id); return true; } return false; });
#endif
}
void BuddyListWindow::showAwayMenu(const QRect &anchor) {
  MenuItem away; away.id = 0; away.children = {MenuItem{QStringLiteral("PLACEHOLDER"), 660}};
  const int id = popupMenu(this, preparedMenu(away), canvasToGlobal(anchor.bottomLeft() + QPoint(0, 1)));
  if (id) emit actionRequested(id, QString());
}

void BuddyListWindow::preferencesChanged() {
  if (group_) {
    ctlShowControl(*group_, Ticker, prefs::showStockTicker()); ctlShowControl(*group_, TickerCaption, prefs::showStockTicker());
    setResizable(canvasForOuter(minimumOuter(ctlIdealSize(*group_, aimEnvironment()))));
  }
  requestUpdate();
}
void BuddyListWindow::closeRequested() {
  if (prefs::signOffWhenClosed()) { emit actionRequested(745, QString()); return; } // "Sign off when Buddy List window is closed"
  hide(); showClosedNotice();
}
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
      if (item.id == 660) { // CreateAwayMenu (oscarui 0x12201a42): New Message..., separator, the messages
        result.append(MenuItem{aimString(282), 24000}); result.append(MenuItem{});
        const auto messages = AwayMessages::menuMessages();
        for (int i = 0; i < messages.size(); ++i) { MenuItem away; away.id = AwaySavedBase + i; away.text = QString(messages[i].first).replace(QLatin1Char('&'), QStringLiteral("&&")); result.append(away); }
        continue;
      }
      if (item.id == 181) item.checked = settings_.value(QStringLiteral("preferences/keepBuddyListOnTop"), false).toBool();
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
  if (command) this->command(command);
}
