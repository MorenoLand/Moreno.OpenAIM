#pragma once
#include <QString>

class OscarClient;
class QWindow;

// Buddy actions shared by the IM window, Buddy List and Buddy Info windows (Research/buddy_info.md sections 5-7).
namespace userActions {
// RT_DIALOG 142 "Send Warning" -> SNAC(04,08); result shown with STRING 526 / 527 / 528.
void warn(QWindow *owner, OscarClient *client, const QString &screenName);
// STRING 529 question -> feedbag deny item.
void block(QWindow *owner, OscarClient *client, const QString &screenName);
// RT_DIALOG 248 "Add Buddy" (group choice, RT_DIALOG 249 "New Group") -> feedbag buddy insert.
void addBuddy(QWindow *owner, OscarClient *client, const QString &screenName);
void infoBox(QWindow *owner, const QString &text);
}
