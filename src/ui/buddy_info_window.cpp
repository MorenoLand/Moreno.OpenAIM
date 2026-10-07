#include "buddy_info_window.h"
#include "art.h"
#include "ctl_group.h"
#include "gdi_text.h"
#include "user_actions.h"
#include "native_dialog.h"
#include <QAbstractTextDocumentLayout>
#include <QDateTime>
#include <QGuiApplication>
#include <QIcon>
#include <QMoveEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QSettings>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
enum : int { ComboId = 0x10, OkId = 0x11, CloseId = 2, ImId = 0x14, AddBuddyId = 0x15, DirectoryId = 0x16 };
QString aimString(quint32 id) { return aimEnvironment().string(id); }
// CSysData::GetFont (wndutils 0x12288190): cell height 14; 0 = MS Sans Serif, 1 = default face, 2 = default face bold.
const GdiFont LabelFont{QStringLiteral("MS Sans Serif"), 14}, ValueFont{QStringLiteral("MS Sans Serif"), 14}, BoldFont{QStringLiteral("MS Sans Serif"), 14, 700};
QList<QPointer<BuddyInfoWindow>> &windows() { static QList<QPointer<BuddyInfoWindow>> list; return list; }
QString normalized(QString name) { name.remove(QLatin1Char(' ')); return name.toCaseFolded(); }
// o_FormatIdleTime (oscore 0x1218b534): "3 days, 9 hours, 42 minutes", zero parts skipped, 0 minutes shown as 1.
QString formatMinutes(quint64 minutes) {
  if (!minutes) minutes = 1;
  const quint64 days = minutes / 1440, hours = (minutes / 60) % 24, rest = minutes % 60; QStringList parts;
  if (days) parts << QString(aimString(days == 1 ? 265 : 266)).replace(QStringLiteral("%u"), QString::number(days));
  if (hours) parts << QString(aimString(hours == 1 ? 263 : 264)).replace(QStringLiteral("%u"), QString::number(hours));
  if (rest) parts << QString(aimString(rest == 1 ? 261 : 262)).replace(QStringLiteral("%u"), QString::number(rest));
  return parts.join(aimString(267));
}
#ifdef Q_OS_WIN
QSize systemTextSize(const QString &text) { HDC dc = GetDC(nullptr); SIZE size{}; GetTextExtentPoint32W(dc, reinterpret_cast<LPCWSTR>(text.utf16()), int(text.size()), &size); ReleaseDC(nullptr, dc); return QSize(size.cx, size.cy); }
QString windowText(void *window) { HWND hwnd = static_cast<HWND>(window); const int length = GetWindowTextLengthW(hwnd); std::vector<wchar_t> buffer(size_t(length) + 1); GetWindowTextW(hwnd, buffer.data(), length + 1); return QString::fromWCharArray(buffer.data()); }
#endif
QSize canvasForOuter(const QSize &outer) {
#ifdef Q_OS_WIN
  RECT frame{0, 0, 0, 0}; AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
  return QSize(outer.width() - (frame.right - frame.left) + 2, outer.height() - (frame.bottom - frame.top) + 24 + 3);
#else
  return outer;
#endif
}
}

void BuddyInfoWindow::open(OscarClient *client, const QString &screenName, const Action &action) {
  // One window per screen name (0x11483607): an existing one is re-requested and brought to the front.
  windows().removeIf([](const QPointer<BuddyInfoWindow> &window) { return window.isNull(); });
  if (!screenName.trimmed().isEmpty()) for (const auto &window : windows()) if (normalized(window->name_) == normalized(screenName)) { window->showNormal(); window->raise(); window->requestActivate(); window->request(false); return; }
  auto *window = new BuddyInfoWindow(client, screenName.trimmed(), action);
  windows().append(window);
  window->show(); window->requestActivate();
  if (!window->name_.isEmpty()) QTimer::singleShot(0, window, [window] { window->request(false); });
}

BuddyInfoWindow::BuddyInfoWindow(OscarClient *client, const QString &screenName, const Action &action) : WindowBase(QString(), QSize(300, 150)), client_(client), action_(action), name_(screenName) {
  setIcon(QIcon(QStringLiteral(":/aim/icons/111")));
  profile_.setDocumentMargin(2); QFont font(QStringLiteral("Times New Roman")); font.setPixelSize(16); profile_.setDefaultFont(font);
  // WM_CREATE metrics (0x11483222): label size, button size from the longest of OK/Close/Cancel in the default font.
  const QSize label = gdiTextSize(LabelFont, aimString(558), GdiNoPrefix); labelW_ = label.width(); labelH_ = label.height();
#ifdef Q_OS_WIN
  int textW = 0, textH = 0; for (quint32 id : {553u, 554u, 555u}) { QString text = aimString(id); text.remove(QLatin1Char('&')); const QSize size = systemTextSize(text); textW = std::max(textW, size.width()); textH = std::max(textH, size.height()); }
  buttonW_ = textW + 14; buttonH_ = textH + 10;
  TEXTMETRICW metrics{}; HDC dc = GetDC(nullptr); GetTextMetricsW(dc, &metrics); ReleaseDC(nullptr, dc); avgCharW_ = metrics.tmAveCharWidth; charH_ = metrics.tmHeight;
  initialOuter_ = QSize(buttonW_ + labelW_ + 60, GetSystemMetrics(SM_CYCAPTION) + 2 * buttonH_ + 60);
#else
  buttonW_ = 60; buttonH_ = 23; initialOuter_ = QSize(buttonW_ + labelW_ + 60, 19 + 2 * buttonH_ + 60);
#endif
  setCanvasSize(canvasForOuter(initialOuter_));
  // Saved "LocateMain" position, otherwise centred on the screen (0x114842d8).
  const QRect saved = QSettings().value(QStringLiteral("windows/LocateMain")).toRect();
  if (saved.isValid()) setFramePosition(saved.topLeft());
  else if (QScreen *screen = QGuiApplication::primaryScreen()) setFramePosition(screen->geometry().center() - QPoint(initialOuter_.width() / 2, initialOuter_.height() / 2));
  buttons_ = {{ImId, {160, 161, 162}, {}}, {AddBuddyId, {163, 164, 165}, {}}, {DirectoryId, {175, 177, 176}, {}}};
  refresh_.setInterval(300000); // timer 0xA0: silent refresh every 5 minutes
  connect(&refresh_, &QTimer::timeout, this, [this] { if (state_ == State::Shown || state_ == State::Error) request(true); });
  connect(client_, &OscarClient::userInfoReceived, this, [this](const aim::oscar::UserInfo &info) { if (state_ == State::Waiting && normalized(info.screenName) == normalized(name_)) showReply(info); });
  connect(client_, &OscarClient::operationFailed, this, [this](const QString &operation, const QString &reason) { if (state_ == State::Waiting && operation.compare(QStringLiteral("Get Info for %1").arg(name_), Qt::CaseInsensitive) == 0) showError(reason); });
  updateTitle();
#ifdef Q_OS_WIN
  QGuiApplication::instance()->installNativeEventFilter(this);
#endif
}
BuddyInfoWindow::~BuddyInfoWindow() {
#ifdef Q_OS_WIN
  QGuiApplication::instance()->removeNativeEventFilter(this);
  for (void *control : {combo_, ok_, close_}) if (control && IsWindow(static_cast<HWND>(control))) DestroyWindow(static_cast<HWND>(control));
#endif
}

QRect BuddyInfoWindow::clientRect() const {
#ifdef Q_OS_WIN
  return QRect(1, TitleBarHeight, width(), height());
#else
  return QRect(0, TitleBarHeight, width(), height() - TitleBarHeight);
#endif
}
void BuddyInfoWindow::updateTitle() { setTitle(formatAimString(aimString(559), {name_})); }
QString BuddyInfoWindow::currentName() const {
#ifdef Q_OS_WIN
  return combo_ ? windowText(combo_).trimmed() : name_;
#else
  return name_;
#endif
}

bool BuddyInfoWindow::event(QEvent *event) {
  if (event->type() == QEvent::Expose && isExposed()) createControls();
  return WindowBase::event(event);
}
void BuddyInfoWindow::createControls() {
#ifdef Q_OS_WIN
  if (combo_) return;
  HWND owner = reinterpret_cast<HWND>(winId());
  SetWindowLongPtrW(owner, GWL_STYLE, GetWindowLongPtrW(owner, GWL_STYLE) | WS_CLIPCHILDREN);
  // No WM_SETFONT anywhere in locateui: the combo and buttons keep the default (System) font.
  combo_ = CreateWindowExW(0, L"COMBOBOX", reinterpret_cast<LPCWSTR>(name_.utf16()), WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_DISABLENOSCROLL, 0, 0, 0, charH_ * 7, owner, reinterpret_cast<HMENU>(ComboId), GetModuleHandleW(nullptr), nullptr);
  ok_ = CreateWindowExW(0, L"BUTTON", reinterpret_cast<LPCWSTR>(aimString(553).utf16()), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, owner, reinterpret_cast<HMENU>(OkId), GetModuleHandleW(nullptr), nullptr);
  close_ = CreateWindowExW(0, L"BUTTON", reinterpret_cast<LPCWSTR>(aimString(554).utf16()), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, owner, reinterpret_cast<HMENU>(CloseId), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(static_cast<HWND>(combo_), CB_LIMITTEXT, 32, 0);
  for (const QString &recent : QSettings().value(QStringLiteral("Locate/recentScreenNames")).toStringList()) SendMessageW(static_cast<HWND>(combo_), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(recent.utf16())); // STRING 566 "recent ScreenNames"
  SetWindowTextW(static_cast<HWND>(combo_), reinterpret_cast<LPCWSTR>(name_.utf16()));
  layoutControls();
  SetFocus(static_cast<HWND>(combo_));
#endif
}
void BuddyInfoWindow::layoutControls() {
#ifdef Q_OS_WIN
  if (!combo_) return;
  const int clientW = width();
  // OnSize (3.3): combo at (5, labelH + 15), OK and Close stacked at the right edge.
  const int comboW = std::min(avgCharW_ * 32, clientW - buttonW_ - 0x23);
  SetWindowPos(static_cast<HWND>(combo_), nullptr, 5, labelH_ + 15, comboW, charH_ * 7, SWP_NOZORDER | SWP_NOACTIVATE);
  SetWindowPos(static_cast<HWND>(ok_), nullptr, clientW - buttonW_ - 10, 10, buttonW_, buttonH_, SWP_NOZORDER | SWP_NOACTIVATE);
  SetWindowPos(static_cast<HWND>(close_), nullptr, clientW - buttonW_ - 10, buttonH_ + 15, buttonW_, buttonH_, SWP_NOZORDER | SWP_NOACTIVATE);
#endif
}
int BuddyInfoWindow::panelTop() const {
  int comboHeight = 21;
#ifdef Q_OS_WIN
  if (combo_) { RECT r{}; GetWindowRect(static_cast<HWND>(combo_), &r); comboHeight = r.bottom - r.top; }
#endif
  return std::max(2 * buttonH_ + 20, comboHeight + labelH_ + 15) + 5;
}
QString BuddyInfoWindow::onlineText() const {
  if (!info_.signOnTime) return {};
  const qint64 seconds = std::max<qint64>(0, QDateTime::currentSecsSinceEpoch() - qint64(info_.signOnTime));
  return formatMinutes(quint64(seconds / 60));
}

void BuddyInfoWindow::paintContent(QPainter &p) {
  const QRect client = clientRect(); const QPoint o = client.topLeft();
  p.fillRect(client, art::Face);
  drawGdiText(p, QRect(o + QPoint(5, 10), QSize(labelW_, labelH_)), aimString(558), LabelFont, Qt::black, art::Face, GdiNoPrefix);
  if (!panel_) return;
  const int top = panelTop(), clientW = width(), clientH = height();
  // Panel (0x114848c1): rules above and below three info lines, the profile label, the ATE and the art-button strip.
  auto rule = [&](int y) { p.setPen(art::Highlight); p.drawLine(o.x(), o.y() + y, o.x() + clientW - 1, o.y() + y); p.setPen(art::Shadow); p.drawLine(o.x(), o.y() + y + 1, o.x() + clientW - 1, o.y() + y + 1); };
  rule(top - 2);
  const int lineH = std::max(gdiTextSize(BoldFont, QStringLiteral("Wg")).height(), gdiTextSize(ValueFont, QStringLiteral("Wg")).height());
  auto line = [&](int y, const QString &label, const QString &value) {
    const int labelW = gdiTextSize(BoldFont, label, GdiSingleLine | GdiNoPrefix).width();
    drawGdiText(p, QRect(o.x() + 5, o.y() + y, labelW, lineH), label, BoldFont, Qt::black, art::Face, GdiSingleLine | GdiVCenter | GdiNoPrefix);
    if (!value.isEmpty()) drawGdiText(p, QRect(o.x() + 5 + labelW + 4, o.y() + y, clientW - labelW - 20, lineH), value, ValueFont, Qt::black, art::Face, GdiSingleLine | GdiVCenter | GdiNoPrefix);
  };
  const int y1 = top + 5, y2 = y1 + lineH + 5, y3 = y2 + lineH + 5;
  const bool shown = state_ == State::Shown;
  if (shown) {
    line(y1, aimString(295), info_.warningLevel == 0xffff ? QString() : QStringLiteral("%1%").arg(MulDiv(info_.warningLevel, 100, 999)));
    line(y2, aimString(298), onlineText());
    const bool away = info_.flags & 0x20;
    if (info_.idleMinutes) line(y3, aimString(away ? 761 : 296), formatMinutes(info_.idleMinutes)); else if (away) line(y3, aimString(762), QString());
  }
  rule(y3 + lineH + 2);
  const int yEnd = y3 + lineH + 5;
  const QSize profileLabel = gdiTextSize(BoldFont, aimString(565), GdiSingleLine | GdiNoPrefix);
  drawGdiText(p, QRect(o + QPoint(5, yEnd + 2), profileLabel), aimString(565), BoldFont, Qt::black, art::Face, GdiSingleLine | GdiNoPrefix);
  // Art-button strip: equal gaps, the hidden &Chat slot keeps its 5 px place (3.4/3.5).
  const int stripTop = clientH - 39; QList<int> widths; for (const Button &b : buttons_) widths << art::image(b.art[0]).width() + 5;
  const int itemsW = widths[0] + widths[1] + 5 + widths[2], gap = (clientW - 10 - itemsW) / 5;
  int x = 5 + gap; QList<int> xs; for (int i = 0; i < buttons_.size(); ++i) { xs << x; x += widths[i] + gap; if (i == 1) x += 5 + gap; }
  for (int i = 0; i < buttons_.size(); ++i) {
    Button &b = buttons_[i]; const QImage normal = art::image(b.art[0]);
    b.rect = QRect(o.x() + xs[i], o.y() + stripTop + (39 - (normal.height() + 5)) / 2, normal.width() + 5, normal.height() + 5);
    const int state = pressed_ == b.id && hovered_ == b.id ? 2 : hovered_ == b.id ? 1 : 0;
    p.drawImage(b.rect.topLeft() + QPoint(2, 2), art::image(b.art[state]));
  }
  // Profile ATE: below the label down to 5 px above the button strip.
  const QRect ate(o.x() + 5, o.y() + yEnd + profileLabel.height() + 5, clientW - 10, std::max(10, stripTop - (yEnd + profileLabel.height() + 5) - 5));
  p.fillRect(ate, Qt::white); art::drawSunken(p, ate);
  const QRect view = ate.adjusted(3, 3, -3, -3); profile_.setTextWidth(view.width());
  p.save(); p.setClipRect(view); p.translate(view.topLeft());
  QAbstractTextDocumentLayout::PaintContext context; context.clip = QRectF(0, 0, view.width(), view.height()); context.palette.setColor(QPalette::Text, Qt::black);
  profile_.documentLayout()->draw(&p, context); p.restore();
}

void BuddyInfoWindow::request(bool silent) {
  const QString name = currentName();
  if (!client_ || !client_->connected()) { if (!silent) userActions::infoBox(this, aimString(696)); return; }
  if (name.isEmpty() || name.size() > 32) { if (!silent) userActions::infoBox(this, aimString(569)); return; }
  name_ = name; updateTitle(); errorShown_ = silent && errorShown_;
  QSettings settings; QStringList recent = settings.value(QStringLiteral("Locate/recentScreenNames")).toStringList(); recent.removeIf([&](const QString &value) { return normalized(value) == normalized(name); }); recent.prepend(name); while (recent.size() > 10) recent.removeLast(); settings.setValue(QStringLiteral("Locate/recentScreenNames"), recent);
  expandPanel();
  refresh_.stop();
  if (!silent) { profile_.setPlainText(aimString(560)); state_ = State::Waiting; setCloseMode(false); }
  else state_ = State::Waiting;
#ifdef Q_OS_WIN
  if (ok_) EnableWindow(static_cast<HWND>(ok_), FALSE);
#endif
  requestUpdate();
  client_->requestUserInfo(name);
}
void BuddyInfoWindow::expandPanel() {
  if (panel_) return;
  // First lookup (0x11484391): the window becomes resizable, 380 wide and 150 px taller; min track (W0, 310).
  panel_ = true;
  setResizable(canvasForOuter(QSize(initialOuter_.width(), 310)));
  const QRect saved = QSettings().value(QStringLiteral("windows/LocateMain")).toRect();
  setCanvasSize(canvasForOuter(saved.isValid() ? saved.size() : QSize(380, initialOuter_.height() + 150)));
  layoutControls();
}
void BuddyInfoWindow::preview(OscarClient *client) {
  auto *window = new BuddyInfoWindow(client, QStringLiteral("ExampleBuddy"), {}); windows().append(window); window->show();
  aim::oscar::UserInfo info; info.screenName = QStringLiteral("ExampleBuddy"); info.signOnTime = quint32(QDateTime::currentSecsSinceEpoch() - (3 * 86400 + 9 * 3600 + 42 * 60));
  window->expandPanel(); window->showReply(info);
}
void BuddyInfoWindow::showReply(const aim::oscar::UserInfo &info) {
  info_ = info; received_.start(); state_ = State::Shown;
#ifdef Q_OS_WIN
  if (combo_) SetWindowTextW(static_cast<HWND>(combo_), reinterpret_cast<LPCWSTR>(info.screenName.utf16()));
#endif
  // Away text in red followed by a rule, then the profile (STRING 561 / 562 when empty).
  QString html;
  if ((info.flags & 0x20) && !info.away.isEmpty()) html += QStringLiteral("<FONT COLOR=\"#ff0000\">%1</FONT><HR>").arg(info.away);
  html += info.profile.trimmed().isEmpty() ? aimString((info.flags & 0x04) ? 562 : 561).toHtmlEscaped() : info.profile;
  profile_.setHtml(html);
  setCloseMode(true); refresh_.start(); requestUpdate();
}
void BuddyInfoWindow::showError(const QString &reason) {
  state_ = State::Error; setCloseMode(true); refresh_.start();
  const int code = [&] { const int at = reason.indexOf(QStringLiteral("(0x")); return at < 0 ? -1 : reason.mid(at + 3, 4).toInt(nullptr, 16); }();
  if (code == 4) profile_.setHtml(formatAimString(aimString(570), {name_.toHtmlEscaped()}));
  else { profile_.clear(); if (!errorShown_) { errorShown_ = true; userActions::infoBox(this, aimString(code == 5 ? 575 : code == 0x0f ? 572 : 571)); } }
  requestUpdate();
}
void BuddyInfoWindow::setCloseMode(bool close) {
  closeMode_ = close;
#ifdef Q_OS_WIN
  if (close_) SetWindowTextW(static_cast<HWND>(close_), reinterpret_cast<LPCWSTR>(aimString(close ? 554 : 555).utf16()));
#endif
}
void BuddyInfoWindow::cancel() { refresh_.stop(); state_ = State::Idle; profile_.clear(); setCloseMode(true); requestUpdate(); }
void BuddyInfoWindow::nameEdited() {
  refresh_.stop();
#ifdef Q_OS_WIN
  if (ok_) EnableWindow(static_cast<HWND>(ok_), TRUE);
#endif
}

bool BuddyInfoWindow::nativeEventFilter(const QByteArray &, void *message, qintptr *result) {
#ifdef Q_OS_WIN
  if (!combo_ || !handle()) return false;
  MSG *msg = static_cast<MSG *>(message); HWND owner = reinterpret_cast<HWND>(winId());
  if (msg->message == WM_COMMAND && msg->hwnd == owner) {
    const int id = LOWORD(msg->wParam), notice = HIWORD(msg->wParam);
    if (id == OkId && notice == BN_CLICKED) { SendMessageW(static_cast<HWND>(combo_), CB_SHOWDROPDOWN, FALSE, 0); request(false); if (result) *result = 0; return true; }
    if (id == CloseId && notice == BN_CLICKED) { if (closeMode_) closeRequested(); else cancel(); if (result) *result = 0; return true; }
    if (id == ComboId && (notice == CBN_EDITCHANGE || notice == CBN_SELENDOK)) { nameEdited(); return false; }
  }
  const bool owns = msg->hwnd == owner || IsChild(owner, msg->hwnd);
  if (owns && msg->message == WM_KEYDOWN) {
    if (msg->wParam == VK_RETURN) { request(false); if (result) *result = 0; return true; }
    if (msg->wParam == VK_ESCAPE) { closeRequested(); if (result) *result = 0; return true; }
  }
#else
  Q_UNUSED(message); Q_UNUSED(result);
#endif
  return false;
}
void BuddyInfoWindow::contentMousePress(const QPoint &point, Qt::MouseButton button) { if (button != Qt::LeftButton) return; for (const Button &b : buttons_) if (panel_ && b.rect.contains(point)) { pressed_ = b.id; requestUpdate(); return; } }
void BuddyInfoWindow::contentMouseRelease(const QPoint &point, Qt::MouseButton button) {
  if (button != Qt::LeftButton || !pressed_) return;
  const int id = pressed_; pressed_ = 0; requestUpdate();
  for (const Button &b : buttons_) if (b.id == id && b.rect.contains(point)) {
    const QString name = currentName(); if (name.isEmpty()) return;
    if (id == ImId && action_) action_(139, name);
    else if (id == AddBuddyId) userActions::addBuddy(this, client_, name);
    // Directory Info goes to OscSrch's directory lookup, which OpenAIM does not have.
  }
}
void BuddyInfoWindow::contentMouseMove(const QPoint &point) { int hovered = 0; for (const Button &b : buttons_) if (panel_ && b.rect.contains(point)) hovered = b.id; if (hovered != hovered_) { hovered_ = hovered; requestUpdate(); } }
void BuddyInfoWindow::contentLeave() { if (hovered_) { hovered_ = 0; requestUpdate(); } }
void BuddyInfoWindow::closeRequested() { hide(); deleteLater(); }
void BuddyInfoWindow::resizeEvent(QResizeEvent *event) { WindowBase::resizeEvent(event); layoutControls(); if (panel_ && isVisible() && visibility() == QWindow::Windowed) QSettings().setValue(QStringLiteral("windows/LocateMain"), QRect(framePosition(), frameGeometry().size())); }
void BuddyInfoWindow::moveEvent(QMoveEvent *event) { WindowBase::moveEvent(event); if (panel_ && isVisible() && visibility() == QWindow::Windowed) QSettings().setValue(QStringLiteral("windows/LocateMain"), QRect(framePosition(), frameGeometry().size())); }
