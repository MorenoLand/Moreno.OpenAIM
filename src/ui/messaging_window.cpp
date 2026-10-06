#include "messaging_window.h"
#include "ctl_group.h"
#include "menu_bar.h"
#include "sounds.h"
#include "ate_toolbar.h"
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

class TextEditor {
public:
  explicit TextEditor(const QFont &font) : cursor(&document) { document.setDefaultFont(font); document.setDocumentMargin(0); document.setUndoRedoEnabled(true); }
  QString text() const { return document.toPlainText(); }
  void setText(const QString &value) { document.setPlainText(value); cursor = QTextCursor(&document); cursor.movePosition(QTextCursor::End); }
  void clear() { setText(QString()); }
  bool handleKey(QKeyEvent *event, bool singleLine, bool *submit) {
    const Qt::KeyboardModifiers modifiers = event->modifiers(); const bool command = modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::MetaModifier);
    const QTextCursor::MoveMode mode = modifiers.testFlag(Qt::ShiftModifier) ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;
    if (command && event->key() == Qt::Key_A) { cursor.select(QTextCursor::Document); return true; }
    if (command && (event->key() == Qt::Key_C || event->key() == Qt::Key_X)) { copy(event->key() == Qt::Key_X); return true; }
    if (command && event->key() == Qt::Key_V) { paste(singleLine); return true; }
    switch (event->key()) {
    case Qt::Key_Backspace: cursor.deletePreviousChar(); return true;
    case Qt::Key_Delete: cursor.deleteChar(); return true;
    case Qt::Key_Left: cursor.movePosition(QTextCursor::PreviousCharacter, mode); return true;
    case Qt::Key_Right: cursor.movePosition(QTextCursor::NextCharacter, mode); return true;
    case Qt::Key_Up: if (!singleLine) cursor.movePosition(QTextCursor::Up, mode); return true;
    case Qt::Key_Down: if (!singleLine) cursor.movePosition(QTextCursor::Down, mode); return true;
    case Qt::Key_Home: cursor.movePosition(QTextCursor::StartOfLine, mode); return true;
    case Qt::Key_End: cursor.movePosition(QTextCursor::EndOfLine, mode); return true;
    case Qt::Key_Return: case Qt::Key_Enter:
      if (!singleLine && (modifiers.testFlag(Qt::ShiftModifier) || modifiers.testFlag(Qt::ControlModifier))) cursor.insertText(QStringLiteral("\n")); else if (submit) *submit = true;
      return true;
    default: break;
    }
    if (!event->text().isEmpty() && !command && !modifiers.testFlag(Qt::AltModifier) && event->text().at(0).isPrint()) { input(event->text(), singleLine); return true; }
    return false;
  }
  void copy(bool cut) { if (!cursor.hasSelection()) return; QGuiApplication::clipboard()->setText(cursor.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'))); if (cut) cursor.removeSelectedText(); }
  void paste(bool singleLine) { input(QGuiApplication::clipboard()->text(), singleLine); }
  void input(const QString &value, bool singleLine) { QString text = value; if (singleLine) text.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")), QStringLiteral(" ")); cursor.insertText(text); }
  QTextDocument document;
  QTextCursor cursor;
  qreal scroll = 0;
};

// History pane: AppendMsg header "<b>Name</b>:" in red (self) or blue (buddy), "<br>" before every later message.
class Transcript {
public:
  Transcript() { document.setDefaultFont(ateFont()); document.setDocumentMargin(2); }
  void appendMessage(const QString &name, const QString &body, bool self) {
    const QString color = self ? QStringLiteral("#ff0000") : QStringLiteral("#0000ff");
    const QString separator = body.trimmed().isEmpty() ? QStringLiteral(".") : QStringLiteral(":");
    QTextCursor cursor(&document); cursor.movePosition(QTextCursor::End);
    if (!first_) cursor.insertBlock();
    cursor.insertHtml(QStringLiteral("<font color=%1><b>%2</b>%3</font>&nbsp;").arg(color, name.toHtmlEscaped(), separator));
    cursor.insertHtml(body); first_ = false; followBottom = true;
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
      : WindowBase(QStringLiteral("Instant Message"), defaultCanvas()), client_(client), recipient_(QFont(QStringLiteral("MS Sans Serif"), 8)), compose_(ateFont()), menu_(101), group_(loadCtlGroup(103)) {
    QObject::setParent(owner); Q_UNUSED(transientParent);
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
  void resetPending(const QString &reason = QString()) { if (!sending_) return; restorePending(); if (!reason.isEmpty()) appendFailure(reason); requestUpdate(); }
  void appendIncoming(const QString &sender, const QString &text) { transcript_.appendMessage(sender, isHtml(text) ? text : text.toHtmlEscaped(), false); setMode(Conversation); requestUpdate(); }
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
    // VALUERES 109/110: 380x240 outer window, also the minimum track size (0x1138cda0, 0x1138d7ef).
    QSize outer(380, 240);
#ifdef Q_OS_WIN
    RECT frame{0, 0, 0, 0}; AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
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
  void updateTitle() { const QString name = recipient(); setTitle(name.isEmpty() ? aimString(517) : QStringLiteral("%1 - %2").arg(name, aimString(517))); } // STRING 515 "%s - %s" with STRING 517
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
    p.fillRect(r, Qt::white); p.setPen(PaneBorder); p.drawRect(r.adjusted(0, 0, -1, -1));
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
    transcript_.document.setTextWidth(qMax(1, view.width()));
    const qreal maxScroll = qMax(qreal(0), transcript_.document.documentLayout()->documentSize().height() - view.height());
    transcript_.scroll = transcript_.followBottom ? maxScroll : qBound(qreal(0), transcript_.scroll, maxScroll);
    if (transcript_.scroll >= maxScroll) transcript_.followBottom = true;
    p.save(); p.setClipRect(view); p.translate(view.left(), view.top() - transcript_.scroll); { QAbstractTextDocumentLayout::PaintContext context; context.clip = QRectF(0, transcript_.scroll, view.width(), view.height()); context.palette = atePalette(); p.setClipRect(QRectF(0, transcript_.scroll, view.width(), view.height()), Qt::IntersectClip); transcript_.document.documentLayout()->draw(&p, context); } p.restore();
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
  void edited() { if (focus_ == 0) { if (recipientChanged) recipientChanged(recipient()); } requestUpdate(); }
  void openMenu(int index) {
    if (index < 0 || index >= menuRects_.size()) return;
    openMenu_ = index; requestUpdate();
    const int id = popupMenu(this, menu_.items[index].children, canvasToGlobal(menuRects_[index].bottomLeft() + QPoint(0, 1)));
    openMenu_ = -1; hoveredMenu_ = -1; requestUpdate();
    if (id) command(quint32(id));
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
      // The picker opens above the text caret (ate32 0x1201afb3).
      const QRect caret = caretRect(); const int glyph = AteToolbar::pickSmiley(this, canvasToGlobal(QPoint(caret.left(), caret.top() - 100)));
      if (glyph >= 0) { compose_.cursor.insertText(AteToolbar::smileyCode(glyph)); focus_ = 1; requestUpdate(); }
      return;
    }
    default: return; // link, IM image and greeting are not implemented yet
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
  static QString aimHtml(const QTextDocument &document) {
    // Outgoing IMs are HTML, as the ATE pane produces them: <B>/<I>/<U>/<FONT> runs and <BR> between lines.
    QString body;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
      if (block != document.begin()) body += QStringLiteral("<BR>");
      for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment(); if (!fragment.isValid()) continue;
        const QTextCharFormat f = fragment.charFormat(); QString open, close;
        QStringList font; if (f.foreground().style() != Qt::NoBrush && f.foreground().color() != Qt::black) font << QStringLiteral("COLOR=\"%1\"").arg(f.foreground().color().name());
        if (f.background().style() != Qt::NoBrush) font << QStringLiteral("BACK=\"%1\"").arg(f.background().color().name());
        if (htmlSize(f) != 3) font << QStringLiteral("SIZE=%1").arg(htmlSize(f));
        if (!font.isEmpty()) { open += QStringLiteral("<FONT %1>").arg(font.join(QLatin1Char(' '))); close.prepend(QStringLiteral("</FONT>")); }
        if (f.fontWeight() >= QFont::Bold) { open += QStringLiteral("<B>"); close.prepend(QStringLiteral("</B>")); }
        if (f.fontItalic()) { open += QStringLiteral("<I>"); close.prepend(QStringLiteral("</I>")); }
        if (f.fontUnderline()) { open += QStringLiteral("<U>"); close.prepend(QStringLiteral("</U>")); }
        body += open + fragment.text().toHtmlEscaped() + close;
      }
    }
    return QStringLiteral("<HTML><BODY BGCOLOR=\"#ffffff\">%1</BODY></HTML>").arg(body);
  }
  void command(quint32 id) {
    switch (id) {
    case Send: case 1: send(); return;
    case 2: closeRequested(); return;                                    // File > Close / IDCANCEL
    case 396: editor().copy(true); requestUpdate(); return;               // Cut
    case 397: if (focus_ == 1 && !compose_.cursor.hasSelection()) return; editor().copy(false); return; // Copy
    case 398: editor().paste(focus_ == 0); edited(); return;              // Paste
    case 399: compose_.input(QLocale::system().toString(QTime::currentTime(), QLocale::ShortFormat), false); focus_ = 1; requestUpdate(); return; // Insert > Timestamp (F2)
    default: return; // Warn, Block, Add Buddy, Talk, Get Info and the rendezvous items are not implemented yet
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
    if (!client_ || !client_->connected()) { errorBox(aimString(542)); return; }            // STRING 542
    sendAttempt_ = true; attemptError_.clear(); if (sendStarted) sendStarted(this);
    const QString html = aimHtml(compose_.document);
    const bool queued = client_->sendMessage(target, html);
    if (sendFinished) sendFinished(this); sendAttempt_ = false;
    if (!queued || !attemptError_.isEmpty()) { appendFailure(attemptError_); attemptError_.clear(); requestUpdate(); return; }
    playAimSound(AimSound::ImSend); rememberRecipient(target); pendingRecipient_ = target; pendingText_ = html; pendingPlain_ = text; sending_ = true; compose_.clear(); compose_.cursor.setCharFormat(QTextCharFormat()); updateTitle(); requestUpdate();
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
  State(MessagingWindows *ownerValue, OscarClient *clientValue, QWindow *parentValue) : owner(ownerValue), client(clientValue), transientParent(parentValue) {
    if (!client) return;
    QObject::connect(client, &OscarClient::messageReceived, owner, [this](const QString &sender, const QString &text) { const bool existing = byRecipient.value(normalizedName(sender)) != nullptr; playAimSound(existing ? AimSound::ImReceive : AimSound::ImFirstReceive); MessageWindow *window = open(sender); window->appendIncoming(sender, text); window->showWindow(); });
    QObject::connect(client, &OscarClient::messageAccepted, owner, [this](const QString &recipient, quint64) { for (const auto &window : windows) if (window && window->acknowledge(recipient)) return; });
    QObject::connect(client, &OscarClient::operationFailed, owner, [this](const QString &operation, const QString &reason) { const QString message = QStringLiteral("%1: %2").arg(operation, reason); if (activeAttempt) { activeAttempt->recordOperationFailure(message); return; } for (const auto &window : windows) if (window && window->isSending() && operation.startsWith(QStringLiteral("IM to "), Qt::CaseInsensitive) && normalizedName(operation.mid(6)) == normalizedName(window->pendingRecipient())) { window->sendFailed(message); return; } });
    QObject::connect(client, &OscarClient::failed, owner, [this](const QString &reason) { resetPending(reason); });
    QObject::connect(client, &OscarClient::loginStageChanged, owner, [this](int stage) { if (stage == 0) resetPending(); });
  }
  MessageWindow *open(const QString &recipient) {
    // One window per buddy (0x1138d434).
    const QString key = normalizedName(recipient); if (!key.isEmpty() && byRecipient.contains(key) && byRecipient.value(key)) { MessageWindow *window = byRecipient.value(key); window->showWindow(); return window; }
    int cascade = 0; for (const auto &window : windows) if (window) ++cascade;
    auto *window = new MessageWindow(client, transientParent, owner, recipient, cascade); if (!key.isEmpty()) byRecipient.insert(key, window); windows.append(window);
    window->recipientChanged = [this, window](const QString &name) { for (auto it = byRecipient.begin(); it != byRecipient.end();) { if (it.value() == window) it = byRecipient.erase(it); else ++it; } if (!name.isEmpty()) byRecipient.insert(normalizedName(name), window); };
    window->sendStarted = [this](MessageWindow *surface) { activeAttempt = surface; };
    window->sendFinished = [this](MessageWindow *surface) { if (activeAttempt == surface) activeAttempt.clear(); };
    QObject::connect(window, &QObject::destroyed, owner, [this, window] { for (auto it = byRecipient.begin(); it != byRecipient.end();) { if (it.value().isNull() || it.value().data() == window) it = byRecipient.erase(it); else ++it; } windows.removeIf([window](const QPointer<MessageWindow> &value) { return value.isNull() || value.data() == window; }); if (activeAttempt.data() == window) activeAttempt.clear(); });
    window->showWindow(); return window;
  }
  void resetPending(const QString &reason = QString()) { for (const auto &window : windows) if (window) window->resetPending(reason); activeAttempt.clear(); }
  void shutdown() { for (const auto &window : windows) if (window) { window->recipientChanged = {}; window->sendStarted = {}; window->sendFinished = {}; delete window.data(); } windows.clear(); byRecipient.clear(); activeAttempt.clear(); }
};

MessagingWindows::MessagingWindows(OscarClient *client, QWindow *owner, QObject *parent) : QObject(parent), state_(std::make_unique<State>(this, client, owner)) {}
MessagingWindows::~MessagingWindows() { if (state_) state_->shutdown(); }
void MessagingWindows::openMessage(const QString &recipient) { if (state_) state_->open(recipient); }
void MessagingWindows::previewConversation() { if (state_) state_->open(QStringLiteral("edward"))->preview(); }
