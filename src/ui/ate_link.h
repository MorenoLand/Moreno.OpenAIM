#pragma once
#include <QString>

class QTextCursor;
class QTextDocument;
class QWindow;

// Pieces of ate32.dll shared by every ATE compose pane (Research/remaining_buttons.md section 3).
namespace ate {
// HTML the ATE serializer sends: <FONT>/<B>/<I>/<U> runs, <A HREF="url">text</A> links and <BR> between lines.
QString html(const QTextDocument &document);
// Toolbar Link button (0x26B): RT_DIALOG 122 (new link), 121 (edit an existing link) or 120 (read-only pane).
// Returns true when the document changed.
bool editLink(QWindow *owner, QTextCursor &cursor, bool readOnly = false);
// External browser, as browse.ocm's handler: scheme-less addresses ("www.aol.com/...") get http://.
void openUrl(const QString &url);
}
