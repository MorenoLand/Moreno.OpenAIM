#include "messaging_window.h"
#include <QAbstractTextDocumentLayout>
#include <QBackingStore>
#include <QClipboard>
#include <QEvent>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QIcon>
#include <QInputMethodEvent>
#include <QHash>
#include <QList>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QRegion>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QWheelEvent>
#include <QWindow>
#include <functional>
#include <limits>
#include <utility>

namespace {
QFont aimFont(bool bold = false) { QFont font(QStringLiteral("MS Sans Serif"), 8, bold ? QFont::Bold : QFont::Normal); return font; }
QString normalizedName(QString name) { name.remove(QLatin1Char(' ')); return name.toCaseFolded(); }
bool isHtml(const QString &text) { return text.contains(QRegularExpression(QStringLiteral("</?[A-Za-z!][^>]*>"))); }
class TextEditor {
public:
  TextEditor() : cursor(&document) { document.setDefaultFont(aimFont()); document.setDocumentMargin(0); document.setUndoRedoEnabled(true); }
  QString text() const { return document.toPlainText(); }
  void setText(const QString &value) { document.setPlainText(value); cursor = QTextCursor(&document); cursor.movePosition(QTextCursor::End); }
  void clear() { setText(QString()); }
  bool handleKey(QKeyEvent *event, bool singleLine, bool submitOnEnter, bool *submit) {
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    const bool command = modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::MetaModifier);
    if (command && event->key() == Qt::Key_A) { cursor.select(QTextCursor::Document); return true; }
    if (command && (event->key() == Qt::Key_C || event->key() == Qt::Key_X)) { if (!cursor.hasSelection()) return true; QGuiApplication::clipboard()->setText(cursor.selectedText()); if (event->key() == Qt::Key_X) cursor.removeSelectedText(); return true; }
    if (command && event->key() == Qt::Key_V) { QString value = QGuiApplication::clipboard()->text(); if (singleLine) value.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")), QStringLiteral(" ")); cursor.insertText(value); return true; }
    if (event->key() == Qt::Key_Backspace) { cursor.deletePreviousChar(); return true; }
    if (event->key() == Qt::Key_Delete) { cursor.deleteChar(); return true; }
    if (event->key() == Qt::Key_Left) { cursor.movePosition(QTextCursor::PreviousCharacter, modifiers.testFlag(Qt::ShiftModifier) ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor); return true; }
    if (event->key() == Qt::Key_Right) { cursor.movePosition(QTextCursor::NextCharacter, modifiers.testFlag(Qt::ShiftModifier) ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor); return true; }
    if (event->key() == Qt::Key_Home) { cursor.movePosition(QTextCursor::StartOfLine, modifiers.testFlag(Qt::ShiftModifier) ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor); return true; }
    if (event->key() == Qt::Key_End) { cursor.movePosition(QTextCursor::EndOfLine, modifiers.testFlag(Qt::ShiftModifier) ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor); return true; }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) { if (submitOnEnter && !modifiers.testFlag(Qt::ShiftModifier)) { if (submit) *submit = true; } else if (!singleLine) cursor.insertText(QStringLiteral("\n")); return true; }
    if (!event->text().isEmpty() && !modifiers.testFlag(Qt::ControlModifier) && !modifiers.testFlag(Qt::AltModifier) && !modifiers.testFlag(Qt::MetaModifier)) { QString value = event->text(); if (singleLine) value.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")), QStringLiteral(" ")); cursor.insertText(value); return true; }
    return false;
  }
  void input(const QString &value, bool singleLine) { QString text = value; if (singleLine) text.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")), QStringLiteral(" ")); cursor.insertText(text); }
  QTextDocument document;
  QTextCursor cursor;
  qreal scroll = 0;
};
class Transcript {
public:
  Transcript() { document.setDefaultFont(aimFont()); document.setDocumentMargin(0); }
  void append(const QString &sender, const QString &text, bool html) {
    QTextCursor cursor(&document); cursor.movePosition(QTextCursor::End); if (!document.isEmpty()) cursor.insertBlock(); QTextCharFormat name; name.setFontWeight(QFont::Bold); cursor.insertText(sender, name); cursor.insertBlock(); if (html) cursor.insertHtml(text); else cursor.insertText(text); cursor.insertBlock(); followBottom = true;
  }
  void appendHtml(const QString &html) { QTextCursor cursor(&document); cursor.movePosition(QTextCursor::End); if (!document.isEmpty()) cursor.insertBlock(); cursor.insertHtml(html); cursor.insertBlock(); }
  QTextDocument document;
  qreal scroll = 0;
  bool followBottom = true;
};
void drawButton(QPainter &painter, const QRect &rect, const QString &text, bool enabled, bool pressed) {
  painter.fillRect(rect, pressed ? QColor(220, 220, 220) : QColor(250, 250, 250)); painter.setPen(QColor(90, 90, 90)); painter.drawRect(rect); painter.setPen(enabled ? QColor(20, 20, 20) : QColor(140, 140, 140)); painter.setFont(aimFont()); painter.drawText(rect, Qt::AlignCenter, text);
}
void drawField(QPainter &painter, const QRect &rect, TextEditor &editor, bool focused) {
  painter.fillRect(rect, Qt::white); painter.setPen(QColor(128, 128, 128)); painter.drawRect(rect); editor.document.setTextWidth(100000); const QString value = editor.text(); const QFont font = aimFont(); const QFontMetricsF metrics(font); const qsizetype position = qBound(qsizetype(0), editor.cursor.position(), qsizetype(value.size())); const qreal caretX = metrics.horizontalAdvance(value.left(position)); const qreal scroll = qMax(qreal(0), caretX - rect.width() + 14); painter.save(); painter.setClipRect(rect.adjusted(1, 1, -1, -1)); painter.setFont(font); painter.setPen(QColor(20, 20, 20)); const qreal x = rect.left() + 5 - scroll; const qreal baseline = rect.center().y() + (metrics.ascent() - metrics.descent()) / 2; painter.drawText(QPointF(x, baseline), value); if (focused) painter.drawLine(QPointF(rect.left() + 5 + caretX - scroll, rect.top() + 4), QPointF(rect.left() + 5 + caretX - scroll, rect.bottom() - 4)); painter.restore();
}
void drawEditor(QPainter &painter, const QRect &rect, TextEditor &editor, bool focused) {
  painter.fillRect(rect, Qt::white); painter.setPen(QColor(128, 128, 128)); painter.drawRect(rect); const QRect view = rect.adjusted(5, 4, -5, -4); editor.document.setTextWidth(qMax(1, view.width())); const QSizeF documentSize = editor.document.documentLayout()->documentSize(); const QTextBlock block = editor.cursor.block(); const QTextLayout *layout = block.layout(); const int blockPosition = editor.cursor.position() - block.position(); const QTextLine line = layout ? layout->lineForTextPosition(blockPosition) : QTextLine(); const QRectF blockRect = editor.document.documentLayout()->blockBoundingRect(block); const qreal caretY = line.isValid() ? blockRect.top() + line.y() + line.height() : blockRect.bottom(); editor.scroll = qBound(qreal(0), caretY - view.height() + 4, qMax(qreal(0), documentSize.height() - view.height())); painter.save(); painter.setClipRect(view); painter.translate(view.left(), view.top() - editor.scroll); QAbstractTextDocumentLayout::PaintContext context; context.clip = QRectF(0, editor.scroll, view.width(), view.height()); if (editor.cursor.hasSelection()) { QAbstractTextDocumentLayout::Selection selection; selection.cursor = editor.cursor; selection.format.setBackground(QColor(0, 120, 215)); selection.format.setForeground(Qt::white); context.selections.append(selection); } editor.document.documentLayout()->draw(&painter, context); if (focused && line.isValid()) { const qreal x = line.cursorToX(blockPosition); const qreal y = blockRect.top() + line.y(); painter.setPen(QColor(20, 20, 20)); painter.drawLine(QPointF(blockRect.left() + x, y), QPointF(blockRect.left() + x, y + line.height())); } painter.restore();
}
void drawDocument(QPainter &painter, const QRect &rect, QTextDocument &document, qreal scroll) {
  painter.fillRect(rect, Qt::white); painter.setPen(QColor(128, 128, 128)); painter.drawRect(rect); const QRect view = rect.adjusted(5, 4, -5, -4); document.setTextWidth(qMax(1, view.width())); document.documentLayout()->documentSize(); painter.save(); painter.setClipRect(view); painter.translate(view.left(), view.top() - scroll); document.drawContents(&painter, QRectF(0, scroll, view.width(), view.height())); painter.restore();
}
void positionCursor(TextEditor &editor, const QRect &rect, const QPoint &point, bool singleLine, qreal scroll = 0) {
  if (singleLine) { const QFontMetricsF metrics(aimFont()); const QString value = editor.text(); int best = 0; qreal distance = std::numeric_limits<qreal>::max(); for (int i = 0; i <= value.size(); ++i) { qreal x = metrics.horizontalAdvance(value.left(i)); qreal d = qAbs(point.x() - (rect.left() + 5 + x - scroll)); if (d < distance) { best = i; distance = d; } } editor.cursor.setPosition(best); return; }
  const QRect view = rect.adjusted(5, 4, -5, -4); editor.document.setTextWidth(qMax(1, view.width())); editor.document.documentLayout()->documentSize(); const QPointF at(point.x() - view.left(), point.y() - view.top() + scroll); const int position = editor.document.documentLayout()->hitTest(at, Qt::FuzzyHit); if (position >= 0) editor.cursor.setPosition(position);
}
void drawTranscript(QPainter &painter, const QRect &rect, Transcript &transcript) {
  painter.fillRect(rect, Qt::white); painter.setPen(QColor(128, 128, 128)); painter.drawRect(rect); const QRect view = rect.adjusted(5, 4, -5, -4); transcript.document.setTextWidth(qMax(1, view.width())); const qreal maxScroll = qMax(qreal(0), transcript.document.documentLayout()->documentSize().height() - view.height()); transcript.scroll = transcript.followBottom ? maxScroll : qBound(qreal(0), transcript.scroll, maxScroll); painter.save(); painter.setClipRect(view); painter.translate(view.left(), view.top() - transcript.scroll); transcript.document.drawContents(&painter, QRectF(0, transcript.scroll, view.width(), view.height())); painter.restore();
}
class MessageWindow final : public QWindow {
public:
  MessageWindow(OscarClient *client, QWindow *transientParent, QObject *owner, const QString &recipient) : QWindow(), backingStore_(this), client_(client), transcript_() {
    QObject::setParent(owner); setSurfaceType(QSurface::RasterSurface); setFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint); setIcon(QIcon(QStringLiteral(":/aim/window-icon.ico"))); setTransientParent(transientParent); const qreal dialogUnit = QFontMetricsF(aimFont()).averageCharWidth() / 4.0; resize(qRound(280 * dialogUnit), 400); setMinimumSize(QSize(qRound(280 * dialogUnit), 320)); recipient_.setText(recipient); focus_ = recipient.trimmed().isEmpty() ? 0 : 1; updateTitle();
  }
  std::function<void(const QString &)> recipientChanged;
  std::function<void(MessageWindow *)> sendStarted;
  std::function<void(MessageWindow *)> sendFinished;
  QString recipient() const { return recipient_.text().trimmed(); }
  QString pendingRecipient() const { return pendingRecipient_; }
  bool isSending() const { return sending_; }
  bool acknowledge(const QString &recipient) {
    if (!sending_ || normalizedName(recipient) != normalizedName(pendingRecipient_)) return false;
    transcript_.append(client_ ? client_->screenName() : QStringLiteral("You"), pendingText_, false); if (compose_.text().isEmpty()) compose_.clear(); pendingText_.clear(); pendingRecipient_.clear(); sending_ = false; status_.clear(); renderNow(); return true;
  }
  void sendFailed(const QString &reason) {
    if (!sending_) return; const QString current = compose_.text(); compose_.setText(current.isEmpty() ? pendingText_ : pendingText_ + QStringLiteral("\n") + current); pendingText_.clear(); pendingRecipient_.clear(); sending_ = false; status_ = reason; renderNow();
  }
  void recordOperationFailure(const QString &message) { if (sendAttempt_) attemptError_ = message; else if (sending_) sendFailed(message); }
  void resetPending(const QString &reason = QString()) { const bool hadPending = sending_; if (sending_) { const QString current = compose_.text(); compose_.setText(current.isEmpty() ? pendingText_ : pendingText_ + QStringLiteral("\n") + current); pendingText_.clear(); pendingRecipient_.clear(); sending_ = false; } if (!reason.isEmpty()) status_ = reason; else if (hadPending) status_.clear(); renderNow(); }
  void appendIncoming(const QString &sender, const QString &text) { transcript_.append(sender, text, isHtml(text)); status_.clear(); renderNow(); }
  void showWindow() { show(); raise(); requestActivate(); }
protected:
  bool event(QEvent *event) override {
    if (event->type() == QEvent::Close) { event->ignore(); hide(); deleteLater(); return true; }
    if (event->type() == QEvent::Expose || event->type() == QEvent::UpdateRequest) { renderNow(); return true; }
    if (event->type() == QEvent::InputMethod) { auto *input = static_cast<QInputMethodEvent *>(event); if (!input->commitString().isEmpty()) { (focus_ == 0 ? recipient_ : compose_).input(input->commitString(), focus_ == 0); if (focus_ == 0) { updateTitle(); if (recipientChanged) recipientChanged(recipient()); } renderNow(); } event->accept(); return true; }
    return QWindow::event(event);
  }
  void resizeEvent(QResizeEvent *event) override { backingStore_.resize(event->size()); renderNow(); }
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton) return; const QPoint point = event->position().toPoint(); if (recipientRect().contains(point)) { focus_ = 0; positionCursor(recipient_, recipientRect(), point, true); } else if (composeRect().contains(point)) { focus_ = 1; positionCursor(compose_, composeRect(), point, false, compose_.scroll); } else if (sendRect().contains(point)) pressedSend_ = true; renderNow();
  }
  void mouseReleaseEvent(QMouseEvent *event) override { if (event->button() != Qt::LeftButton) return; const bool shouldSend = pressedSend_ && sendRect().contains(event->position().toPoint()); pressedSend_ = false; if (shouldSend) send(); renderNow(); }
  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Escape) { close(); return; }
    if (event->key() == Qt::Key_Tab) { focus_ = 1 - focus_; renderNow(); return; }
    bool submit = false; TextEditor &editor = focus_ == 0 ? recipient_ : compose_; if (editor.handleKey(event, focus_ == 0, focus_ == 1, &submit)) { if (focus_ == 0) { updateTitle(); if (recipientChanged) recipientChanged(recipient()); } if (submit) send(); renderNow(); event->accept(); return; } QWindow::keyPressEvent(event);
  }
  void wheelEvent(QWheelEvent *event) override { if (transcriptRect().contains(event->position().toPoint())) transcript_.scroll = qMax(qreal(0), transcript_.scroll - event->angleDelta().y() / 2.0); else if (composeRect().contains(event->position().toPoint())) compose_.scroll = qMax(qreal(0), compose_.scroll - event->angleDelta().y() / 2.0); else { event->ignore(); return; } renderNow(); event->accept(); }
private:
  QRect recipientRect() const { return QRect(57, 9, qMax(100, width() - 69), 23); }
  QRect transcriptRect() const { return QRect(10, 41, qMax(100, width() - 20), qMax(80, height() - 174)); }
  QRect composeRect() const { return QRect(10, height() - 119, qMax(100, width() - 20), 76); }
  QRect sendRect() const { return QRect(width() - 94, height() - 34, 84, 24); }
  void updateTitle() { const QString name = recipient(); setTitle(name.isEmpty() ? QStringLiteral("Instant Message") : QStringLiteral("%1 - Instant Message").arg(name)); }
  void send() {
    const QString target = recipient(), text = compose_.text(); if (sending_ || target.isEmpty() || text.trimmed().isEmpty()) return; sendAttempt_ = true; attemptError_.clear(); if (sendStarted) sendStarted(this); const bool queued = client_ && client_->sendMessage(target, text); if (sendFinished) sendFinished(this); sendAttempt_ = false; if (!queued || !attemptError_.isEmpty()) { status_ = attemptError_.isEmpty() ? QStringLiteral("Unable to queue message.") : attemptError_; attemptError_.clear(); renderNow(); return; }
    pendingRecipient_ = target; pendingText_ = text; sending_ = true; compose_.clear(); status_.clear(); renderNow();
  }
  void renderNow() {
    if (!isExposed() || width() <= 0 || height() <= 0) return; const QRegion region(0, 0, width(), height()); backingStore_.beginPaint(region); QPainter painter(backingStore_.paintDevice()); painter.fillRect(QRect(0, 0, width(), height()), QColor(240, 240, 240)); painter.setFont(aimFont()); painter.setPen(QColor(20, 20, 20)); painter.drawText(QPoint(11, 25), QStringLiteral("To:")); drawField(painter, recipientRect(), recipient_, focus_ == 0); drawTranscript(painter, transcriptRect(), transcript_); drawEditor(painter, composeRect(), compose_, focus_ == 1); drawButton(painter, sendRect(), QStringLiteral("Send"), !sending_, pressedSend_); if (!status_.isEmpty()) { painter.setFont(aimFont()); painter.setPen(QColor(150, 0, 0)); painter.drawText(QRect(12, height() - 30, width() - 115, 22), Qt::AlignVCenter | Qt::TextSingleLine, status_); } painter.end(); backingStore_.endPaint(); backingStore_.flush(region);
  }
  QBackingStore backingStore_;
  OscarClient *client_ = nullptr;
  TextEditor recipient_;
  TextEditor compose_;
  Transcript transcript_;
  QString pendingRecipient_;
  QString pendingText_;
  QString status_;
  QString attemptError_;
  int focus_ = 1;
  bool sending_ = false;
  bool sendAttempt_ = false;
  bool pressedSend_ = false;
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
    QObject::connect(client, &OscarClient::messageReceived, owner, [this](const QString &sender, const QString &text) { MessageWindow *window = open(sender); window->appendIncoming(sender, text); window->showWindow(); });
    QObject::connect(client, &OscarClient::messageAccepted, owner, [this](const QString &recipient, quint64) { for (const auto &window : windows) if (window && window->acknowledge(recipient)) return; });
    QObject::connect(client, &OscarClient::operationFailed, owner, [this](const QString &operation, const QString &reason) { const QString message = QStringLiteral("%1: %2").arg(operation, reason); if (activeAttempt) { activeAttempt->recordOperationFailure(message); return; } for (const auto &window : windows) if (window && window->isSending() && operation.startsWith(QStringLiteral("IM to "), Qt::CaseInsensitive) && normalizedName(operation.mid(6)) == normalizedName(window->pendingRecipient())) { window->sendFailed(message); return; } });
    QObject::connect(client, &OscarClient::failed, owner, [this](const QString &reason) { resetPending(reason); });
    QObject::connect(client, &OscarClient::loginStageChanged, owner, [this](int stage) { if (stage == 0) resetPending(); });
  }
  MessageWindow *open(const QString &recipient) {
    const QString key = normalizedName(recipient); if (byRecipient.contains(key) && byRecipient.value(key)) { MessageWindow *window = byRecipient.value(key); window->showWindow(); return window; }
    auto *window = new MessageWindow(client, transientParent, owner, recipient); byRecipient.insert(key, window); windows.append(window);
    window->recipientChanged = [this, window](const QString &name) { for (auto it = byRecipient.begin(); it != byRecipient.end();) { if (it.value() == window) it = byRecipient.erase(it); else ++it; } byRecipient.insert(normalizedName(name), window); };
    window->sendStarted = [this](MessageWindow *surface) { activeAttempt = surface; };
    window->sendFinished = [this](MessageWindow *surface) { if (activeAttempt == surface) activeAttempt.clear(); };
    QObject::connect(window, &QObject::destroyed, owner, [this, window] { for (auto it = byRecipient.begin(); it != byRecipient.end();) { if (it.value().isNull() || it.value().data() == window) it = byRecipient.erase(it); else ++it; } if (activeAttempt.data() == window) activeAttempt.clear(); });
    window->showWindow(); return window;
  }
  void resetPending(const QString &reason = QString()) { for (const auto &window : windows) if (window) window->resetPending(reason); activeAttempt.clear(); }
  void shutdown() { for (const auto &window : windows) if (window) { window->recipientChanged = {}; window->sendStarted = {}; window->sendFinished = {}; delete window.data(); } windows.clear(); byRecipient.clear(); activeAttempt.clear(); }
};

MessagingWindows::MessagingWindows(OscarClient *client, QWindow *owner, QObject *parent) : QObject(parent), state_(std::make_unique<State>(this, client, owner)) {}
MessagingWindows::~MessagingWindows() { if (state_) state_->shutdown(); }
void MessagingWindows::openMessage(const QString &recipient) { if (state_) state_->open(recipient); }
