// Headless live self-test for the OSCAR client. Signs two dedicated test accounts on
// to a real server and exercises messaging, user info, chat and buddy list flows.
// Credentials are read at runtime from a "screenname<TAB>password" file and never printed.
#include "client.h"
#include "direct_connection.h"
#include "talk_call.h"
#include <cmath>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QTextStream>
#include <QtEndian>
#include <functional>

using aim::oscar::ChatInvitation;
using aim::oscar::ChatRoom;
using aim::oscar::UserInfo;
using aim::oscar::Rendezvous;
using aim::oscar::capDirectIm;

namespace {
constexpr int kTimeoutMs = 20000;
const char kHostVariable[] = "OPENAIM_TEST_HOST";
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
  QVector<quint16> warnLevels;
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
    QObject::connect(&client, &OscarClient::warnCompleted, ctx, [this](const QString &, quint16, quint16 level) { warnLevels.append(level); });
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
  a.client.sendMessage(QString(), QStringLiteral("ping"));
  const bool err = waitFor([&] { return a.opFailures.size() > failed; });
  check("im invalid recipient -> operationFailed", err, "no operationFailed within timeout");
  if (err) out << "     (reason: " << a.opFailures.last().second << ")" << Qt::endl;
}

void scenarioUserInfo(Recorder &a, Recorder &b) {
  a.client.requestUserInfo(b.name);
  const bool got = waitFor([&] { return !a.infos.isEmpty(); });
  if (!got) fail("user info 1->2", "timeout: " + a.lastOpFailure());
  else check("user info 1->2", same(a.infos.last().screenName, b.name), "screen name was " + a.infos.last().screenName);
}

void scenarioWarn(Recorder &a, Recorder &b) {
  // Anonymous warning between the two test accounts: SNAC(04,08) -> SNAC(04,09).
  const qsizetype failuresBefore = a.opFailures.size();
  a.client.warnUser(b.name, true);
  const bool got = waitFor([&] { return !a.warnLevels.isEmpty() || a.opFailures.size() > failuresBefore; });
  if (!got) fail("warn 1->2 (anonymous)", "timeout");
  else if (a.warnLevels.isEmpty()) fail("warn 1->2 (anonymous)", a.lastOpFailure());
  else check("warn 1->2 (anonymous)", a.warnLevels.last() > 0, "new level " + QString::number(a.warnLevels.last()));
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

// IM Image: propose over ICBM channel 2 through the server, the acceptor connects to port 4443, ODC2 frames both ways.
void scenarioDirectIm(Recorder &a, Recorder &b) {
  QObject context;QVector<Rendezvous> received; QObject::connect(&b.client, &OscarClient::rendezvousReceived, &context, [&](const Rendezvous &rv) { received.append(rv); });
  const quint64 cookie = 0x1122334455667788ULL;
  DirectConnection listener(cookie, a.name); if (!listener.listen()) { fail("direct im listen", "port 4443 unavailable"); return; }
  Rendezvous rv; rv.type = 0; rv.cookie = cookie; rv.capability = capDirectIm(); QByteArray port(2, 0); port[0] = char(0x14); port[1] = char(0x46);
  QByteArray ip(4, 0); ip[0] = char(127); ip[3] = char(1); rv.values = {{0x02,ip},{0x03, ip}, {0x05, port}};
  check("direct im propose sent", a.client.sendRendezvous(b.name, rv), a.lastOpFailure());
  if (!waitFor([&] { return !received.isEmpty(); })) { fail("direct im propose relayed", "no rendezvous received: " + b.lastOpFailure()); return; }
  const Rendezvous got = received.first();
  check("direct im propose contents", got.type == 0 && got.cookie == cookie && got.capability == capDirectIm() && same(got.sender, a.name), "type/cookie/capability mismatch");
  bool hasVerified = false; for (const auto &tlv : got.values) hasVerified = hasVerified || tlv.tag == 4;
  check("direct im server added verified IP", hasVerified, "no TLV 4");
  QList<QHostAddress> candidates;bool preserved=false;for(quint16 tag:{quint16(4),quint16(3),quint16(2)})for(const auto &tlv:got.values)if(tlv.tag==tag&&tlv.value.size()==4){const QHostAddress address(qFromBigEndian<quint32>(tlv.value.constData()));if(!address.isNull()&&!candidates.contains(address))candidates.append(address);if(tag==2)preserved=tlv.value==ip;}
  check("direct im LAN candidate preserved through server",preserved,QStringLiteral("rendezvous IP candidate missing or changed"));
  DirectConnection acceptor(cookie, b.name);
  bool aConnected = false, bConnected = false; QByteArray aGot, bGot; quint8 typing = 0;
  QObject::connect(&listener, &DirectConnection::connected, &listener, [&] { aConnected = true; });
  QObject::connect(&acceptor, &DirectConnection::connected, &acceptor, [&] { bConnected = true; });
  QObject::connect(&listener, &DirectConnection::messageReceived, &listener, [&](const QByteArray &p, quint16, quint8) { aGot = p; });
  QObject::connect(&acceptor, &DirectConnection::messageReceived, &acceptor, [&](const QByteArray &p, quint16, quint8) { bGot = p; });
  QObject::connect(&acceptor, &DirectConnection::typingChanged, &acceptor, [&](quint8 f) { typing = f; });
  acceptor.connectTo(candidates); // both ends run on this machine
  check("direct im connected from relayed candidates", waitFor([&] { return aConnected && bConnected; }, 65000), "ODC2 connection not established");
  QByteArray image("GIF89a\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00\xff\xff\xff,\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02D\x01\x00;", 35);
  const QByteArray payload = QByteArray("<HTML><BODY>pic <IMG SRC=\"x.gif\" ID=\"1\" WIDTH=\"1\" HEIGHT=\"1\" DATASIZE=\"35\"></BODY></HTML><BINARY><DATA ID=\"1\" SIZE=\"35\">") + image + "</DATA></BINARY>";
  listener.sendTyping(0x0E);
  check("direct im typing frame", waitFor([&] { return typing == 0x0E; }, 5000), QStringLiteral("flags %1").arg(typing));
  listener.sendMessage(payload, 0);
  check("direct im frame a->b (image intact)", waitFor([&] { return bGot == payload; }, 5000), QStringLiteral("got %1 bytes").arg(bGot.size()));
  acceptor.sendMessage(QByteArrayLiteral("<HTML><BODY>back</BODY></HTML>"), 0);
  check("direct im frame b->a", waitFor([&] { return aGot.contains("back"); }, 5000), "no frame");
  listener.close(); acceptor.close();
}

// Talk engine on this machine: callee listens, caller connects (case 1), 18-byte handshake, UDP probe, Rtv audio
// both ways, half-duplex control frames, hang up.
void scenarioTalkRendezvous(Recorder &a,Recorder &b) {
  QObject context;QVector<Rendezvous> toCaller,toCallee;QStringList errors;const quint64 cookie=QRandomGenerator::global()->generate64();
  TalkCall caller(cookie,TalkCall::Role::Caller,false),callee(cookie,TalkCall::Role::Callee,false);
  caller.audio().setSpeakerVolume(0);callee.audio().setSpeakerVolume(0);
  bool callerConnected=false,calleeConnected=false,calleeEnded=false;int callerSamples=0,calleeSamples=0;quint16 calleePort=0,callerPort=0;
  auto value=[](const Rendezvous &rv,quint16 tag){for(const auto &tlv:rv.values)if(tlv.tag==tag)return tlv.value;return QByteArray();};
  auto sequence=[&](const Rendezvous &rv){const QByteArray bytes=value(rv,0x0a);return bytes.size()==2?qFromBigEndian<quint16>(bytes.constData()):quint16(0);};
  auto addresses=[&](const Rendezvous &rv){QList<QHostAddress> result;for(quint16 tag:{quint16(3),quint16(4),quint16(2)}){const QByteArray ip=value(rv,tag);if(ip.size()==4){const QHostAddress address(qFromBigEndian<quint32>(ip.constData()));if(!address.isNull()&&!result.contains(address))result.append(address);}}return result;};
  auto send=[&](Recorder &from,Recorder &to,quint16 type,quint16 port,quint16 seq){Rendezvous rv;rv.type=type;rv.cookie=cookie;rv.capability=aim::oscar::capVoice();rv.values.append({3,QByteArray::fromHex("7f000001")});if(port){QByteArray bytes(2,0);qToBigEndian(port,bytes.data());rv.values.append({5,bytes});}if(type==0){QByteArray bytes(2,0);qToBigEndian(seq,bytes.data());rv.values.append({0x0a,bytes});rv.values.append({0x2711,QByteArray::fromHex("00000001")});}return from.client.sendRendezvous(to.name,rv);};
  QObject::connect(&caller,&TalkCall::connected,&context,[&]{callerConnected=true;caller.audio().setCapturing(false);});
  QObject::connect(&callee,&TalkCall::connected,&context,[&]{calleeConnected=true;callee.audio().setCapturing(false);});
  QObject::connect(&caller,&TalkCall::audioReceived,&context,[&](int count){callerSamples+=count;});
  QObject::connect(&callee,&TalkCall::audioReceived,&context,[&](int count){calleeSamples+=count;});
  QObject::connect(&callee,&TalkCall::ended,&context,[&](bool){calleeEnded=true;});
  QObject::connect(&caller,&TalkCall::failed,&context,[&](const QString &reason){errors.append(QStringLiteral("caller: ")+reason);});
  QObject::connect(&callee,&TalkCall::failed,&context,[&](const QString &reason){errors.append(QStringLiteral("callee: ")+reason);});
  QObject::connect(&a.client,&OscarClient::rendezvousReceived,&context,[&](const Rendezvous &rv){if(rv.cookie!=cookie||rv.capability!=aim::oscar::capVoice()||!same(rv.sender,b.name))return;toCaller.append(rv);if(rv.type!=2)return;const QByteArray port=value(rv,5);if(port.size()!=2){errors.append(QStringLiteral("accept missing listener port"));return;}caller.connectTo(addresses(rv),qFromBigEndian<quint16>(port.constData()));callerPort=caller.listen();if(!callerPort||!send(a,b,0,callerPort,2))errors.append(QStringLiteral("counter-proposal failed"));});
  QObject::connect(&b.client,&OscarClient::rendezvousReceived,&context,[&](const Rendezvous &rv){if(rv.cookie!=cookie||rv.capability!=aim::oscar::capVoice()||!same(rv.sender,a.name))return;toCallee.append(rv);if(rv.type!=0)return;if(sequence(rv)==1){calleePort=callee.listen();if(!calleePort||!send(b,a,2,calleePort,0))errors.append(QStringLiteral("accept failed"));}else if(sequence(rv)==2){const QByteArray port=value(rv,5);if(port.size()!=2){errors.append(QStringLiteral("counter-proposal missing listener port"));return;}callee.connectTo(addresses(rv),qFromBigEndian<quint16>(port.constData()));}});
  check("talk rendezvous proposal sent",send(a,b,0,0,1),a.lastOpFailure());
  const bool relayed=waitFor([&]{return !toCaller.isEmpty()&&toCallee.size()>=2||!errors.isEmpty();});
  check("talk rendezvous propose/accept/counter relayed",relayed&&errors.isEmpty()&&toCaller.size()==1&&toCallee.size()==2,errors.isEmpty()?QStringLiteral("caller=%1 callee=%2").arg(toCaller.size()).arg(toCallee.size()):errors.join(';'));
  if(!relayed||!errors.isEmpty()||toCaller.isEmpty()||toCallee.size()<2)return;
  const Rendezvous proposal=toCallee[0],accept=toCaller[0],counter=toCallee[1];
  check("talk rendezvous proposal contents",proposal.type==0&&sequence(proposal)==1&&value(proposal,5).isEmpty()&&value(proposal,0x2711)==QByteArray::fromHex("00000001")&&value(proposal,4).size()==4,QStringLiteral("proposal fields or verified IP mismatch"));
  check("talk rendezvous accept contents",accept.type==2&&value(accept,3)==QByteArray::fromHex("7f000001")&&value(accept,5).size()==2&&qFromBigEndian<quint16>(value(accept,5).constData())==calleePort,QStringLiteral("accept address or listener port mismatch"));
  check("talk rendezvous counter contents",counter.type==0&&sequence(counter)==2&&value(counter,5).size()==2&&qFromBigEndian<quint16>(value(counter,5).constData())==callerPort&&value(counter,4).size()==4,QStringLiteral("counter sequence, listener port or verified IP mismatch"));
  const bool connected=waitFor([&]{return callerConnected&&calleeConnected||!errors.isEmpty();},15000);check("talk rendezvous call connected",connected&&callerConnected&&calleeConnected,errors.isEmpty()?QStringLiteral("caller=%1 callee=%2").arg(callerConnected).arg(calleeConnected):errors.join(';'));if(!callerConnected||!calleeConnected)return;
  caller.audio().setCapturing(false);callee.audio().setCapturing(false);QByteArray pcm(TalkAudio::ChunkBytes,0);auto *samples=reinterpret_cast<qint16*>(pcm.data());for(int i=0;i<TalkAudio::ChunkSamples;++i)samples[i]=qint16(6000*std::sin(i*0.12)+3000*std::sin(i*0.37));
  const int beforeCaller=callerSamples,beforeCallee=calleeSamples;for(int i=0;i<4;++i){emit caller.audio().captured(pcm);emit callee.audio().captured(pcm);settle(50);}
  check("talk rendezvous audio both directions",waitFor([&]{return callerSamples-beforeCaller>=4*TalkAudio::ChunkSamples&&calleeSamples-beforeCallee>=4*TalkAudio::ChunkSamples;},5000),QStringLiteral("caller=%1 callee=%2 decoded samples").arg(callerSamples-beforeCaller).arg(calleeSamples-beforeCallee));
  caller.hangUp();check("talk rendezvous hangup",waitFor([&]{return calleeEnded;},3000),QStringLiteral("callee did not receive BYE"));
}

void scenarioTalkLoopback() {
  const quint64 cookie = 0xA1B2C3D4E5F60718ULL;
  TalkCall callee(cookie, TalkCall::Role::Callee, false), caller(cookie, TalkCall::Role::Caller, false);
  caller.audio().setSpeakerVolume(0);callee.audio().setSpeakerVolume(0);
  const quint16 port = callee.listen(); check("talk callee listener", port >= 1112 && port <= 3333, QStringLiteral("port %1").arg(port));
  bool c1 = false, c2 = false; int heardByCallee = 0, heardByCaller = 0; bool calleeEnded = false;
  QObject::connect(&caller, &TalkCall::connected, &caller, [&] { c1 = true; });
  QObject::connect(&callee, &TalkCall::connected, &callee, [&] { c2 = true; });
  QObject::connect(&callee, &TalkCall::audioReceived, &callee, [&](int n) { heardByCallee += n; });
  QObject::connect(&caller, &TalkCall::audioReceived, &caller, [&](int n) { heardByCaller += n; });
  QObject::connect(&callee, &TalkCall::ended, &callee, [&](bool) { calleeEnded = true; });
  caller.listen(); caller.connectTo(QHostAddress(QHostAddress::LocalHost), port);
  check("talk connected (handshake + media negotiation)", waitFor([&] { return c1 && c2; }, 15000), QStringLiteral("caller %1 callee %2").arg(c1).arg(c2));
  if (!c1 || !c2) return;
  // Drive the encoders directly with a synthetic vowel-like signal (no microphone needed).
  QByteArray pcm(TalkAudio::ChunkBytes, 0); auto *s = reinterpret_cast<qint16 *>(pcm.data());
  for (int i = 0; i < TalkAudio::ChunkSamples; ++i) s[i] = qint16(6000 * std::sin(i * 0.12) + 3000 * std::sin(i * 0.37));
  caller.startSending(); callee.startSending(); settle(300);
  caller.audio().setCapturing(false);callee.audio().setCapturing(false);
  for (int i = 0; i < 4; ++i) { emit caller.audio().captured(pcm); emit callee.audio().captured(pcm); settle(50); }
  check("talk audio caller->callee decoded", waitFor([&] { return heardByCallee >= 4 * 1440; }, 5000), QStringLiteral("%1 samples").arg(heardByCallee));
  check("talk audio callee->caller decoded", waitFor([&] { return heardByCaller >= 4 * 1440; }, 5000), QStringLiteral("%1 samples").arg(heardByCaller));
  caller.setHold(true); check("talk hold seen by peer", waitFor([&] { return callee.remotePaused(); }, 3000), "no HOLD");
  caller.setHold(false); check("talk resume seen by peer", waitFor([&] { return !callee.remotePaused(); }, 3000), "no RESUME");
  caller.hangUp(); check("talk hang up seen by peer", waitFor([&] { return calleeEnded; }, 3000), "no BYE");
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
  if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--talk")) { scenarioTalkLoopback(); out << "SUMMARY failures=" << failures << Qt::endl; return failures; }
  const QString path = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../../../Research/test-accounts.local.txt"));
  const QString user1 = QStringLiteral("openaimtest1"), user2 = QStringLiteral("openaimtest2");
  QString pass1, pass2;
  if (!loadAccounts(path, user1, user2, &pass1, &pass2)) {
    out << "FAIL setup: could not read both test accounts from the credentials file" << Qt::endl;
    return 1;
  }
  const QString host=qEnvironmentVariable(kHostVariable);if(host.trimmed().isEmpty()){out << "FAIL setup: set OPENAIM_TEST_HOST to your test server" << Qt::endl;return 1;}
  Recorder a(user1), b(user2);
  scenarioSignOn(a, b, host, pass1, pass2);
  pass1.fill(QLatin1Char(0));
  pass2.fill(QLatin1Char(0));
  if (!a.client.connected() || !b.client.connected()) {
    skip("remaining scenarios", "sign-on failed");
  } else {
    scenarioMessaging(a, b);
    scenarioUserInfo(a, b);
    scenarioWarn(a, b);
    scenarioChat(a, b);
    scenarioBuddies(a, b);
    scenarioDirectIm(a, b);
    scenarioTalkRendezvous(a,b);
    scenarioTalkLoopback();
  }
  scenarioSignOff(a, b);
  out << "SUMMARY failures=" << failures << Qt::endl;
  return failures;
}
