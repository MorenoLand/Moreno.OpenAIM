#pragma once
#include <QColor>
#include <QFont>
#include <QTextCharFormat>
#include <QVariant>

// Typed access to the values the original Preferences pages store (NativePreferences keeps them as
// nativePreferences/<RT_DIALOG page>/<control id>). Defaults are what a fresh install shows.
class OscarClient;
namespace prefs {
QVariant value(int page, int control, const QVariant &fallback = {});
void invalidate(); // call after writing preferences (NativePreferences::apply does)
bool checked(int page, int control, bool fallback = false);

// IM/Chat (RT_DIALOG 288)
bool enterInsertsReturn();          // 353: Enter = new line, Ctrl+Enter sends
bool tabInsertsTab();               // 354
bool alwaysTimestamp();             // 355
bool graphicalSmileys();            // !356
bool acceptMessageDialog();         // 357
bool imSignOnOffNotices();          // 1053
bool chatFlash();                   // 683
bool chatAnnouncements();           // 672
bool blockChatInvitations();        // 657
qreal textMagnification();          // 513/512/511/510 = 2.0 / 1.33 / 1.0 / 0.75
// "Defaults for Composing Windows" (ATE 352): font, font colour and window colour.
QFont composeFont();
QColor composeTextColor();
QColor composeWindowColor();
QTextCharFormat composeFormat();    // char format new compose text starts with
int htmlSizeForPoints(qreal points); // HTML <FONT SIZE> 1..7 nearest to a point size

// Buddy List (RT_DIALOG 277)
QFont buddyListFont();              // 232 family / 233 points
bool dimIdleBuddies(); int dimIdleMinutes(); // 234 / 235
bool flashOnSignOnOff();            // 237
bool hideTaskbarWhenMinimized();    // 1490
bool signOffWhenClosed();           // 1491
bool dockable();                    // 820

// Away / Idle (RT_DIALOG 274 / 273)
bool awayAutoRespond();             // 415 (else 416: profile only)
bool hideWindowsWhileAway();        // 743
bool idleAutoRespond();             // 413
QString idleMessage();              // ATE 414 text

// Privacy (RT_DIALOG 292) mirrors the server's feedbag: the radio buttons are the PDINFO mode, the lists the
// permit (245) and deny (247) entries.
void privacyFromServer(OscarClient *client);
void privacyToServer(OscarClient *client);

// Stock ticker (RT_DIALOG 284)
bool showStockTicker();             // 917
}
