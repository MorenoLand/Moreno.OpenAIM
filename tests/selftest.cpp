// Headless live self-test for the OSCAR client. Signs two dedicated test accounts on
// to a real server and exercises messaging, user info, chat and buddy list flows.
// Credentials are read at runtime from a "screenname<TAB>password" file and never printed.
#include "client.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QTextStream>
#include <functional>

using aim::oscar::ChatInvitation;
using aim::oscar::ChatRoom;
using aim::oscar::UserInfo;

namespace {
constexpr int kTimeoutMs = 20000;
const char kDefaultAccounts[] = "G:/Development/C++/AIM/Research/test-accounts.local.txt";
const char kHost[] = "login.oscar.moreno.land";
constexpr quint16 kPort = 5190;
const QString kGroupName = QStringLiteral("OpenAIM Selftest");

QTextStream out(stdout);
int failures = 0;

void pass(const QString &name) { out << "PASS " << name << Qt::endl; }
void fail(const QString &name, const QString &reason) { ++failures; out << "FAIL " << name << ": " << reason << Qt::endl; }
void skip(const QString &name, const QString &reason) { out << "SKIP " << name << ": " << reason << Qt::endl; }
void check(const QString &name, bool ok, const QString &reason) { if (ok) pass(name); else fail(name, reason); }

// Spins the event loop until the condition holds or the timeout expires.
bool waitFor(const std::function<bool()> &condition, int timeoutMs = kTimeoutMs) {
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() > timeoutMs) return false;
    QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 25);
  }
  return true;
}
void settle(int ms) { waitFor([] { return false; }, ms); }

// Records every signal of one client so scenarios can inspect what happened.
struct Recorder {
  struct Message { QString from; QString text; };
  QString name;
  OscarClient client;
  QVector<int> stages;
  QStringList failures;
  QVector<Message> messages;
  QStringList accepted;
  QVector<UserInfo> infos;
  QVector<QPair<QString, QString>> opFailures;
  QVector<ChatRoom> roomsReady;
  QHash<QString, QStringList> participants;
  QVector<Message> chatMessages;
  QVector<ChatInvitation> invitations;
  QStringList closedRooms;
  QVector<bool> rosterEdits;
  int rosterReadyCount = 0;

  explicit Recorder(const QString &screenName) : name(screenName) {
    QObject *ctx = &client;
    QObject::connect(&client, &OscarClient::loginStageChanged, ctx, [this](int s) { stages.append(s); });
    QObject::connect(&client, &OscarClient::failed, ctx, [this](const QString &r) { failures.append(r); });
    QObject::connect(&client, &OscarClient::rosterReady, ctx, [this] { ++rosterReadyCount; });
    QObject::connect(&client, &OscarClient::messageReceived, ctx, [this](const QString &f, const QString &t) { messages.append({f, t}); });
    QObject::connect(&client, &OscarClient::messageAccepted, ctx, [this](const QString &r, quint64) { accepted.append(r); });
    QObject::connect(&client, &OscarClient::userInfoReceived, ctx, [this](const UserInfo &i) { infos.append(i); });
    QObject::connect(&client, &OscarClient::operationFailed, ctx, [this](const QString &o, const QString &r) { opFailures.append({o, r}); });
    QObject::connect(&client, &OscarClient::chatRoomReady, ctx, [this](const ChatRoom &r) { roomsReady.append(r); });
    QObject::connect(&client, &OscarClient::chatParticipantsChanged, ctx, [this](const QString &c, const QVector<UserInfo> &p) {
      QStringList names;
      for (const auto &u : p) names.append(u.screenName.toCaseFolded());
      participants.insert(c, names);
    });
    QObject::connect(&client, &OscarClient::chatMessageReceived, ctx, [this](const QString &, const QString &s, const QString &t) { chatMessages.append({s, t}); });
    QObject::connect(&client, &OscarClient::chatInvitationReceived, ctx, [this](const ChatInvitation &i) { invitations.append(i); });
    QObject::connect(&client, &OscarClient::chatRoomClosed, ctx, [this](const QString &c, const QString &) { closedRooms.append(c); });
    QObject::connect(&client, &OscarClient::rosterEditFinished, ctx, [this](bool ok) { rosterEdits.append(ok); });
  }
  QString lastOpFailure() const { return opFailures.isEmpty() ? QStringLiteral("no operationFailed") : opFailures.last().first + " / " + opFailures.last().second; }
};

bool same(const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) == 0; }

QString rosterSignature(const OscarClient &client) {
  QStringList keys;
  for (const auto &item : client.roster()) {
    // The test group is compared separately; a leftover empty one must not mask other changes.
    if (item.classId == 1 && item.groupId != 0 && same(item.name, kGroupName)) continue;
    keys.append(QStringLiteral("%1:%2:%3:%4").arg(item.classId).arg(item.groupId).arg(item.itemId).arg(item.name));
  }
  keys.sort();
  return keys.join('|');
}

bool loadAccounts(const QString &path, const QString &user1, const QString &user2, QString *pass1, QString *pass2) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
  while (!file.atEnd()) {
    const QStringList parts = QString::fromUtf8(file.readLine()).trimmed().split('\t');
    if (parts.size() < 2) continue;
    if (same(parts[0], user1)) *pass1 = parts[1];
    if (same(parts[0], user2)) *pass2 = parts[1];
  }
  return !pass1->isEmpty() && !pass2->isEmpty();
}

bool stagesInOrder(const QVector<int> &stages) {
  return stages.size() >= 3 && stages[0] == 1 && stages[1] == 2 && stages[2] == 3;
}

// Waits for the next rosterEditFinished after an edit call and reports whether it succeeded.
bool rosterEdit(Recorder &r, const std::function<bool()> &edit) {
  const qsizetype before = r.rosterEdits.size();
  if (!edit()) return false;
  if (!waitFor([&] { return r.rosterEdits.size() > before; })) return false;
  return r.rosterEdits.last();
}

quint16 findGroup(const OscarClient &client, const QString &name) {
  for (const auto &item : client.roster()) if (item.classId == 1 && item.groupId != 0 && same(item.name, name)) return item.groupId;
  return 0;
}
quint16 findBuddy(const OscarClient &client, quint16 group, const QString &name) {
  for (const auto &item : client.roster()) if (item.classId == 0 && item.groupId == group && same(item.name, name)) return item.itemId;
  return 0;
}

void scenarioSignOn(Recorder &a, Recorder &b, const QString &host, const QString &pa, const QString &pb) {
  a.client.signOn(host, kPort, a.name, pa);
  b.client.signOn(host, kPort, b.name, pb);
  for (Recorder *r : {&a, &b}) {
    const QString n = "signon " + r->name;
    const bool ready = waitFor([r] { return r->rosterReadyCount > 0 || !r->failures.isEmpty(); });
    if (!ready) fail(n, QStringLiteral("timeout waiting for rosterReady (stages seen: %1)").arg(r->stages.size()));
    else if (!r->failures.isEmpty()) fail(n, "failed(): " + r->failures.last());
    else if (!stagesInOrder(r->stages)) fail(n, "loginStageChanged did not begin 1,2,3");
    else if (!r->client.connected()) fail(n, "rosterReady but connected() is false");
    else pass(n + " (stages 1,2,3, rosterReady)");
  }
}

void scenarioMessaging(Recorder &a, Recorder &b) {
  const QString ascii = QStringLiteral("hello from selftest 1");
  const bool sent = a.client.sendMessage(b.name, ascii);
  const bool got = waitFor([&] { return !b.messages.isEmpty() && !a.accepted.isEmpty(); });
  if (!sent) fail("im ascii 1->2", "sendMessage returned false: " + a.lastOpFailure());
  else if (!got) fail("im ascii 1->2", QStringLiteral("timeout (accepted=%1 received=%2) %3").arg(a.accepted.size()).arg(b.messages.size()).arg(a.lastOpFailure()));
  else if (!same(a.accepted.last(), b.name)) fail("im ascii 1->2", "messageAccepted recipient was " + a.accepted.last());
  else if (b.messages.last().text != ascii || !same(b.messages.last().from, a.name)) fail("im ascii 1->2", "received text/sender mismatch: " + b.messages.last().from);
  else pass("im ascii 1->2 (accepted + exact text)");

  const QString unicode = QString::fromUtf8("h\xC3\xA9llo \xE2\x9C\x93 \xE6\x97\xA5\xE6\x9C\xAC");
  const qsizetype seen = a.messages.size();
  b.accepted.clear();
  const bool sent2 = b.client.sendMessage(a.name, unicode);
  const bool got2 = waitFor([&] { return a.messages.size() > seen; });
  if (!sent2) fail("im unicode 2->1", "sendMessage returned false: " + b.lastOpFailure());
  else if (!got2) fail("im unicode 2->1", "timeout: " + b.lastOpFailure());
  else if (a.messages.last().text != unicode) fail("im unicode 2->1", "text did not round-trip: got " + QString::number(a.messages.last().text.size()) + " chars, expected " + QString::number(unicode.size()));
  else pass("im unicode 2->1 (exact round-trip)");

  const qsizetype failed = a.opFailures.size();
  a.client.sendMessage(QStringLiteral("openaimtest_nobody_zz"), QStringLiteral("ping"));
  const bool err = waitFor([&] { return a.opFailures.size() > failed; });
  check("im to nonexistent user -> operationFailed", err, "no operationFailed within timeout");
  if (err) out << "     (reason: " << a.opFailures.last().second << ")" << Qt::endl;
}

void scenarioUserInfo(Recorder &a, Recorder &b) {
  a.client.requestUserInfo(b.name);
  const bool got = waitFor([&] { return !a.infos.isEmpty(); });
  if (!got) fail("user info 1->2", "timeout: " + a.lastOpFailure());
  else check("user info 1->2", same(a.infos.last().screenName, b.name), "screen name was " + a.infos.last().screenName);
}

void scenarioChat(Recorder &a, Recorder &b) {
  const QString roomName = QStringLiteral("selftest%1").arg(QRandomGenerator::global()->bounded(100000));
  a.client.createChatRoom(roomName, 4);
  if (!waitFor([&] { return !a.roomsReady.isEmpty(); })) { fail("chat create room", "timeout: " + a.lastOpFailure()); return; }
  const ChatRoom room = a.roomsReady.last();
  pass("chat create room (exchange 4)");

  if (!waitFor([&] { return a.participants.value(room.cookie).contains(a.name.toCaseFolded()); })) { fail("chat creator participant", "creator never listed"); return; }
  const bool invited = a.client.inviteToChat(b.name, room.cookie, QStringLiteral("selftest invitation"));
  if (!invited || !waitFor([&] { return !b.invitations.isEmpty(); })) { fail("chat invite", "no invitation received: " + a.lastOpFailure()); return; }
  pass("chat invite delivered");
  const ChatInvitation inv = b.invitations.last();
  if (inv.room.cookie != room.cookie || !same(inv.sender, a.name)) fail("chat invite contents", "room cookie or sender mismatch");
  else pass("chat invite contents");

  b.client.respondToChatInvitation(inv, true);
  if (!waitFor([&] { return !b.roomsReady.isEmpty(); })) { fail("chat accept/join", "timeout: " + b.lastOpFailure()); return; }
  pass("chat accept/join");
  const QString cookie = room.cookie;
  const bool both = waitFor([&] { return a.participants.value(cookie).size() >= 2 && b.participants.value(cookie).size() >= 2; });
  check("chat both see participants", both, QStringLiteral("a sees %1, b sees %2").arg(a.participants.value(cookie).join(',')).arg(b.participants.value(cookie).join(',')));

  const QString m1 = QStringLiteral("chat from 1");
  const bool s1 = a.client.sendChatMessage(cookie, m1);
  const bool r1 = waitFor([&] { return !b.chatMessages.isEmpty() && !a.chatMessages.isEmpty(); });
  if (!s1 || !r1) fail("chat message 1->2", "send=" + QString::number(s1) + " received/reflected timeout: " + a.lastOpFailure());
  else check("chat message 1->2 (+reflection)", b.chatMessages.last().text == m1 && same(b.chatMessages.last().from, a.name) && a.chatMessages.last().text == m1,
             "text or sender mismatch");
  const qsizetype na = a.chatMessages.size(), nb = b.chatMessages.size();
  const QString m2 = QString::fromUtf8("chat from 2 \xE2\x9C\x93");
  const bool s2 = b.client.sendChatMessage(cookie, m2);
  const bool r2 = waitFor([&] { return a.chatMessages.size() > na && b.chatMessages.size() > nb; });
  if (!s2 || !r2) fail("chat message 2->1", "send=" + QString::number(s2) + " received/reflected timeout: " + b.lastOpFailure());
  else check("chat message 2->1 (+reflection)", a.chatMessages.last().text == m2 && same(a.chatMessages.last().from, b.name) && b.chatMessages.last().text == m2,
             "text or sender mismatch");

  a.client.leaveChatRoom(cookie);
  check("chat leave (1)", a.closedRooms.contains(cookie), "no chatRoomClosed");
  check("chat leave seen by other", waitFor([&] { return b.participants.value(cookie).size() == 1; }, 8000), "participants still " + b.participants.value(cookie).join(','));
  b.client.leaveChatRoom(cookie);
  check("chat leave (2)", b.closedRooms.contains(cookie), "no chatRoomClosed");
}

void scenarioBuddies(Recorder &a, Recorder &b) {
  const QString before = rosterSignature(a.client);
  // Reuse the test group left over from an earlier run, otherwise create it.
  quint16 group = findGroup(a.client, kGroupName);
  if (!group && !rosterEdit(a, [&] { return a.client.addGroup(kGroupName); })) { fail("buddy add group", a.lastOpFailure()); return; }
  if (!group) group = findGroup(a.client, kGroupName);
  if (!group) { fail("buddy add group", "group missing from roster after edit"); return; }
  pass("buddy add group");

  if (!rosterEdit(a, [&] { return a.client.addBuddy(group, b.name); })) { fail("buddy add", a.lastOpFailure()); rosterEdit(a, [&] { return a.client.removeGroup(group); }); return; }
  const quint16 item = findBuddy(a.client, group, b.name);
  check("buddy add (in roster)", item != 0, "buddy missing from roster");
  check("buddy presence online", waitFor([&] { return a.client.isOnline(b.name); }, 10000), "isOnline false for " + b.name);

  bool removed = false;
  if (item) {
    removed = rosterEdit(a, [&] { return a.client.removeBuddy(group, item); });
    check("buddy remove", removed && findBuddy(a.client, group, b.name) == 0, "remove failed: " + a.lastOpFailure());
  } else {
    skip("buddy remove", "buddy was not added");
  }
  const bool groupGone = rosterEdit(a, [&] { return a.client.removeGroup(group); });
  check("buddy cleanup group", groupGone && findGroup(a.client, kGroupName) == 0, "group removal failed: " + a.lastOpFailure());
  check("roster unchanged after cleanup", rosterSignature(a.client) == before,
        "before=" + before + " after=" + rosterSignature(a.client));
}

void scenarioSignOff(Recorder &a, Recorder &b) {
  a.client.signOff();
  b.client.signOff();
  settle(1500);
  for (Recorder *r : {&a, &b}) {
    const QString n = "signoff " + r->name;
    if (r->stages.isEmpty() || r->stages.last() != 0) fail(n, "loginStageChanged(0) not last");
    else if (!r->failures.isEmpty()) fail(n, "stray failed(): " + r->failures.last());
    else pass(n);
  }
}
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const QString path = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString::fromLatin1(kDefaultAccounts);
  const QString user1 = QStringLiteral("openaimtest1"), user2 = QStringLiteral("openaimtest2");
  QString pass1, pass2;
  if (!loadAccounts(path, user1, user2, &pass1, &pass2)) {
    out << "FAIL setup: could not read both test accounts from the credentials file" << Qt::endl;
    return 1;
  }
  Recorder a(user1), b(user2);
  scenarioSignOn(a, b, QString::fromLatin1(kHost), pass1, pass2);
  pass1.fill(QLatin1Char(0));
  pass2.fill(QLatin1Char(0));
  if (!a.client.connected() || !b.client.connected()) {
    skip("remaining scenarios", "sign-on failed");
  } else {
    scenarioMessaging(a, b);
    scenarioUserInfo(a, b);
    scenarioChat(a, b);
    scenarioBuddies(a, b);
  }
  scenarioSignOff(a, b);
  out << "SUMMARY failures=" << failures << Qt::endl;
  return failures;
}
