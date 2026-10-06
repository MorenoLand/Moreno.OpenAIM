#pragma once
#include <QClipboard>
#include <QFont>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QTextCursor>
#include <QTextDocument>

// Editable text of an ATE pane or edit control: a QTextDocument with a cursor and the original key handling
// (Enter submits unless Shift/Ctrl is held; Ctrl+A/C/X/V).
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

