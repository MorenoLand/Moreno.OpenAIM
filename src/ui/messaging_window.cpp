#include "messaging_window.h"
#include "ate_link.h"
#include "preferences.h"
#include "../oscar/direct_connection.h"
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QtEndian>
#include "native_dialog.h"
#include "ate_toolbar.h"
#include <QUrl>
#include <QCursor>
#include "ctl_group.h"
#include "menu_bar.h"
#include "sounds.h"
#include "ate_toolbar.h"
#include "buddy_info_window.h"
#include "user_actions.h"
#include "text_editor.h"
#include "window_base.h"
#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QDateTime>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QList>
#include <QLocale>
#include <QMoveEvent>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QWheelEvent>
#include <functional>
#include <limits>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Instant Message window of icbmui.ocm (Research/im_window.md): CTLGROUP 103 laid out by the ported CTLGROUP engine,
// RT_MENU 101, modes from SetMode 0x113908db and history formatting from AppendMsg 0x1138fe5c.
namespace {
enum : quint32 { ToRow = 0x191, ToCombo = 0x192, History = 0x193, Compose = 0x194, Warn = 0x195, Block = 0x196, AddBuddy = 0x197, BuddyIcon = 0x198, Send = 0x199, RateMeter = 0x19a, Talk = 0x12, GetInfo = 0x16f, WarnSeparator = 0x2b7 };
enum Mode { NewMessage = 0x11, WithRecipient = 1, Conversation = 2 };
constexpr int ToolbarHeight = AteToolbar::Height, SplitterBand = 4, HistoryMinimum = 20, ComposeMinimum = 40;
const QColor Face(240, 240, 240), Shadow(160, 160, 160), Highlight(255, 255, 255), PaneBorder(130, 135, 144);
QString aimString(quint32 id) { return aimEnvironment().string(id); }
QFont ateFont(bool bold = false) { QFont font(QStringLiteral("Times New Roman")); font.setPixelSize(16); font.setBold(bold); return font; } // AIM default IM font: Times New Roman 12 pt
QFont controlFont() { return CtlEnvironment::font(106); }
// ATE panes are black text on a white window (WindowColor default 0xFFFFFF), whatever the system colour scheme is.
QPalette atePalette() { QPalette palette; palette.setColor(QPalette::Text, Qt::black); palette.setColor(QPalette::WindowText, Qt::black); palette.setColor(QPalette::Base, Qt::white); palette.setColor(QPalette::Link, QColor(0, 0, 255)); return palette; }
QString normalizedName(QString name) { name.remove(QLatin1Char(' ')); return name.toCaseFolded(); }
bool isHtml(const QString &text) { return text.contains(QRegularExpression(QStringLiteral("</?[A-Za-z!][^>]*>"))); }

// History pane: AppendMsg header "<b>Name</b>:" in red (self) or blue (buddy), "<br>" before every later message.
class Transcript {
public:
  Transcript() { document.setDefaultFont(ateFont()); document.setDocumentMargin(2); }
  void appendMessage(const QString &name, const QString &body, bool self) {
    const QString color = self ? QStringLiteral("#ff0000") : QStringLiteral("#0000ff");
    const QString separator = body.trimmed().isEmpty() ? QStringLiteral(".") : QStringLiteral(":");
    // Preferences > IM/Chat "Always view timestamp": "Name (h:mm:ss AM):".
    const QString stamp = prefs::alwaysTimestamp() ? QStringLiteral(" (%1)").arg(QLocale::system().toString(QTime::currentTime(), QStringLiteral("h:mm:ss AP"))) : QString();
    QTextCursor cursor(&document); cursor.movePosition(QTextCursor::End);
    if (!first_) cursor.insertBlock();
    cursor.insertHtml(QStringLiteral("<font color=%1><b>%2</b>%3%4</font>&nbsp;").arg(color, name.toHtmlEscaped(), stamp.toHtmlEscaped(), separator));
    const int start = cursor.position();
    cursor.insertHtml(body); first_ = false; followBottom = true;
    ate::insertSmileys(document, start);
  }
  void appendNotice(const QString &html) { QTextCursor cursor(&document); cursor.movePosition(QTextCursor::End); if (!first_) cursor.insertBlock(); cursor.insertHtml(QStringLiteral("<hr>") + html); first_ = false; followBottom = true; }
  QTextDocument document;
  qreal scroll = 0;
  bool followBottom = true;
private:
  bool first_ = true;
};

QImage artImage(quint32 id) {
  static QHash<quint32, QImage> cache; auto it = cache.find(id); if (it != cache.end()) return *it;
  // The artwork colour key is the bitmap's top-left pixel (magenta for most art, grey for the Talk art).
  QImage image = QImage(QStringLiteral(":/aim/art/%1").arg(id)).convertToFormat(QImage::Format_ARGB32);
  if (!image.isNull()) { const QRgb key = image.pixel(0, 0) & 0x00ffffffu; for (int y = 0; y < image.height(); ++y) { auto *line = reinterpret_cast<QRgb *>(image.scanLine(y)); for (int x = 0; x < image.width(); ++x) if ((line[x] & 0x00ffffffu) == key) line[x] = 0; } }
  cache.insert(id, image); return image;
}
QImage disabledImage(const QImage &source) {
  QImage image = source.convertToFormat(QImage::Format_ARGB32);
  for (int y = 0; y < image.height(); ++y) { auto *line = reinterpret_cast<QRgb *>(image.scanLine(y)); for (int x = 0; x < image.width(); ++x) { const int gray = qGray(line[x]); const int value = 128 + gray / 2; line[x] = qRgba(value, value, value, qAlpha(line[x])); } }
  return image;
}
void drawEtched(QPainter &p, const QRect &r) {
  // CtlGroupPaintBackground frame for groups with flag 0x20 (0x12204c94).
  const int L = r.left(), T = r.top(), R = r.left() + r.width(), B = r.top() + r.height();
  p.setPen(Shadow); p.drawLine(L, B, L, T); p.drawLine(L, T, R - 1, T); p.drawLine(R - 1, T, R - 1, B - 1); p.drawLine(R - 1, B - 1, L, B - 1);
  p.setPen(Highlight); p.drawLine(L + 1, B - 2, L + 1, T + 1); p.drawLine(L + 1, T + 1, R - 1, T + 1); p.drawLine(R, T, R, B); p.drawLine(R, B, L - 1, B);
}

class MessageWindow final : public WindowBase {
public:
  MessageWindow(OscarClient *client, QWindow *transientParent, QObject *owner, const QString &recipient, int cascade)
      : WindowBase(QStringLiteral("Instant Message"), defaultCanvas()), client_(client), recipient_(QFont(QStringLiteral("MS Sans Serif"), 8)), compose_(prefs::composeFont()), menu_(101), group_(loadCtlGroup(103)) {
    QObject::setParent(owner); Q_UNUSED(transientParent);
    compose_.cursor.setCharFormat(prefs::composeFormat()); // Preferences > IM/Chat > Defaults for Composing Windows
    typingTimer_.setSingleShot(true); typingTimer_.setInterval(5000);
    QObject::connect(&typingTimer_, &QTimer::timeout, owner, [this] { if (directConnected() && lastTyping_ == 0x0E) { direct_->sendTyping(0x06); lastTyping_ = 0x06; } });
    setResizable(defaultCanvas());
    recipient_.setText(recipient);
    setMode(recipient.trimmed().isEmpty() ? NewMessage : WithRecipient);
    focus_ = recipient.trimmed().isEmpty() ? 0 : 1;
    const QRect saved = QSettings().value(QStringLiteral("windows/MessageMain")).toRect();
    setPosition(saved.isValid() ? saved.topLeft() + QPoint(16, 20) * cascade : QPoint(80 + 16 * cascade, 40 + 20 * cascade)); // cascade +16/+20 (0x1138cde0)
    if (saved.isValid()) resize(saved.size());
  }
  std::function<void(const QString &)> recipientChanged;
  std::function<void(MessageWindow *)> sendStarted;
  std::function<void(MessageWindow *)> sendFinished;
  std::function<void(const QString &)> openMessage;
  std::function<void(const QString &)> inviteToChat;
  std::function<void(MessageWindow *)> connectImage; // People > Connect to Send IM Image (818) / toolbar cell
  std::function<void(MessageWindow *)> closeImage;   // People > Close IM Image Connection (819)
  void setDirect(DirectConnection *connection) {
    direct_ = connection; toolbar_.setImageConnected(directConnected()); peerTyping_ = 0; lastTyping_ = 0; updateTitle(); requestUpdate();
  }
  bool directConnected() const { return direct_ && direct_->isConnected(); }
  void appendDirect(const QString &sender, const QByteArray &payload, quint16 encoding, quint8 flags) {
    appendIncoming(sender, ate::directHtml(payload, encoding, transcript_.document), flags & DirectConnection::AutoResponse);
  }
  void setPeerTyping(quint8 flags) { peerTyping_ = flags; requestUpdate(); }
  QString recipient() const { return recipient_.text().trimmed(); }
  QString pendingRecipient() const { return pendingRecipient_; }
  bool isSending() const { return sending_; }
  bool acknowledge(const QString &recipient) {
    if (!sending_ || normalizedName(recipient) != normalizedName(pendingRecipient_)) return false;
    transcript_.appendMessage(client_ ? client_->screenName() : QString(), pendingText_, true);
    pendingText_.clear(); pendingRecipient_.clear(); sending_ = false; setMode(Conversation); requestUpdate(); return true;
  }
  void sendFailed(const QString &reason) { if (!sending_) return; restorePending(); appendFailure(reason); requestUpdate(); }
  void recordOperationFailure(const QString &message) { if (sendAttempt_) attemptError_ = message; else if (sending_) sendFailed(message); }
  void appendPresenceNotice(const QString &text) { transcript_.appendNotice(text.toHtmlEscaped()); if (mode_ == Conversation) requestUpdate(); }
  bool hasConversation() const { return mode_ == Conversation; }
  void resetPending(const QString &reason = QString()) { if (!sending_) return; restorePending(); if (!reason.isEmpty()) appendFailure(reason); requestUpdate(); }
  void appendIncoming(const QString &sender, const QString &text, bool autoResponse = false) { transcript_.appendMessage(autoResponse ? QString(aimString(521)).replace(QStringLiteral("%s"), sender) : sender, isHtml(text) ? text : text.toHtmlEscaped(), false); // STRING 521 "Auto response from %s"
    setMode(Conversation); requestUpdate(); }
  void showWindow() { show(); raise(); requestActivate(); }
  void preview() { // developer preview: a short conversation and some compose text
    transcript_.appendMessage(QStringLiteral("Edward"), QStringLiteral("<HTML><BODY>Hey, are you there?</BODY></HTML>"), false);
    transcript_.appendMessage(client_ ? client_->screenName() : QStringLiteral("denveous"), aimHtml([] { static QTextDocument d; d.setPlainText(QStringLiteral("Yes, I am here.")); return std::cref(d); }().get()), true);
    compose_.setText(QStringLiteral("Typing a reply")); setMode(Conversation); requestUpdate();
  }
protected:
  bool event(QEvent *event) override {
    if (event->type() == QEvent::InputMethod) { auto *input = static_cast<QInputMethodEvent *>(event); if (!input->commitString().isEmpty()) { editor().input(input->commitString(), focus_ == 0); edited(); } event->accept(); return true; }
    return WindowBase::event(event);
  }
  void moveEvent(QMoveEvent *event) override { WindowBase::moveEvent(event); if (isVisible() && visibility() == QWindow::Windowed) QSettings().setValue(QStringLiteral("windows/MessageMain"), QRect(position(), size())); } // SaveWindowPos "MessageMain" on WM_MOVE
  void resizeEvent(QResizeEvent *event) override { WindowBase::resizeEvent(event); if (isVisible() && visibility() == QWindow::Windowed) QSettings().setValue(QStringLiteral("windows/MessageMain"), QRect(position(), size())); }
  void closeRequested() override { hide(); deleteLater(); }
  void paintContent(QPainter &p) override {
    layout();
    p.fillRect(client(), Face);
    menu_.paint(p, menuRects_, hoveredMenu_, openMenu_);
    if (directConnected() && !menuRects_.isEmpty()) { // typing state of the buddy, right of the menu bar (0x1138e76a): STRING 1100/1099/1098/1101
      const quint32 id = (peerTyping_ & DirectConnection::Recording) ? 1100 : (peerTyping_ & DirectConnection::Typing) ? 1099 : (peerTyping_ & DirectConnection::Typed) ? 1098 : 1101;
      const QRect last = menuRects_.last(); const QRect area(last.right() + 8, last.top(), canvasWidth() - last.right() - 16, last.height());
      if (area.width() > 20) { p.setPen(QColor(64, 64, 64)); p.setFont(QFont(QStringLiteral("MS Sans Serif"), 8)); p.drawText(area, Qt::AlignRight | Qt::AlignVCenter, QFontMetrics(p.font()).elidedText(aimString(id), Qt::ElideRight, area.width())); }
    }
    std::function<void(CtlObject &)> paint = [&](CtlObject &o) {
      if (!o.shown()) return;
      const QRect r = o.windowRect();
      switch (o.kind) {
      case CtlObject::Kind::Group: case CtlObject::Kind::TabGroup:
        if (o.flags & CtlObject::Padding) drawEtched(p, r);
        for (const auto &child : o.children) paint(*child);
        return;
      case CtlObject::Kind::Static: p.setFont(controlFont()); p.setPen(Qt::black); p.drawText(r.adjusted(0, -2, 20, 2), Qt::AlignLeft | Qt::AlignVCenter, aimString(o.textId)); return;
      case CtlObject::Kind::PersistentCombo: paintCombo(p, r); return;
      case CtlObject::Kind::Ate: paintAte(p, o.id == History ? historyRect_ : composeRect_, o.id == Compose); return;
      case CtlObject::Kind::ArtButton: paintButton(p, o); return;
      case CtlObject::Kind::Separator: if (r.height() > 2) { p.setPen(Shadow); p.drawLine(r.left(), r.top(), r.left(), r.bottom()); p.setPen(Highlight); p.drawLine(r.left() + 1, r.top(), r.left() + 1, r.bottom()); } else { p.setPen(Shadow); p.drawLine(r.left(), r.top(), r.right(), r.top()); p.setPen(Highlight); p.drawLine(r.left(), r.top() + 1, r.right(), r.top() + 1); } return;
      case CtlObject::Kind::RateMeter: paintRateMeter(p, r); return;
      default: return;
      }
    };
    if (group_) paint(*group_);
  }
  void contentMousePress(const QPoint &point, Qt::MouseButton button) override {
    if (button != Qt::LeftButton) return;
    layout();
    if (const int menu = PaintedMenuBar::hit(menuRects_, point); menu >= 0) { openMenu(menu); return; }
    if (mode_ == Conversation && splitterBand().contains(point)) { draggingSplitter_ = true; return; }
    if (CtlObject *combo = group_ ? group_->find(ToCombo) : nullptr; combo && combo->shown() && comboRect(combo->windowRect()).contains(point)) {
      const QRect field = comboRect(combo->windowRect());
      if (point.x() >= field.right() - 17) { showRecentNames(field); return; }
      focus_ = 0; placeCursor(recipient_, field.adjusted(3, 0, -18, 0), point, true); requestUpdate(); return;
    }
    if (const int tool = toolbar_.hit(point); tool >= 0 && composeRect_.contains(point)) { pressedTool_ = tool; requestUpdate(); return; }
    if (composeText().contains(point)) { focus_ = 1; placeCursor(compose_, composeText(), point, false); requestUpdate(); return; }
    if (CtlObject *pressed = buttonAt(point)) { pressed_ = pressed->id; requestUpdate(); }
  }
  void contentMouseMove(const QPoint &point) override {
    if (draggingSplitter_) { setSplitter(point.y()); requestUpdate(); return; }
    layout();
    const int menu = PaintedMenuBar::hit(menuRects_, point); CtlObject *button = buttonAt(point); const quint32 hovered = button ? button->id : 0;
    if (const int tool = toolbar_.hit(point); tool != hoveredTool_) { hoveredTool_ = tool; requestUpdate(); }
    setCursor(mode_ == Conversation && splitterBand().contains(point) ? Qt::SizeVerCursor : composeText().contains(point) ? Qt::IBeamCursor : Qt::ArrowCursor);
    if (menu != hoveredMenu_ || hovered != hovered_) { hoveredMenu_ = menu; hovered_ = hovered; requestUpdate(); }
  }
  void contentMouseRelease(const QPoint &point, Qt::MouseButton button) override {
    if (button != Qt::LeftButton) return;
    if (draggingSplitter_) { draggingSplitter_ = false; return; }
    if (pressedTool_ >= 0) { const int tool = pressedTool_; pressedTool_ = -1; requestUpdate(); if (toolbar_.hit(point) == tool) toolCommand(toolbar_.items()[tool].command); return; }
    const quint32 pressed = pressed_; pressed_ = 0; requestUpdate();
    CtlObject *target = buttonAt(point); if (pressed && target && target->id == pressed) command(pressed);
  }
  void contentLeave() override { if (hovered_ || hoveredMenu_ >= 0 || hoveredTool_ >= 0) { hovered_ = 0; hoveredMenu_ = -1; hoveredTool_ = -1; requestUpdate(); } }
  void contentKeyPress(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Escape) { closeRequested(); return; }  // IDCANCEL closes (0x1138dbe8)
    if (focus_ == 1 && event->key() == Qt::Key_Tab && prefs::tabInsertsTab()) { compose_.cursor.insertText(QStringLiteral("\t")); edited(); return; }
    if (focus_ == 1 && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && prefs::enterInsertsReturn()) {
      // "Enter key inserts Return": Enter starts a new line and Ctrl+Enter sends.
      if (event->modifiers().testFlag(Qt::ControlModifier)) command(Send); else { compose_.cursor.insertText(QStringLiteral("\n")); edited(); }
      return;
    }
    if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) { if (toRowShown()) focus_ = 1 - focus_; requestUpdate(); return; }
    if (event->key() == Qt::Key_F2) { command(399); return; }
    bool submit = false;
    if (editor().handleKey(event, focus_ == 0, &submit)) { if (submit) { if (focus_ == 0) focus_ = 1; else command(Send); } edited(); event->accept(); return; }
    WindowBase::contentKeyPress(event);
  }
  void wheelEvent(QWheelEvent *event) override {
    const QPoint point = canvasPoint(event->position().toPoint());
    if (mode_ == Conversation && historyRect_.contains(point)) { transcript_.followBottom = false; transcript_.scroll = qMax(qreal(0), transcript_.scroll - event->angleDelta().y() / 2.0); }
    else if (composeRect_.contains(point)) compose_.scroll = qMax(qreal(0), compose_.scroll - event->angleDelta().y() / 2.0);
    else { event->ignore(); return; }
    requestUpdate(); event->accept();
  }
private:
  static QSize defaultCanvas() {
    // VALUERES 109/110 = 380 x 240 (0x1138cda0, 0x1138d7ef). The reference screenshot of a new IM window is 380 px
    // wide outside but 300 px tall: the 240 is the height below the caption and menu bar (inferred from that capture).
    QSize outer(380, 240);
#ifdef Q_OS_WIN
    RECT frame{0, 0, 0, 0}; AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
    RECT withMenu{0, 0, 0, 0}; AdjustWindowRectEx(&withMenu, WS_OVERLAPPEDWINDOW, TRUE, 0);
    outer.rheight() += (withMenu.bottom - withMenu.top);
    const QSize client(outer.width() - (frame.right - frame.left), outer.height() - (frame.bottom - frame.top));
    return client + QSize(2, TitleBarHeight + 3);
#else
    return outer;
#endif
  }
  QRect client() const {
#ifdef Q_OS_WIN
    return QRect(1, TitleBarHeight, width(), height());
#else
    return QRect(0, TitleBarHeight, width(), height() - TitleBarHeight);
#endif
  }
  TextEditor &editor() { return focus_ == 0 ? recipient_ : compose_; }
  bool toRowShown() { CtlObject *row = group_ ? group_->find(ToRow) : nullptr; return row && row->shown(); }
  void setMode(Mode mode) {
    // SetMode 0x113908db
    mode_ = mode; if (!group_) return;
    ctlShowControl(*group_, BuddyIcon, false);
    const bool conversation = mode == Conversation;
    ctlShowControl(*group_, ToRow, !conversation);
    ctlShowControl(*group_, History, conversation);
    for (quint32 id : {Warn, Block, WarnSeparator, AddBuddy}) ctlShowControl(*group_, id, conversation);
    if (conversation) { focus_ = 1; updateTitle(); }
  }
  void updateTitle() { const QString name = recipient(); const QString kind = aimString(directConnected() ? 657 : 517); setTitle(name.isEmpty() ? kind : QStringLiteral("%1 - %2").arg(name, kind)); } // STRING 515 "%s - %s" with STRING 517, or 657 "Direct Instant Message" while connected
  void layout() {
    const QRect c = client(); const int menuTop = c.top() + 4;
    menuRects_ = menu_.layout(c.left(), menuTop, c.width());
    const int top = PaintedMenuBar::bottom(menuRects_, menuTop);
    if (group_) ctlMove(*group_, QRect(QPoint(c.left() + 3, top + 5), QPoint(c.right() - 2, c.bottom())), aimEnvironment()); // WM_SIZE: left+3, top+5, right-2 (0x1138e912)
    CtlObject *history = group_ ? group_->find(History) : nullptr, *compose = group_ ? group_->find(Compose) : nullptr;
    historyRect_ = history && history->shown() ? history->windowRect() : QRect(); composeRect_ = compose ? compose->windowRect() : QRect();
    if (mode_ == Conversation && splitterY_ >= 0 && historyRect_.isValid()) {
      // Splitter drag (0x1138e973): y clamped to [history.top + 20, compose.bottom - 40], 4 px band, compose starts below it.
      const int y = qBound(historyRect_.top() + HistoryMinimum, splitterY_, composeRect_.bottom() - ComposeMinimum);
      historyRect_.setBottom(y - 1); composeRect_.setTop(y + SplitterBand);
    }
  }
  QRect splitterBand() const { return historyRect_.isValid() ? QRect(historyRect_.left(), historyRect_.bottom() + 1, historyRect_.width(), composeRect_.top() - historyRect_.bottom() - 1) : QRect(); }
  void setSplitter(int y) { splitterY_ = y; }
  QRect composeText() const { return composeRect_.adjusted(1, 1 + ToolbarHeight, -1, -1); }
  static QRect comboRect(const QRect &window) { return QRect(window.left(), window.top(), window.width(), 23); }
  CtlObject *buttonAt(const QPoint &point) {
    CtlObject *found = nullptr;
    std::function<void(CtlObject &)> visit = [&](CtlObject &o) { if (!o.shown()) return; if (o.kind == CtlObject::Kind::ArtButton && o.art[0] && o.windowRect().contains(point)) found = &o; for (const auto &child : o.children) visit(*child); };
    if (group_) visit(*group_); return found;
  }
  bool enabled(const CtlObject &o) const { if (o.id == Send) return !sending_ && !compose_.text().trimmed().isEmpty(); return true; }
  void paintButton(QPainter &p, const CtlObject &o) {
    if (!o.art[0]) return; // art-less buttons are 2x2 placeholders (816 / 665)
    const bool isEnabled = enabled(o); const int state = !isEnabled ? 0 : (pressed_ == o.id && hovered_ == o.id) ? 2 : hovered_ == o.id ? 1 : 0;
    QImage image = artImage(o.art[state] ? o.art[state] : o.art[0]); if (!isEnabled) image = disabledImage(image);
    const QRect r = o.windowRect(); p.drawImage(r.topLeft() + QPoint(1, 1), image); // type-2 art buttons: art + 2 px
  }
  void paintCombo(QPainter &p, const QRect &window) {
    const QRect field = comboRect(window);
    p.fillRect(field, Qt::white); p.setPen(QColor(122, 122, 122)); p.drawRect(field.adjusted(0, 0, -1, -1));
    const QPoint c(field.right() - 9, field.center().y()); p.setPen(QPen(QColor(60, 60, 60), 1)); p.drawLine(c + QPoint(-4, -2), c + QPoint(0, 2)); p.drawLine(c + QPoint(0, 2), c + QPoint(4, -2));
    p.save(); p.setClipRect(field.adjusted(3, 1, -18, -1)); p.setFont(recipient_.document.defaultFont()); p.setPen(Qt::black);
    const QString text = recipient_.text(); const QFontMetricsF metrics(recipient_.document.defaultFont());
    const qreal baseline = field.center().y() + (metrics.ascent() - metrics.descent()) / 2; p.drawText(QPointF(field.left() + 4, baseline), text);
    if (focus_ == 0 && caretOn()) { const qreal x = field.left() + 4 + metrics.horizontalAdvance(text.left(recipient_.cursor.position())); p.drawLine(QPointF(x, field.top() + 4), QPointF(x, field.bottom() - 4)); }
    p.restore();
  }
  void paintAte(QPainter &p, const QRect &r, bool composePane) {
    if (!r.isValid()) return;
    p.fillRect(r, composePane ? prefs::composeWindowColor() : QColor(Qt::white)); p.setPen(PaneBorder); p.drawRect(r.adjusted(0, 0, -1, -1));
    QRect view = r.adjusted(1, 1, -1, -1);
    if (composePane) {
      const QRect toolbar(view.left(), view.top(), view.width(), ToolbarHeight);
      toolbar_.layout(toolbar); toolbar_.paint(p, hoveredTool_, pressedTool_, checkedFormats());
      view.setTop(toolbar.bottom() + 1);
      drawEditor(p, view.adjusted(3, 2, -3, -2));
    } else drawTranscript(p, view.adjusted(3, 2, -3, -2));
  }
  void drawEditor(QPainter &p, const QRect &view) {
    TextEditor &editor = compose_; editor.document.setTextWidth(qMax(1, view.width()));
    const QSizeF documentSize = editor.document.documentLayout()->documentSize(); const QTextBlock block = editor.cursor.block(); const QTextLayout *layout = block.layout();
    const int blockPosition = editor.cursor.position() - block.position(); const QTextLine line = layout ? layout->lineForTextPosition(blockPosition) : QTextLine();
    const QRectF blockRect = editor.document.documentLayout()->blockBoundingRect(block); const qreal caretY = line.isValid() ? blockRect.top() + line.y() + line.height() : blockRect.bottom();
    editor.scroll = qBound(qreal(0), caretY - view.height() + 2, qMax(qreal(0), documentSize.height() - view.height()));
    p.save(); p.setClipRect(view); p.translate(view.left(), view.top() - editor.scroll);
    QAbstractTextDocumentLayout::PaintContext context; context.clip = QRectF(0, editor.scroll, view.width(), view.height()); context.palette = atePalette();
    if (editor.cursor.hasSelection()) { QAbstractTextDocumentLayout::Selection selection; selection.cursor = editor.cursor; selection.format.setBackground(QColor(0, 120, 215)); selection.format.setForeground(Qt::white); context.selections.append(selection); }
    editor.document.documentLayout()->draw(&p, context);
    if (focus_ == 1 && caretOn() && line.isValid()) { const qreal x = line.cursorToX(blockPosition), y = blockRect.top() + line.y(); p.setPen(Qt::black); p.drawLine(QPointF(blockRect.left() + x, y), QPointF(blockRect.left() + x, y + line.height())); }
    p.restore();
  }
  void drawTranscript(QPainter &p, const QRect &view) {
    // Preferences > IM/Chat > Text Magnification scales the history pane (200 / 133 / 100 / 75 %).
    const qreal zoom = prefs::textMagnification(); const qreal width = view.width() / zoom, height = view.height() / zoom;
    transcript_.document.setTextWidth(qMax(qreal(1), width));
    const qreal maxScroll = qMax(qreal(0), transcript_.document.documentLayout()->documentSize().height() - height);
    transcript_.scroll = transcript_.followBottom ? maxScroll : qBound(qreal(0), transcript_.scroll, maxScroll);
    if (transcript_.scroll >= maxScroll) transcript_.followBottom = true;
    p.save(); p.setClipRect(view); p.translate(view.left(), view.top()); p.scale(zoom, zoom); p.translate(0, -transcript_.scroll); { QAbstractTextDocumentLayout::PaintContext context; context.clip = QRectF(0, transcript_.scroll, width, height); context.palette = atePalette(); p.setClipRect(QRectF(0, transcript_.scroll, width, height), Qt::IntersectClip); transcript_.document.documentLayout()->draw(&p, context); } p.restore();
  }
  void paintRateMeter(QPainter &p, const QRect &r) {
    // _Oscar_RateMeter: 15 cells, 3 px pitch ((15+1)*3 x 8); red/yellow at the low end, green when sending is allowed.
    p.fillRect(r, QColor(64, 64, 64));
    for (int i = 0; i < 15; ++i) { const QColor color = i < 2 ? QColor(255, 0, 0) : i < 5 ? QColor(255, 255, 0) : QColor(0, 200, 0); p.fillRect(QRect(r.left() + 2 + i * 3, r.top() + 1, 2, r.height() - 2), color); }
  }
  bool caretOn() const { return isActive(); }
  static void placeCursor(TextEditor &editor, const QRect &rect, const QPoint &point, bool singleLine) {
    if (singleLine) { const QFontMetricsF metrics(editor.document.defaultFont()); const QString value = editor.text(); int best = 0; qreal distance = std::numeric_limits<qreal>::max(); for (int i = 0; i <= value.size(); ++i) { const qreal d = qAbs(point.x() - (rect.left() + 1 + metrics.horizontalAdvance(value.left(i)))); if (d < distance) { best = i; distance = d; } } editor.cursor.setPosition(best); return; }
    const QRect view = rect.adjusted(3, 2, -3, -2); editor.document.setTextWidth(qMax(1, view.width())); editor.document.documentLayout()->documentSize();
    const int position = editor.document.documentLayout()->hitTest(QPointF(point.x() - view.left(), point.y() - view.top() + editor.scroll), Qt::FuzzyHit); if (position >= 0) editor.cursor.setPosition(position);
  }
  void edited() {
    if (focus_ == 0) { if (recipientChanged) recipientChanged(recipient()); }
    else if (directConnected()) { // typing frames: 0x0E typing, 0x06 typed (after a pause), 0x02 nothing typed
      const quint8 state = compose_.text().isEmpty() ? 0x02 : 0x0E;
      if (state != lastTyping_) { direct_->sendTyping(state); lastTyping_ = state; }
      if (state == 0x0E) typingTimer_.start();
    }
    requestUpdate();
  }
  void openMenu(int index) {
    if (index < 0 || index >= menuRects_.size()) return;
    openMenu_ = index; requestUpdate();
    // 818 greyed while connected, 819 and Insert > Image greyed while not (0x1138ef4a)
    const QList<int> disabled = directConnected() ? QList<int>{818} : QList<int>{819, 1104};
    const int id = popupMenu(this, menu_.items[index].children, canvasToGlobal(menuRects_[index].bottomLeft() + QPoint(0, 1)), disabled);
    openMenu_ = -1; hoveredMenu_ = -1; requestUpdate();
    if (id && !disabled.contains(id)) command(quint32(id));
  }
  void showRecentNames(const QRect &field) {
    QList<MenuItem> items; for (const QString &name : QSettings().value(QStringLiteral("IM/recentScreenNames")).toStringList()) { MenuItem item; item.text = QString(name).replace(QLatin1Char('&'), QStringLiteral("&&")); item.id = 1000 + int(items.size()); items.append(item); }
    if (items.isEmpty()) return;
    const QStringList names = QSettings().value(QStringLiteral("IM/recentScreenNames")).toStringList();
    const int id = popupMenu(this, items, canvasToGlobal(field.bottomLeft() + QPoint(0, 1)));
    if (id >= 1000 && id - 1000 < names.size()) { recipient_.setText(names[id - 1000]); focus_ = 1; edited(); }
  }
  QList<int> checkedFormats() const {
    // ate32 reports B/I/U state of the caret/selection to the bar (WM_USER+0x49 -> SetButtonState).
    const QTextCharFormat format = compose_.cursor.charFormat(); QList<int> checked;
    if (format.fontWeight() >= QFont::Bold) checked.append(AteToolbar::Bold); if (format.fontItalic()) checked.append(AteToolbar::Italic); if (format.fontUnderline()) checked.append(AteToolbar::Underline);
    return checked;
  }
  void applyFormat(const QTextCharFormat &format) { if (compose_.cursor.hasSelection()) compose_.cursor.mergeCharFormat(format); else { QTextCharFormat current = compose_.cursor.charFormat(); current.merge(format); compose_.cursor.setCharFormat(current); } focus_ = 1; requestUpdate(); }
  void toolCommand(int command) {
    const QTextCharFormat current = compose_.cursor.charFormat(); QTextCharFormat format;
    switch (command) {
    case AteToolbar::Bold: format.setFontWeight(current.fontWeight() >= QFont::Bold ? QFont::Normal : QFont::Bold); applyFormat(format); return;
    case AteToolbar::Italic: format.setFontItalic(!current.fontItalic()); applyFormat(format); return;
    case AteToolbar::Underline: format.setFontUnderline(!current.fontUnderline()); applyFormat(format); return;
    case AteToolbar::Smaller: case AteToolbar::Larger: case AteToolbar::NormalSize: {
      // HTML font sizes 1..7; 3 is the default (Times New Roman 12 pt).
      const int size = command == AteToolbar::NormalSize ? 3 : qBound(1, htmlSize(current) + (command == AteToolbar::Larger ? 1 : -1), 7);
      format.setProperty(HtmlSizeProperty, size); format.setFontPointSize(htmlPointSize(size)); applyFormat(format); return;
    }
    case AteToolbar::TextColor: case AteToolbar::BackgroundColor: {
      const bool text = command == AteToolbar::TextColor; QColor color;
      if (!chooseColor(text ? current.foreground().color() : (current.background().style() == Qt::NoBrush ? QColor(Qt::white) : current.background().color()), color)) return;
      if (text) format.setForeground(color); else format.setBackground(color); applyFormat(format); return;
    }
    case AteToolbar::Smiley: {
      // The picker is centred 100 px above the mouse pointer (ate32 0x1201afb3: GetCursorPos, TPM_CENTERALIGN).
      const int glyph = AteToolbar::pickSmiley(this, QCursor::pos() - QPoint(0, 100));
      if (glyph >= 0) { compose_.cursor.insertText(AteToolbar::smileyCode(glyph)); focus_ = 1; requestUpdate(); }
      return;
    }
    case AteToolbar::Link: if (ate::editLink(this, compose_.cursor)) edited(); focus_ = 1; requestUpdate(); return;
    case AteToolbar::Greeting: openGreeting(); return;
    case AteToolbar::ConnectImage: this->command(818); return; // same as People > Connect to Send IM Image
    case AteToolbar::InsertPicture: this->command(1104); return; // Insert Picture (0x265) while connected
    default: return;
    }
  }
  static constexpr int HtmlSizeProperty = QTextFormat::UserProperty + 1;
  static int htmlSize(const QTextCharFormat &format) { return format.hasProperty(HtmlSizeProperty) ? format.intProperty(HtmlSizeProperty) : 3; }
  static qreal htmlPointSize(int size) { static const qreal points[] = {8, 10, 12, 14, 18, 24, 36}; return points[qBound(1, size, 7) - 1]; }
  QRect caretRect() {
    const QRect view = composeText().adjusted(3, 2, -3, -2); const QTextBlock block = compose_.cursor.block(); const QTextLayout *layout = block.layout(); const int position = compose_.cursor.position() - block.position();
    const QTextLine line = layout ? layout->lineForTextPosition(position) : QTextLine(); const QRectF blockRect = compose_.document.documentLayout()->blockBoundingRect(block);
    if (!line.isValid()) return QRect(view.topLeft(), QSize(1, 16));
    return QRect(QPoint(view.left() + int(blockRect.left() + line.cursorToX(position)), view.top() + int(blockRect.top() + line.y() - compose_.scroll)), QSize(1, int(line.height())));
  }
  bool chooseColor(const QColor &initial, QColor &chosen) {
#ifdef Q_OS_WIN
    // ChooseColorA with CC_RGBINIT|CC_PREVENTFULLOPEN and a grey custom-colour ramp (ate32 0x12010970).
    static COLORREF custom[16]; for (int i = 0; i < 16; ++i) { const int v = (i + 1) * 15; custom[i] = RGB(v, v, v); }
    CHOOSECOLORW chooser{}; chooser.lStructSize = sizeof(chooser); chooser.hwndOwner = reinterpret_cast<HWND>(winId()); chooser.rgbResult = RGB(initial.red(), initial.green(), initial.blue()); chooser.lpCustColors = custom; chooser.Flags = CC_RGBINIT | CC_PREVENTFULLOPEN;
    if (!ChooseColorW(&chooser)) return false;
    chosen = QColor(GetRValue(chooser.rgbResult), GetGValue(chooser.rgbResult), GetBValue(chooser.rgbResult)); return true;
#else
    Q_UNUSED(initial); Q_UNUSED(chosen); return false;
#endif
  }
  static QString aimHtml(const QTextDocument &document) { return ate::html(document, prefs::composeWindowColor()); }
  void openGreeting() {
    // icbmui 0x1138e1b7: STRING 1801 with both names, STRING 1800 when there is no recipient yet.
    const QString from = client_ ? client_->screenName() : QString(), to = recipient();
    QString url = aimString(to.isEmpty() ? 1800 : 1801); url.replace(url.indexOf(QStringLiteral("%s")), 2, from); if (!to.isEmpty()) url.replace(url.indexOf(QStringLiteral("%s")), 2, to);
    ate::openUrl(url);
  }
  void command(quint32 id) {
    switch (id) {
    case Send: case 1: send(); return;
    case 2: closeRequested(); return;                                    // File > Close / IDCANCEL
    case 396: editor().copy(true); requestUpdate(); return;               // Cut
    case 397: if (focus_ == 1 && !compose_.cursor.hasSelection()) return; editor().copy(false); return; // Copy
    case 398: editor().paste(focus_ == 0); edited(); return;              // Paste
    case 399: compose_.input(QLocale::system().toString(QTime::currentTime(), QLocale::ShortFormat), false); focus_ = 1; requestUpdate(); return; // Insert > Timestamp (F2)
    case Warn: case 668: userActions::warn(this, client_, recipient()); return;          // Warn button / People > Warn...
    case Block: case 666: userActions::block(this, client_, recipient()); return;        // Block button / People > Block...
    case AddBuddy: case 670: userActions::addBuddy(this, client_, recipient()); return;  // Add Buddy button / People > Add to Buddy List...
    case GetInfo: case 669: BuddyInfoWindow::open(client_, recipient(), [this](int, const QString &name) { if (openMessage) openMessage(name); }); return; // Get Info button / People > Info...
    case 665: if (inviteToChat) inviteToChat(recipient()); return;                       // People > Send Chat Invitation... / &Chat
    case 818: if (connectImage) connectImage(this); return;            // People > Connect to Send IM Image
    case 819: if (closeImage) closeImage(this); return;                 // People > Close IM Image Connection
    case 1104: if (directConnected() && ate::insertPicture(this, compose_.cursor)) { focus_ = 1; edited(); } return; // Insert > Image or Sound File (only while connected)
    case 1106: toolCommand(AteToolbar::Link); return;                                  // Insert > Web Link...
    case 1198: case 0x4B0: openGreeting(); return;                                        // People > Send IM Greeting
    default: return; // Talk (voice) and the rendezvous items are not implemented
    }
  }
  void rememberRecipient(const QString &name) {
    // CtlGroup persistent combo keyed by STRING 518 "recent IM ScreenNames".
    QSettings settings; QStringList names = settings.value(QStringLiteral("IM/recentScreenNames")).toStringList();
    names.removeIf([&](const QString &value) { return normalizedName(value) == normalizedName(name); }); names.prepend(name); while (names.size() > 10) names.removeLast();
    settings.setValue(QStringLiteral("IM/recentScreenNames"), names);
  }
  void send() {
    const QString target = recipient(), text = compose_.text();
    if (sending_ || text.trimmed().isEmpty()) return;
    if (target.isEmpty()) { errorBox(aimString(525)); focus_ = 0; requestUpdate(); return; }  // STRING 525
    if (directConnected()) { // IM Image connection: the whole compose pane, images included, goes as one ODC2 frame
      quint16 encoding = 0; const QByteArray payload = ate::directPayload(compose_.document, prefs::composeWindowColor(), &encoding);
      direct_->sendMessage(payload, encoding); direct_->sendTyping(0x02); lastTyping_ = 0x02;
      transcript_.appendMessage(client_ ? client_->screenName() : QString(), ate::directHtml(payload, encoding, transcript_.document), true);
      playAimSound(AimSound::ImSend); compose_.clear(); compose_.cursor.setCharFormat(prefs::composeFormat()); setMode(Conversation); requestUpdate(); return;
    }
    if (ate::hasImages(compose_.document)) { errorBox(aimString(1326)); return; } // STRING 1326: images need the direct connection
    if (!client_ || !client_->connected()) { errorBox(aimString(542)); return; }            // STRING 542
    sendAttempt_ = true; attemptError_.clear(); if (sendStarted) sendStarted(this);
    const QString html = aimHtml(compose_.document);
    const bool queued = client_->sendMessage(target, html);
    if (sendFinished) sendFinished(this); sendAttempt_ = false;
    if (!queued || !attemptError_.isEmpty()) { appendFailure(attemptError_); attemptError_.clear(); requestUpdate(); return; }
    playAimSound(AimSound::ImSend); rememberRecipient(target); pendingRecipient_ = target; pendingText_ = html; pendingPlain_ = text; sending_ = true; compose_.clear(); compose_.cursor.setCharFormat(prefs::composeFormat()); updateTitle(); requestUpdate();
  }
  void restorePending() { const QString current = compose_.text(); compose_.setText(current.isEmpty() ? pendingPlain_ : pendingPlain_ + QStringLiteral("\n") + current); pendingText_.clear(); pendingRecipient_.clear(); sending_ = false; }
  void appendFailure(const QString &reason) {
    // Delivery failures become history notices: STRING 545 for an unavailable user, 546 with the error code otherwise.
    const QString name = recipient().toHtmlEscaped(); const QRegularExpressionMatch code = QRegularExpression(QStringLiteral("0x([0-9a-fA-F]{4})")).match(reason);
    QString notice;
    if (code.hasMatch() && code.captured(1).toInt(nullptr, 16) == 4) notice = QString(aimString(545)).replace(QStringLiteral("%s"), name);
    else if (code.hasMatch()) notice = QString(aimString(546)).replace(QStringLiteral("%s"), name).replace(QStringLiteral("%d"), QString::number(code.captured(1).toInt(nullptr, 16))).replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    else notice = QString(aimString(522)).replace(QStringLiteral("%s"), name) + (reason.isEmpty() ? QString() : QStringLiteral("<br>") + reason.toHtmlEscaped());
    transcript_.appendNotice(notice); setMode(Conversation);
  }
  void errorBox(const QString &text) {
#ifdef Q_OS_WIN
    const QString title = aimString(537); MessageBoxW(reinterpret_cast<HWND>(winId()), reinterpret_cast<LPCWSTR>(text.utf16()), reinterpret_cast<LPCWSTR>(title.utf16()), MB_OK | MB_ICONEXCLAMATION);
#else
    transcript_.appendNotice(text.toHtmlEscaped()); setMode(Conversation);
#endif
  }
  OscarClient *client_ = nullptr;
  TextEditor recipient_;
  TextEditor compose_;
  Transcript transcript_;
  PaintedMenuBar menu_;
  std::shared_ptr<CtlObject> group_;
  QVector<QRect> menuRects_;
  QRect historyRect_, composeRect_;
  Mode mode_ = NewMessage;
  QString pendingRecipient_, pendingText_, pendingPlain_, attemptError_;
  AteToolbar toolbar_{AteToolbar::Set::InstantMessage};
  QPointer<DirectConnection> direct_;
  quint8 peerTyping_ = 0, lastTyping_ = 0;
  QTimer typingTimer_;
  int hoveredTool_ = -1, pressedTool_ = -1;
  int focus_ = 1, hoveredMenu_ = -1, openMenu_ = -1, splitterY_ = -1;
  quint32 hovered_ = 0, pressed_ = 0;
  bool sending_ = false, sendAttempt_ = false, draggingSplitter_ = false;
};
}

struct MessagingWindows::State {
  MessagingWindows *owner = nullptr;
  OscarClient *client = nullptr;
  QPointer<QWindow> transientParent;
  QHash<QString, QPointer<MessageWindow>> byRecipient;
  QList<QPointer<MessageWindow>> windows;
  QPointer<MessageWindow> activeAttempt;
  std::function<void(const QString &)> chatHandler;
  State(MessagingWindows *ownerValue, OscarClient *clientValue, QWindow *parentValue) : owner(ownerValue), client(clientValue), transientParent(parentValue) {
    if (!client) return;
    QObject::connect(client, &OscarClient::messageReceived, owner, [this](const QString &sender, const QString &text, bool autoResponse) {
      const bool existing = byRecipient.value(normalizedName(sender)) != nullptr;
      if (!existing && !autoResponse && !acceptMessage(sender, text)) return;
      playAimSound(existing ? AimSound::ImReceive : AimSound::ImFirstReceive);
      // Away Message "Hide windows while I'm Away": new conversations open minimized instead of popping up.
      const bool quiet = client->away() && prefs::hideWindowsWhileAway();
      MessageWindow *window = quiet && !existing ? openQuietly(sender) : open(sender); window->appendIncoming(sender, text, autoResponse); if (!quiet) window->showWindow();
    });
    // Preferences > IM/Chat "Show sign on/off notifications": STRING 1344 / 1345 in an open conversation.
    QObject::connect(client, &OscarClient::buddyPresenceChanged, owner, [this](const QString &name, bool online) {
      if (!prefs::imSignOnOffNotices()) return; MessageWindow *window = byRecipient.value(normalizedName(name)); if (!window || !window->hasConversation()) return;
      QString text = aimString(online ? 1344 : 1345); text.replace(text.indexOf(QStringLiteral("%s")), 2, name); text.replace(text.indexOf(QStringLiteral("%s")), 2, QLocale::system().toString(QTime::currentTime(), QStringLiteral("h:mm:ss AP")));
      window->appendPresenceNotice(text);
    });
    QObject::connect(client, &OscarClient::messageAccepted, owner, [this](const QString &recipient, quint64) { for (const auto &window : windows) if (window && window->acknowledge(recipient)) return; });
    QObject::connect(client, &OscarClient::operationFailed, owner, [this](const QString &operation, const QString &reason) { const QString message = QStringLiteral("%1: %2").arg(operation, reason); if (activeAttempt) { activeAttempt->recordOperationFailure(message); return; } for (const auto &window : windows) if (window && window->isSending() && operation.startsWith(QStringLiteral("IM to "), Qt::CaseInsensitive) && normalizedName(operation.mid(6)) == normalizedName(window->pendingRecipient())) { window->sendFailed(message); return; } });
    QObject::connect(client, &OscarClient::failed, owner, [this](const QString &reason) { resetPending(reason); });
    QObject::connect(client, &OscarClient::rendezvousReceived, owner, [this](const aim::oscar::Rendezvous &rv) { incomingRendezvous(rv); });
    QObject::connect(client, &OscarClient::loginStageChanged, owner, [this](int stage) { if (stage == 0) resetPending(); });
  }
  MessageWindow *openQuietly(const QString &recipient) { quietOpen = true; MessageWindow *window = open(recipient); quietOpen = false; return window; }
  bool quietOpen = false;
  MessageWindow *open(const QString &recipient) {
    // One window per buddy (0x1138d434).
    const QString key = normalizedName(recipient); if (!key.isEmpty() && byRecipient.contains(key) && byRecipient.value(key)) { MessageWindow *window = byRecipient.value(key); window->showWindow(); return window; }
    int cascade = 0; for (const auto &window : windows) if (window) ++cascade;
    auto *window = new MessageWindow(client, transientParent, owner, recipient, cascade); if (!key.isEmpty()) byRecipient.insert(key, window); windows.append(window);
    window->recipientChanged = [this, window](const QString &name) { for (auto it = byRecipient.begin(); it != byRecipient.end();) { if (it.value() == window) it = byRecipient.erase(it); else ++it; } if (!name.isEmpty()) byRecipient.insert(normalizedName(name), window); };
    window->sendStarted = [this](MessageWindow *surface) { activeAttempt = surface; };
    window->sendFinished = [this](MessageWindow *surface) { if (activeAttempt == surface) activeAttempt.clear(); };
    window->openMessage = [this](const QString &name) { open(name); };
    window->inviteToChat = [this](const QString &name) { if (chatHandler) chatHandler(name); };
    window->connectImage = [this](MessageWindow *surface) { startImage(surface); };
    window->closeImage = [this](MessageWindow *surface) { closeImageFor(surface->recipient(), true); };
    if (DirectSession *session = images.value(key)) if (session->connection && session->connection->isConnected()) window->setDirect(session->connection);
    QObject::connect(window, &QObject::destroyed, owner, [this, window] { for (auto it = byRecipient.begin(); it != byRecipient.end();) { if (it.value().isNull() || it.value().data() == window) it = byRecipient.erase(it); else ++it; } windows.removeIf([window](const QPointer<MessageWindow> &value) { return value.isNull() || value.data() == window; }); if (activeAttempt.data() == window) activeAttempt.clear(); });
    if (quietOpen) window->showMinimized(); else window->showWindow(); return window;
  }
  // RT_DIALOG 144 "Accept Message" for a first message from someone not on the Buddy List (Preferences > IM/Chat).
  bool acceptMessage(const QString &sender, const QString &text) {
    if (!prefs::acceptMessageDialog() || !client) return true;
    for (const auto &item : client->roster()) if (item.classId == 0 && normalizedName(item.name) == normalizedName(sender)) return true;
#ifdef Q_OS_WIN
    Q_UNUSED(text); bool dontShow = false; int choice = 0;
    runOriginalDialog(transientParent && transientParent->handle() ? reinterpret_cast<HWND>(transientParent->winId()) : nullptr, 144,
      [&](HWND dialog) {
        auto set = [&](int id, QString value) { SetDlgItemTextW(dialog, id, reinterpret_cast<LPCWSTR>(value.utf16())); };
        wchar_t buffer[1024]{}; GetDlgItemTextW(dialog, 392, buffer, 1024); set(392, formatAimString(QString::fromWCharArray(buffer), {sender}));
        GetDlgItemTextW(dialog, 393, buffer, 1024); set(393, formatAimString(QString::fromWCharArray(buffer), {QStringLiteral("0")}));
        GetDlgItemTextW(dialog, 255, buffer, 1024); set(255, formatAimString(QString::fromWCharArray(buffer), {sender}));
      },
      [&](HWND dialog, int id, int) {
        if (id == IDOK || id == IDCANCEL || id == 390) { choice = id; dontShow = IsDlgButtonChecked(dialog, 391) == BST_CHECKED; EndDialog(dialog, id); return true; }
        if (id == 389) { BuddyInfoWindow::open(client, sender, [this](int, const QString &name) { open(name); }); return true; }
        return false;
      });
    if (dontShow) { QSettings().setValue(QStringLiteral("nativePreferences/288/357"), 0); prefs::invalidate(); }
    if (choice == 390) { userActions::block(nullptr, client, sender); return false; }
    return choice == IDOK;
#else
    Q_UNUSED(sender); Q_UNUSED(text); return true;
#endif
  }
  // ---- IM Image (Research/direct_im.md) ----
  struct DirectSession { QString name; quint64 cookie = 0; bool proposer = false, reverse = false; QPointer<DirectConnection> connection; void *status = nullptr; QTimer timer; };
  QHash<QString, DirectSession *> images;
  QWindow *ownerFor(const QString &name) { MessageWindow *window = byRecipient.value(normalizedName(name)); return window ? static_cast<QWindow *>(window) : transientParent.data(); }
#ifdef Q_OS_WIN
  static HWND hwnd(QWindow *window) { return window && window->handle() ? reinterpret_cast<HWND>(window->winId()) : nullptr; }
  // Rendezvous results are message boxes titled STRING 1010 "Rendezvous Error".
  void rendezvousBox(QWindow *owner, quint32 id, const QString &name) { const QString text = QString(aimString(id)).replace(QStringLiteral("%s"), name), title = aimString(1010); MessageBoxW(hwnd(owner), reinterpret_cast<LPCWSTR>(text.utf16()), reinterpret_cast<LPCWSTR>(title.utf16()), MB_OK | MB_ICONINFORMATION); }
#else
  void rendezvousBox(QWindow *, quint32, const QString &) {}
#endif
  static QByteArray ipv4(const QHostAddress &address) { QByteArray out(4, 0); qToBigEndian(address.toIPv4Address(), out.data()); return out; }
  static QHostAddress localAddress() {
    // GetLocalIP: first address of this host, the second one with "Use alternate Internet Address" (prefs 276/279, 903).
    QList<QHostAddress> found; for (const QHostAddress &a : QNetworkInterface::allAddresses()) if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !a.isLinkLocal()) found.append(a);
    if (found.isEmpty()) return QHostAddress(QHostAddress::LocalHost);
    return prefs::checked(276, 903) && found.size() > 1 ? found[1] : found[0];
  }
  aim::oscar::Rendezvous rendezvous(quint16 type, quint64 cookie) {
    aim::oscar::Rendezvous rv; rv.type = type; rv.cookie = cookie; rv.capability = aim::oscar::capDirectIm();
    if (type != 1) { QByteArray port(2, 0); qToBigEndian<quint16>(5190, port.data()); QByteArray sequence(2, 0); qToBigEndian<quint16>(1, sequence.data()); rv.values = {{0x03, ipv4(localAddress())}, {0x05, port}, {0x0a, sequence}}; } // the advertised port stays 5190; connections use 4443
    return rv;
  }
  void showStatus(DirectSession *session, int textId) {
#ifdef Q_OS_WIN
    if (QSettings().value(QStringLiteral("IM/IMDirectNoStatusDlg"), 0).toInt()) return;
    if (session->status) { DestroyWindow(static_cast<HWND>(session->status)); session->status = nullptr; }
    const QString name = session->name;
    session->status = createOriginalDialog(hwnd(ownerFor(name)), 230, [textId](HWND dialog) { for (int id : {805, 833, 834}) ShowWindow(GetDlgItem(dialog, id), id == textId ? SW_SHOW : SW_HIDE); },
      [this, name](HWND dialog, int id, int) { if (id != IDCANCEL) return false; if (DirectSession *s = images.value(normalizedName(name))) s->status = nullptr; DestroyWindow(dialog); cancelImage(name, 1); return true; });
#else
    Q_UNUSED(session); Q_UNUSED(textId);
#endif
  }
  void closeStatus(DirectSession *session) {
#ifdef Q_OS_WIN
    if (session->status) { HWND dialog = static_cast<HWND>(session->status); session->status = nullptr; DestroyWindow(dialog); }
#endif
  }
  DirectSession *newSession(const QString &name, quint64 cookie, bool proposer) {
    auto *session = new DirectSession; session->name = name; session->cookie = cookie; session->proposer = proposer; session->timer.setSingleShot(true);
    session->connection = new DirectConnection(cookie, client->screenName(), owner);
    QObject::connect(session->connection, &DirectConnection::connected, owner, [this, name] { imageConnected(name); });
    QObject::connect(session->connection, &DirectConnection::connectFailed, owner, [this, name] { imageConnectFailed(name); });
    QObject::connect(session->connection, &DirectConnection::closed, owner, [this, name] { closeImageFor(name, false); });
    QObject::connect(session->connection, &DirectConnection::messageReceived, owner, [this, name](const QByteArray &payload, quint16 encoding, quint8 flags) { MessageWindow *window = open(name); playAimSound(AimSound::ImReceive); window->appendDirect(name, payload, encoding, flags); window->showWindow(); });
    QObject::connect(session->connection, &DirectConnection::typingChanged, owner, [this, name](quint8 flags) { if (MessageWindow *window = byRecipient.value(normalizedName(name))) window->setPeerTyping(flags); });
    images.insert(normalizedName(name), session); return session;
  }
  void dropSession(const QString &name) {
    DirectSession *session = images.take(normalizedName(name)); if (!session) return;
    closeStatus(session); session->timer.stop();
    if (session->connection) { session->connection->disconnect(owner); session->connection->close(); session->connection->deleteLater(); }
    if (MessageWindow *window = byRecipient.value(normalizedName(name))) window->setDirect(nullptr);
    delete session;
  }
  void startImage(MessageWindow *window) {
    const QString name = window->recipient(); if (name.isEmpty() || !client || !client->connected()) return;
    if (DirectSession *existing = images.value(normalizedName(name))) { if (existing->connection && existing->connection->isConnected()) return; }
    for (DirectSession *other : images) if (!other->connection || !other->connection->isConnected()) { rendezvousBox(window, 1775, name); return; } // STRING 1775: one pending connection at a time
#ifdef Q_OS_WIN
    // RT_DIALOG 228 "Start IM Images Connection" (title STRING 1008) unless IM\IMDirectNoStartDlg.
    if (!QSettings().value(QStringLiteral("IM/IMDirectNoStartDlg"), 0).toInt()) {
      bool connect = false, dontShow = false;
      runOriginalDialog(hwnd(window), 228, [&](HWND dialog) { const QString title = QString(aimString(1008)).replace(QStringLiteral("%s"), name); SetWindowTextW(dialog, reinterpret_cast<LPCWSTR>(title.utf16())); },
        [&](HWND dialog, int id, int) { if (id == 831 || id == 832 || id == IDCANCEL) { connect = id == 831; dontShow = IsDlgButtonChecked(dialog, 825) == BST_CHECKED; EndDialog(dialog, id); return true; } return false; });
      if (dontShow) QSettings().setValue(QStringLiteral("IM/IMDirectNoStartDlg"), 1);
      if (!connect) return;
    }
#endif
    const quint64 cookie = QRandomGenerator::global()->generate64();
    DirectSession *session = newSession(name, cookie, true);
    if (!session->connection->listen()) { dropSession(name); rendezvousBox(window, 1025, name); return; } // DirectListen on 4443
    client->sendRendezvous(name, rendezvous(0, cookie));
    showStatus(session, 805);
    QObject::connect(&session->timer, &QTimer::timeout, owner, [this, name] { rendezvousBox(ownerFor(name), 1022, name); cancelImage(name, 1); });
    session->timer.start(300000); // proposal timer 300 s
  }
  void cancelImage(const QString &name, quint16 reason) {
    DirectSession *session = images.value(normalizedName(name)); if (!session) return;
    aim::oscar::Rendezvous rv = rendezvous(1, session->cookie); QByteArray value(2, 0); qToBigEndian(reason, value.data()); rv.values = {{0x0b, value}};
    if (client && client->connected() && !(session->connection && session->connection->isConnected())) client->sendRendezvous(name, rv);
    dropSession(name);
  }
  void closeImageFor(const QString &name, bool local) {
    DirectSession *session = images.value(normalizedName(name)); if (!session) return;
    const bool wasConnected = session->connection && (session->connection->isConnected() || !local);
    dropSession(name);
    if (MessageWindow *window = byRecipient.value(normalizedName(name)); window && wasConnected) window->appendPresenceNotice(QString(aimString(658)).replace(QStringLiteral("%s"), name)); // STRING 658
  }
  void imageConnected(const QString &name) {
    DirectSession *session = images.value(normalizedName(name)); if (!session) return;
    session->timer.stop(); closeStatus(session);
    MessageWindow *window = open(name); window->setDirect(session->connection); window->showWindow();
    window->appendPresenceNotice(QString(aimString(1024)).replace(QStringLiteral("%s"), name)); // STRING 1024 "%s is now directly connected"
  }
  void imageConnectFailed(const QString &name) {
    DirectSession *session = images.value(normalizedName(name)); if (!session) return;
    if (session->proposer || session->reverse) return; // the proposer keeps listening for its own timer
    // Reverse connect (event 0xA at the acceptor): listen on 4443, send ACCEPT, wait 30 s.
    session->reverse = true;
    if (!session->connection->listen()) { rendezvousBox(ownerFor(name), 1025, name); cancelImage(name, 3); return; }
    client->sendRendezvous(name, rendezvous(2, session->cookie)); showStatus(session, 834);
    session->timer.disconnect(); QObject::connect(&session->timer, &QTimer::timeout, owner, [this, name] { rendezvousBox(ownerFor(name), 1025, name); cancelImage(name, 3); });
    session->timer.start(30000);
  }
  static QList<QHostAddress> addresses(const aim::oscar::Rendezvous &rv) {
    QList<QHostAddress> out; for (quint16 tag : {quint16(4), quint16(3), quint16(2)}) for (const auto &tlv : rv.values) if (tlv.tag == tag && tlv.value.size() == 4) { const QHostAddress a(qFromBigEndian<quint32>(tlv.value.constData())); if (!a.isNull() && !out.contains(a)) out.append(a); }
    return out;
  }
  void incomingRendezvous(const aim::oscar::Rendezvous &rv) {
    if (rv.capability != aim::oscar::capDirectIm()) return;
    const QString name = rv.sender; DirectSession *session = images.value(normalizedName(name));
    if (rv.type == 1) { // cancel: notice by reason (TLV 0x0B)
      if (!session || session->cookie != rv.cookie) return;
      quint16 reason = 0xffff; for (const auto &tlv : rv.values) if (tlv.tag == 0x0b && tlv.value.size() == 2) reason = qFromBigEndian<quint16>(tlv.value.constData());
      const quint32 id = !session->proposer ? 1015 : reason == 1 ? 1017 : reason == 2 ? 1016 : reason == 5 ? 1019 : reason == 0 ? 1020 : reason == 7 ? 1347 : 1015;
      dropSession(name); rendezvousBox(ownerFor(name), id, name); return;
    }
    if (rv.type == 2) { // ACCEPT: the acceptor could not reach us and listens itself; connect to it (we keep listening too)
      if (!session || session->cookie != rv.cookie || !session->proposer) return;
      session->connection->connectTo(addresses(rv)); return;
    }
    if (rv.type != 0) return;
    if (session) { if (session->cookie == rv.cookie) return; cancelImage(name, 2); } // glare/duplicate: keep the newest
    // Accept policy (prefs page 276): buddies 767 allow / 770 approve / 895 deny, others 768 / 769 / 765.
    bool buddy = false; for (const auto &item : client->roster()) buddy = buddy || (item.classId == 0 && normalizedName(item.name) == normalizedName(name));
    const bool allow = buddy ? prefs::checked(276, 767) : prefs::checked(276, 768), deny = buddy ? prefs::checked(276, 895) : prefs::checked(276, 765);
    auto reject = [&](quint16 reason) { aim::oscar::Rendezvous answer = rendezvous(1, rv.cookie); QByteArray value(2, 0); qToBigEndian(reason, value.data()); answer.values = {{0x0b, value}}; client->sendRendezvous(name, answer); };
    if (deny) { reject(2); return; }
    if (!allow) {
#ifdef Q_OS_WIN
      // RT_DIALOG 229 "Receive IM Images Connection" (title STRING 1009): Accept 792, Reject 2, Ignore 799, Warn 793.
      int choice = 0; MessageWindow *window = open(name);
      runOriginalDialog(hwnd(window), 229, [&](HWND dialog) { const QString title = QString(aimString(1009)).replace(QStringLiteral("%s"), name); SetWindowTextW(dialog, reinterpret_cast<LPCWSTR>(title.utf16())); },
        [&](HWND dialog, int id, int) { if (id == 792 || id == IDCANCEL || id == 799 || id == 793) { choice = id; EndDialog(dialog, id); return true; } return false; });
      if (choice == 793) { userActions::warn(window, client, name); reject(1); return; }
      if (choice == 799) { reject(2); return; }
      if (choice != 792) { reject(1); return; }
#endif
    }
    // Accept: connect to the verified address (TLV 4), then TLV 3, port 4443; no ACCEPT is sent on this path.
    DirectSession *accepted = newSession(name, rv.cookie, false);
    open(name)->showWindow(); showStatus(accepted, 833);
    accepted->connection->connectTo(addresses(rv));
  }
  void resetPending(const QString &reason = QString()) { for (const auto &window : windows) if (window) window->resetPending(reason); activeAttempt.clear(); }
  void shutdown() { for (const QString &name : images.keys()) dropSession(images.value(name)->name); for (const auto &window : windows) if (window) { window->recipientChanged = {}; window->sendStarted = {}; window->sendFinished = {}; window->openMessage = {}; delete window.data(); } windows.clear(); byRecipient.clear(); activeAttempt.clear(); }
};

MessagingWindows::MessagingWindows(OscarClient *client, QWindow *owner, QObject *parent) : QObject(parent), state_(std::make_unique<State>(this, client, owner)) {}
MessagingWindows::~MessagingWindows() { if (state_) state_->shutdown(); }
void MessagingWindows::openMessage(const QString &recipient) { if (state_) state_->open(recipient); }
void MessagingWindows::setChatHandler(std::function<void(const QString &)> handler) { if (state_) state_->chatHandler = std::move(handler); }
void MessagingWindows::previewConversation() { if (state_) state_->open(QStringLiteral("edward"))->preview(); }
