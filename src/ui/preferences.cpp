#include "preferences.h"
#include <QHash>
#include <QSettings>
#include <QStringList>
#include "../oscar/client.h"
#include <cmath>

namespace prefs {
namespace {
QString key(int page, int control) { return QStringLiteral("nativePreferences/%1/%2").arg(page).arg(control); }
constexpr int HtmlSizeProperty = QTextFormat::UserProperty + 1; // same property the IM compose pane uses
}
namespace { QHash<QString, QVariant> &cache() { static QHash<QString, QVariant> values; return values; } }
// Values are read through a cache: the Buddy List repaints often (ticker) and QSettings is backed by the registry.
QVariant stored(const QString &name, const QVariant &fallback) {
  auto it = cache().find(name); if (it == cache().end()) it = cache().insert(name, QSettings().value(name));
  return it->isValid() ? *it : fallback;
}
void invalidate() { cache().clear(); }
QVariant value(int page, int control, const QVariant &fallback) { return stored(key(page, control), fallback); }
bool checked(int page, int control, bool fallback) { const QVariant v = value(page, control); return v.isValid() ? v.toInt() != 0 : fallback; }

bool enterInsertsReturn() { return checked(288, 353); }
bool tabInsertsTab() { return checked(288, 354); }
bool alwaysTimestamp() { return checked(288, 355); }
bool graphicalSmileys() { return !checked(288, 356); }
bool acceptMessageDialog() { return checked(288, 357); }
bool imSignOnOffNotices() { return checked(288, 1053); }
bool chatFlash() { return checked(288, 683); }
bool chatAnnouncements() { return checked(288, 672, true); }
bool blockChatInvitations() { return checked(288, 657); }
qreal textMagnification() {
  if (checked(288, 513)) return 2.0;
  if (checked(288, 512)) return 1.33;
  if (checked(288, 510)) return 0.75;
  return 1.0;
}
QFont composeFont() {
  QFont font;
  font.setFamily(stored(key(288, 352) + QStringLiteral("/face"), QStringLiteral("Times New Roman")).toString());
  const qreal points = stored(key(288, 352) + QStringLiteral("/points"), 12).toReal();
  font.setPixelSize(qMax(1, int(std::lround(points * 4.0 / 3.0)))); // 96 dpi, as the IM panes are laid out in pixels
  font.setBold(stored(key(288, 352) + QStringLiteral("/bold"), false).toBool());
  font.setItalic(stored(key(288, 352) + QStringLiteral("/italic"), false).toBool());
  font.setUnderline(stored(key(288, 352) + QStringLiteral("/underline"), false).toBool());
  return font;
}
QColor composeTextColor() { const QColor c(stored(key(288, 352) + QStringLiteral("/color"), QStringLiteral("#000000")).toString()); return c.isValid() ? c : QColor(Qt::black); }
QColor composeWindowColor() { const QColor c(stored(key(288, 352) + QStringLiteral("/background"), QStringLiteral("#ffffff")).toString()); return c.isValid() ? c : QColor(Qt::white); }
int htmlSizeForPoints(qreal points) { static const qreal sizes[] = {8, 10, 12, 14, 18, 24, 36}; int best = 3; qreal distance = 1e9; for (int i = 0; i < 7; ++i) if (std::abs(sizes[i] - points) < distance) { distance = std::abs(sizes[i] - points); best = i + 1; } return best; }
QTextCharFormat composeFormat() {
  QTextCharFormat format; const QFont font = composeFont();
  format.setFontFamilies({font.family()}); format.setFontWeight(font.bold() ? QFont::Bold : QFont::Normal); format.setFontItalic(font.italic()); format.setFontUnderline(font.underline());
  const qreal points = stored(key(288, 352) + QStringLiteral("/points"), 12).toReal();
  format.setProperty(HtmlSizeProperty, htmlSizeForPoints(points)); format.setFontPointSize(points);
  if (composeTextColor() != QColor(Qt::black)) format.setForeground(composeTextColor());
  return format;
}

QFont buddyListFont() {
  const QString family = value(277, 232, QStringLiteral("Arial")).toString();
  const int points = value(277, 233, 9).toInt();
  QFont font(family.isEmpty() ? QStringLiteral("Arial") : family); font.setPixelSize(qMax(6, int(std::lround(points * 4.0 / 3.0)))); return font;
}
bool dimIdleBuddies() { return checked(277, 234, true); }
int dimIdleMinutes() { bool ok = false; const int minutes = value(277, 235, 10).toInt(&ok); return ok && minutes > 0 ? minutes : 10; }
bool flashOnSignOnOff() { return checked(277, 237); }
bool hideTaskbarWhenMinimized() { return checked(277, 1490); }
bool signOffWhenClosed() { return checked(277, 1491); }
bool dockable() { return checked(277, 820); }

bool awayAutoRespond() { return !checked(274, 416); }
bool hideWindowsWhileAway() { return checked(274, 743); }
bool idleAutoRespond() { return checked(273, 413); }
QString idleMessage() { return value(273, 414).toString(); }

namespace {
// "Who can contact me" radio buttons and their PDINFO modes. "Block AIM users only" (1122) has no counterpart on an
// AIM-only service and is sent as "Block all users".
const std::pair<int, quint8> PrivacyRadios[] = {{239, 1}, {241, 2}, {243, 3}, {242, 4}, {240, 5}, {1122, 2}};
QStringList listNames(int control) { QStringList names; for (const QVariant &v : QSettings().value(key(292, control) + QStringLiteral("/items")).toList()) { const QString n = v.toMap().value(QStringLiteral("label")).toString().trimmed(); if (!n.isEmpty()) names.append(n); } return names; }
QString nameKey(QString n) { n.remove(QLatin1Char(' ')); return n.toCaseFolded(); }
bool sameNames(QStringList a, QStringList b) { for (QString &n : a) n = nameKey(n); for (QString &n : b) n = nameKey(n); a.sort(); b.sort(); a.removeDuplicates(); b.removeDuplicates(); return a == b; }
}
void privacyFromServer(OscarClient *client) {
  if (!client || !client->connected() || client->rosterEditPending()) return;
  QSettings settings; const quint8 mode = client->privacyMode(); bool matched = false;
  for (const auto &[control, value] : PrivacyRadios) { const bool on = !matched && value == mode && control != 1122; settings.setValue(key(292, control), on ? 1 : 0); matched = matched || on; }
  auto store = [&](int control, const QStringList &names) { QVariantList items; for (const QString &n : names) { QVariantMap item; item[QStringLiteral("label")] = n; item[QStringLiteral("382")] = n; items.append(item); } settings.setValue(key(292, control) + QStringLiteral("/items"), items); };
  store(245, client->privacyList(2)); store(247, client->privacyList(3));
  invalidate();
}
void privacyToServer(OscarClient *client) {
  if (!client || !client->connected()) return;
  quint8 mode = client->privacyMode(); for (const auto &[control, value] : PrivacyRadios) if (checked(292, control)) { mode = value; break; }
  const QStringList allow = listNames(245), block = listNames(247);
  if (mode == client->privacyMode() && sameNames(allow, client->privacyList(2)) && sameNames(block, client->privacyList(3))) return;
  client->setPrivacy(mode, allow, block);
}

bool showStockTicker() { return checked(284, 917, true); }
}
