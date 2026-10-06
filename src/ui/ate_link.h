#pragma once
#include <QColor>
#include <QByteArray>
#include <QList>
#include <QString>

class QTextCursor;
class QTextDocument;
class QWindow;

// Pieces of ate32.dll shared by every ATE compose pane (Research/remaining_buttons.md section 3).
namespace ate {
// HTML the ATE serializer sends: <FONT>/<B>/<I>/<U> runs, <A HREF="url">text</A> links and <BR> between lines.
QString html(const QTextDocument &document, const QColor &background = QColor(255, 255, 255), QList<QByteArray> *images = nullptr);
// IM Image frames (Research/direct_im.md 2.3): ATE HTML with <IMG SRC ID WIDTH HEIGHT DATASIZE> tags, then
// <BINARY><DATA ID SIZE>raw file bytes</DATA>...</BINARY>. Text is sent as ASCII/ISO-8859-1 (encoding 0 or 3);
// other characters become numeric references.
QByteArray directPayload(const QTextDocument &document, const QColor &background, quint16 *encoding);
// Decodes a received frame into HTML whose images are resources added to `target`.
QString directHtml(const QByteArray &payload, quint16 encoding, QTextDocument &target);
// Insert > Image (0x265): RT_STRING 386 title, 385 filter; inserts the picture at the caret. Returns true if inserted.
bool insertPicture(QWindow *owner, QTextCursor &cursor);
bool hasImages(const QTextDocument &document);
// Toolbar Link button (0x26B): RT_DIALOG 122 (new link), 121 (edit an existing link) or 120 (read-only pane).
// Returns true when the document changed.
bool editLink(QWindow *owner, QTextCursor &cursor, bool readOnly = false);
// External browser, as browse.ocm's handler: scheme-less addresses ("www.aol.com/...") get http://.
void openUrl(const QString &url);
// Replaces smiley codes after `from` with the strip 1008 glyphs (unless Preferences > IM/Chat disables them).
void insertSmileys(QTextDocument &document, int from);
}
