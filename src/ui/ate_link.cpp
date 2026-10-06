#include "ate_link.h"
#include "ctl_group.h"
#include "native_dialog.h"
#include "ate_toolbar.h"
#include "preferences.h"
#include <QTextImageFormat>
#include <QDesktopServices>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QUrl>
#include <QWindow>

namespace {
constexpr int HtmlSizeProperty = QTextFormat::UserProperty + 1; // HTML font size 1..7 of a run (IM compose pane)
QString aimString(quint32 id) { return aimEnvironment().string(id); }
QString appTitle() { return QStringLiteral("AOL Instant Messenger (SM)"); }

// The link run around `position` inside its block: [start, end) of contiguous fragments with the same href.
bool linkRange(const QTextDocument &document, int position, int *start, int *end, QString *href) {
  const QTextBlock block = document.findBlock(position); if (!block.isValid()) return false;
  QVector<QTextFragment> fragments; for (auto it = block.begin(); !it.atEnd(); ++it) if (it.fragment().isValid()) fragments.append(it.fragment());
  for (int i = 0; i < fragments.size(); ++i) {
    const QTextFragment &f = fragments[i];
    if (!f.charFormat().isAnchor() || position < f.position() || position >= f.position() + f.length()) continue;
    const QString url = f.charFormat().anchorHref(); int first = i, last = i;
    while (first > 0 && fragments[first - 1].charFormat().isAnchor() && fragments[first - 1].charFormat().anchorHref() == url) --first;
    while (last + 1 < fragments.size() && fragments[last + 1].charFormat().isAnchor() && fragments[last + 1].charFormat().anchorHref() == url) ++last;
    *start = fragments[first].position(); *end = fragments[last].position() + fragments[last].length(); *href = url; return true;
  }
  return false;
}
QTextCharFormat linkFormat(const QString &url) {
  QTextCharFormat format; format.setAnchor(true); format.setAnchorHref(url); format.setFontUnderline(true); format.setForeground(QColor(0, 0, 255)); return format;
}
QTextCharFormat plainFormat(const QTextCharFormat &from) {
  QTextCharFormat format = from; format.setAnchor(false); format.clearProperty(QTextFormat::AnchorHref); format.setFontUnderline(false); format.clearForeground(); return format;
}
}

namespace ate {
QString html(const QTextDocument &document, const QColor &background) {
  QString body;
  for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
    if (block != document.begin()) body += QStringLiteral("<BR>");
    QString openHref; bool inLink = false;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
      const QTextFragment fragment = it.fragment(); if (!fragment.isValid()) continue;
      const QTextCharFormat f = fragment.charFormat();
      if (inLink && (!f.isAnchor() || f.anchorHref() != openHref)) { body += QStringLiteral("</A>"); inLink = false; }
      if (f.isAnchor() && !inLink) { openHref = f.anchorHref(); body += QStringLiteral("<A HREF=\"%1\">").arg(openHref); inLink = true; } // the URL is not escaped (quotes are refused by the dialog)
      QString open, close; QStringList font;
      // A link's own blue underline is implied by <A>.
      if (!f.isAnchor() && f.foreground().style() != Qt::NoBrush && f.foreground().color() != Qt::black) font << QStringLiteral("COLOR=\"%1\"").arg(f.foreground().color().name());
      if (f.background().style() != Qt::NoBrush) font << QStringLiteral("BACK=\"%1\"").arg(f.background().color().name());
      if (f.hasProperty(QTextFormat::FontFamilies) && !f.fontFamilies().toStringList().isEmpty()) font << QStringLiteral("FACE=\"%1\"").arg(f.fontFamilies().toStringList().first());
      const int size = f.hasProperty(HtmlSizeProperty) ? f.intProperty(HtmlSizeProperty) : 3; if (size != 3) font << QStringLiteral("SIZE=%1").arg(size);
      if (!font.isEmpty()) { open += QStringLiteral("<FONT %1>").arg(font.join(QLatin1Char(' '))); close.prepend(QStringLiteral("</FONT>")); }
      if (f.fontWeight() >= QFont::Bold) { open += QStringLiteral("<B>"); close.prepend(QStringLiteral("</B>")); }
      if (f.fontItalic()) { open += QStringLiteral("<I>"); close.prepend(QStringLiteral("</I>")); }
      if (f.fontUnderline() && !f.isAnchor()) { open += QStringLiteral("<U>"); close.prepend(QStringLiteral("</U>")); }
      body += open + fragment.text().toHtmlEscaped() + close;
    }
    if (inLink) body += QStringLiteral("</A>");
  }
  return QStringLiteral("<HTML><BODY BGCOLOR=\"%1\">%2</BODY></HTML>").arg(background.name(), body);
}

void insertSmileys(QTextDocument &document, int from) {
  if (!prefs::graphicalSmileys()) return;
  for (int glyph = 0; glyph < 16; ++glyph) {
    const QString code = AteToolbar::smileyCode(glyph); const QUrl url(QStringLiteral("aim-smiley:%1").arg(glyph));
    if (document.resource(QTextDocument::ImageResource, url).isNull()) document.addResource(QTextDocument::ImageResource, url, AteToolbar::smileyImage(glyph));
    for (QTextCursor found = document.find(code, from); !found.isNull(); found = document.find(code, found)) {
      if (found.charFormat().isAnchor()) continue;
      QTextImageFormat image; image.setName(url.toString()); image.setVerticalAlignment(QTextCharFormat::AlignMiddle); found.insertImage(image);
    }
  }
}
void openUrl(const QString &url) {
  QString target = url.trimmed(); if (target.isEmpty()) return;
  if (!target.contains(QStringLiteral("://")) && !target.startsWith(QStringLiteral("aim:"), Qt::CaseInsensitive) && !target.startsWith(QStringLiteral("mailto:"), Qt::CaseInsensitive)) target.prepend(QStringLiteral("http://"));
  QDesktopServices::openUrl(QUrl::fromUserInput(target));
}

bool editLink(QWindow *owner, QTextCursor &cursor, bool readOnly) {
#ifdef Q_OS_WIN
  HWND ownerWindow = owner && owner->handle() ? reinterpret_cast<HWND>(owner->winId()) : nullptr;
  auto box = [&](HWND parent, quint32 id, UINT style) { return MessageBoxW(parent, reinterpret_cast<LPCWSTR>(aimString(id).utf16()), reinterpret_cast<LPCWSTR>(appTitle().utf16()), style); };
  QTextDocument &document = *cursor.document();
  // Existing link: caret inside a link with no selection, or a selection that is exactly one whole link (ate32 0x12019796).
  int start = 0, end = 0; QString href; bool existing = false;
  if (!cursor.hasSelection()) existing = linkRange(document, cursor.position(), &start, &end, &href) || (cursor.position() > 0 && linkRange(document, cursor.position() - 1, &start, &end, &href));
  else {
    const int from = cursor.selectionStart(), to = cursor.selectionEnd(); bool anyLink = false, anyPlain = false;
    for (int p = from; p < to; ++p) { int s, e; QString h; if (linkRange(document, p, &s, &e, &h)) { anyLink = true; if (!existing && s == from && e == to) { existing = true; start = s; end = e; href = h; } } else anyPlain = true; }
    if (anyLink && (anyPlain || !existing)) { box(ownerWindow, 774, MB_OK | MB_ICONEXCLAMATION); return false; }
  }
  const int dialogId = !existing ? 122 : readOnly ? 120 : 121;
  if (readOnly && !existing) return false;
  QString url = href, text;
  auto valid = [&](HWND dialog, bool hasText) {
    url = nativeWindowText(GetDlgItem(dialog, 267)); if (hasText) text = nativeWindowText(GetDlgItem(dialog, 270));
    if (url.isEmpty() || (hasText && text.isEmpty())) { box(dialog, hasText ? 432 : 431, MB_OK | MB_ICONEXCLAMATION); return false; }
    if (url.contains(QLatin1Char('"'))) { box(dialog, 1346, MB_OK | MB_ICONEXCLAMATION); return false; }
    static const QString forbidden = QStringLiteral(" <>[]{}\\^`");
    for (const QChar c : url) if (forbidden.contains(c)) return box(dialog, 793, MB_OKCANCEL | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDOK; // 0x12022f56
    return true;
  };
  const INT_PTR result = runOriginalDialog(ownerWindow, dialogId,
    [&](HWND dialog) { SetDlgItemTextW(dialog, 267, reinterpret_cast<LPCWSTR>(href.utf16())); SendDlgItemMessageW(dialog, 267, EM_LIMITTEXT, 0x103, 0); SendDlgItemMessageW(dialog, 270, EM_LIMITTEXT, 0x103, 0); },
    [&](HWND dialog, int id, int) {
      const bool hasText = dialogId == 122;
      switch (id) {
      case IDOK: if (dialogId != 120 && valid(dialog, hasText)) EndDialog(dialog, 1); return true;
      case 268: if (dialogId == 120) { EndDialog(dialog, 2); return true; } if (valid(dialog, hasText)) EndDialog(dialog, 2); return true; // Launch
      case 269: EndDialog(dialog, 0); return true;                                    // Remove Link
      case 721: box(dialog, 1828, MB_OK | MB_ICONEXCLAMATION); return true;          // Use Current: no DDE-capable browser page to read
      case IDCANCEL: EndDialog(dialog, -1); return true;
      default: return false;
      }
    });
  if (result < 0) return false;
  if (dialogId == 120) { if (result == 2) openUrl(href); return false; }
  if (result == 0) { QTextCursor c(&document); c.setPosition(start); c.setPosition(end, QTextCursor::KeepAnchor); c.mergeCharFormat([] { QTextCharFormat f; f.setAnchor(false); f.setAnchorHref(QString()); f.setFontUnderline(false); f.setForeground(QColor(Qt::black)); return f; }()); return true; }
  if (existing) { QTextCursor c(&document); c.setPosition(start); c.setPosition(end, QTextCursor::KeepAnchor); QTextCharFormat f; f.setAnchor(true); f.setAnchorHref(url); c.mergeCharFormat(f); }
  else {
    const QTextCharFormat base = cursor.charFormat();
    QTextCharFormat format = base; format.merge(linkFormat(url));
    cursor.insertText(text.isEmpty() ? QStringLiteral("X") : text, format); // inserted at the caret, replacing the selection
    cursor.setCharFormat(plainFormat(base));
  }
  if (result == 2) openUrl(url);
  return true;
#else
  Q_UNUSED(owner); Q_UNUSED(cursor); Q_UNUSED(readOnly); return false;
#endif
}
}
