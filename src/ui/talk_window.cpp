#include "talk_window.h"
#include "art.h"
#include "ctl_window.h"
#include "native_dialog.h"
#include "preferences.h"
#include "sounds.h"
#include "user_actions.h"
#include "../talk/talk_call.h"
#include <QGuiApplication>
#include <QIcon>
#include <QMouseEvent>
#include <QNetworkInterface>
#include <QProcess>
#include <QRandomGenerator>
#include <QScreen>
#include <QSettings>
#include <QtEndian>

namespace {
QString aimString(quint32 id) { return aimEnvironment().string(id); }
QString named(quint32 id, const QString &name) { return QString(aimString(id)).replace(QStringLiteral("%s"), name); }
QString key(QString name) { name.remove(QLatin1Char(' ')); return name.toCaseFolded(); }
enum : quint32 { TalkLed = 29, PauseLed = 23, DisconnectLed = 27, PushToTalk = 34, PushToListen = 35, Mute = 32, Pause = 31, Disconnect = 33,
                 SendIm = 562, VolumeControls = 983, MeterStyle = 42, OwnName = 38, BuddyName = 39, OwnMeter = 36, BuddyMeter = 37,
                 MicSlider = 21, SpeakerSlider = 22, Countdown = 40, Status = 969 };
#ifdef Q_OS_WIN
HWND hwnd(QWindow *window) { return window && window->handle() ? reinterpret_cast<HWND>(window->winId()) : nullptr; }
void talkBox(QWindow *owner, const QString &text) { const QString title = aimString(1810); MessageBoxW(hwnd(owner), reinterpret_cast<LPCWSTR>(text.utf16()), reinterpret_cast<LPCWSTR>(title.utf16()), MB_OK | MB_ICONINFORMATION); } // "Talk Error"
#else
void talkBox(QWindow *, const QString &) {}
#endif
QHostAddress localAddress() {
  // GetLocalIP (icbmui 0x113918e0): first address of the host, the second with "Use alternate Internet Address".
  QList<QHostAddress> found; for (const QHostAddress &a : QNetworkInterface::allAddresses()) if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !a.isLinkLocal()) found.append(a);
  if (found.isEmpty()) return QHostAddress(QHostAddress::LocalHost);
  return prefs::checked(279, 903) && found.size() > 1 ? found[1] : found[0];
}
QList<QHostAddress> addressesOf(const aim::oscar::Rendezvous &rv) {
  QList<QHostAddress> out; for (quint16 tag : {quint16(3), quint16(4), quint16(2)}) for (const auto &tlv : rv.values) if (tlv.tag == tag && tlv.value.size() == 4) { const QHostAddress a(qFromBigEndian<quint32>(tlv.value.constData())); if (!a.isNull() && !out.contains(a)) out.append(a); }
  return out;
}
quint16 tlv16(const aim::oscar::Rendezvous &rv, quint16 tag, quint16 fallback = 0) { for (const auto &tlv : rv.values) if (tlv.tag == tag && tlv.value.size() == 2) return qFromBigEndian<quint16>(tlv.value.constData()); return fallback; }
}

// ---- Talk window (aimtalk.dll AIM_Talk, CTLGROUP 1) ----
class TalkWindow final : public CtlWindow {
public:
  TalkWindow(const QString &own, const QString &buddy, bool outgoing) : CtlWindow(named(outgoing ? 55 : 56, buddy), 1), own_(own), buddy_(buddy) {
    setIcon(QIcon(QStringLiteral(":/aim/icons/101"))); setCaptionButtons(true, false, true);
    QSize client(354, 148); if (group_) { const QSize ideal = ctlIdealSize(*group_, aimEnvironment()); if (ideal.width() > 0 && ideal.height() > 0) client = ideal; }
    setCanvasSize(canvasForClient(client));
    const QRect saved = QSettings().value(QStringLiteral("windows/TalkWnd")).toRect();
    if (saved.isValid()) setFramePosition(saved.topLeft()); else setFramePosition(QPoint(10, 10));
    style_ = QSettings().value(QStringLiteral("Talk/Style"), 0).toInt() % 3;
    mic_ = QSettings().value(QStringLiteral("Talk/MicLevel"), 100).toInt(); speaker_ = QSettings().value(QStringLiteral("Talk/SpeakerLevel"), 100).toInt();
    meterTimer_.setInterval(100); QObject::connect(&meterTimer_, &QTimer::timeout, this, [this] { requestUpdate(); }); meterTimer_.start(); // 100 ms meter timer
    countdownTimer_.setInterval(1000); QObject::connect(&countdownTimer_, &QTimer::timeout, this, [this] { if (--countdown_ <= 0) closeRequested(); else requestUpdate(); });
    updateControls();
  }
  std::function<void()> userClosed;            // Disconnect / window closed by the user
  std::function<void(const QString &)> openIm;
  void setCall(TalkCall *call) {
    call_ = call; if (!call) return;
    call->audio().setMicGain(mic_); call->audio().setSpeakerVolume(speaker_);
    QObject::connect(call, &TalkCall::stateChanged, this, [this] { updateControls(); });
    QObject::connect(call, &TalkCall::connected, this, [this] { setTitle(named(57, buddy_)); updateControls(); });
  }
  void dismiss() { userClosed = {}; hide(); deleteLater(); }
  void ended() {
    userClosed = {};
    // "%s disconnected. This window will close in %d seconds" (STRING 54), 4 s countdown.
    disconnected_ = true; call_.clear(); countdown_ = 4; countdownTimer_.start(); updateControls();
  }
protected:
  QString staticText(const CtlObject &o) const override {
    switch (o.id) {
    case OwnName: return own_;
    case BuddyName: return buddy_;
    case Countdown: return QString(aimString(54)).replace(QStringLiteral("%s"), buddy_).replace(QStringLiteral("%d"), QString::number(countdown_)).trimmed();
    case Status: return statusText();
    default: return CtlWindow::staticText(o);
    }
  }
  bool paintCustom(QPainter &p, const CtlObject &o) override {
    const QRect r = o.windowRect();
    if (o.id == OwnMeter || o.id == BuddyMeter) { paintMeter(p, r, o.id == OwnMeter ? level(true) : level(false)); return true; }
    if (o.id == TalkLed || o.id == PauseLed || o.id == DisconnectLed) {
      const bool on = o.id == TalkLed ? (call_ && call_->isConnected() && call_->sending()) : o.id == PauseLed ? (call_ && (call_->paused() || call_->remotePaused())) : disconnected_;
      const QColor colour = !on ? QColor(64, 64, 64) : o.id == TalkLed ? QColor(0, 220, 0) : o.id == PauseLed ? QColor(255, 210, 0) : QColor(230, 0, 0);
      p.save(); p.setRenderHint(QPainter::Antialiasing); p.setPen(QColor(32, 32, 32)); p.setBrush(colour); const int d = qMin(r.width(), r.height()) - 2; p.drawEllipse(QRect(r.left() + (r.width() - d) / 2, r.top() + (r.height() - d) / 2, d, d)); p.restore();
      return true;
    }
    if (o.kind == CtlObject::Kind::Trackbar) { paintSlider(p, r, o.id == MicSlider ? mic_ : speaker_); return true; }
    return false;
  }
  void contentMousePress(const QPoint &point, Qt::MouseButton button) override {
    for (quint32 id : {MicSlider, SpeakerSlider}) if (const CtlObject *o = object(id); o && o->shown() && o->windowRect().contains(point) && button == Qt::LeftButton) { dragging_ = id; slide(point); return; }
    CtlWindow::contentMousePress(point, button);
  }
  void contentMouseMove(const QPoint &point) override { if (dragging_) { slide(point); return; } CtlWindow::contentMouseMove(point); }
  void contentMouseRelease(const QPoint &point, Qt::MouseButton button) override { if (dragging_) { dragging_ = 0; return; } CtlWindow::contentMouseRelease(point, button); }
  void command(int id) override {
    switch (id) {
    case PushToTalk: if (call_) call_->startSending(); return;
    case PushToListen: if (call_) call_->stopSending(); return;
    case Mute: if (call_) { if (call_->sending()) call_->stopSending(); else call_->startSending(); } return;
    case Pause: if (call_) call_->setHold(!call_->paused()); return;
    case Disconnect: case 2: closeRequested(); return;
    case SendIm: if (openIm) openIm(buddy_); return;
    case VolumeControls: QProcess::startDetached(QStringLiteral("sndvol.exe"), {}); return; // the original ran SNDVOL32.EXE
    case MeterStyle: style_ = (style_ + 1) % 3; QSettings().setValue(QStringLiteral("Talk/Style"), style_); requestUpdate(); return;
    default: return;
    }
  }
  bool controlEnabled(quint32 id) const override {
    if (id == PushToTalk || id == PushToListen || id == Mute || id == Pause) return call_ && call_->isConnected() && !disconnected_;
    return true;
  }
  void closeRequested() override {
    if (isVisible()) QSettings().setValue(QStringLiteral("windows/TalkWnd"), QRect(framePosition(), frameGeometry().size()));
    if (!disconnected_ && userClosed) { auto handler = userClosed; userClosed = {}; handler(); }
    hide(); deleteLater();
  }
private:
  void updateControls() {
    if (!group_) return;
    const bool full = call_ && call_->fullDuplex(), half = call_ && !full;
    ctlShowControl(*group_, PushToTalk, !full && !(half && call_->sending()));
    ctlShowControl(*group_, PushToListen, half && call_->sending());
    ctlShowControl(*group_, Mute, full);
    ctlShowControl(*group_, Countdown, disconnected_);
    relayout(); requestUpdate();
  }
  QString statusText() const {
    // aimtalk 0x12102c38: Connecting 1176; full duplex 1177 paused / 1178 muted; half duplex 1179-1183.
    if (disconnected_ || !call_ || !call_->isConnected()) return disconnected_ ? QString() : aimString(1176);
    if (call_->fullDuplex()) { if (call_->paused() || call_->remotePaused()) return aimString(1177); if (!call_->sending()) return aimString(1178); return {}; }
    if (call_->paused()) return aimString(1179);
    if (call_->remotePaused()) return aimString(1180);
    if (call_->sending()) return aimString(1181);
    return aimString(call_->remoteSending() ? 1182 : 1183);
  }
  int level(bool own) const { if (!call_ || !call_->isConnected()) return 0; return own ? call_->audio().micLevel() : call_->audio().speakerLevel(); }
  void paintMeter(QPainter &p, const QRect &r, int value) {
    // Three meter styles cycled by the "change meter style" button ([Talk] Style 0..2).
    p.fillRect(r, Qt::black); art::drawSunken(p, r);
    const QRect inner = r.adjusted(3, 3, -3, -3); const int lit = inner.width() * qBound(0, value, 100) / 100;
    if (style_ == 0) { p.fillRect(QRect(inner.left(), inner.top(), lit, inner.height()), QColor(0, 200, 0)); }
    else if (style_ == 1) { const int segments = 15, w = inner.width() / segments; for (int i = 0; i < segments; ++i) { const bool on = i * 100 / segments < value; const QColor c = i < 9 ? QColor(0, 200, 0) : i < 13 ? QColor(230, 210, 0) : QColor(230, 0, 0); p.fillRect(QRect(inner.left() + i * w + 1, inner.top() + 2, w - 2, inner.height() - 4), on ? c : c.darker(400)); } }
    else { const int bars = 20, w = inner.width() / bars; for (int i = 0; i < bars; ++i) { const int h = inner.height() * (i + 1) / bars; const bool on = i * 100 / bars < value; p.fillRect(QRect(inner.left() + i * w + 1, inner.bottom() - h + 1, w - 1, h), on ? QColor(0, 200, 0) : QColor(0, 60, 0)); } }
  }
  void paintSlider(QPainter &p, const QRect &r, int value) {
    p.fillRect(r, art::Face);
    const int y = r.center().y(); const QRect groove(r.left() + 5, y - 2, r.width() - 10, 4); art::drawSunken(p, groove);
    const int x = groove.left() + (groove.width() - 1) * value / 100; const QRect thumb(x - 4, r.top() + 2, 9, r.height() - 4);
    p.fillRect(thumb, art::Face); p.setPen(Qt::white); p.drawLine(thumb.topLeft(), thumb.topRight()); p.drawLine(thumb.topLeft(), thumb.bottomLeft());
    p.setPen(Qt::black); p.drawLine(thumb.topRight(), thumb.bottomRight()); p.drawLine(thumb.bottomLeft(), thumb.bottomRight());
  }
  void slide(const QPoint &point) {
    const CtlObject *o = object(dragging_); if (!o) return; const QRect r = o->windowRect().adjusted(5, 0, -5, 0);
    const int value = qBound(0, (point.x() - r.left()) * 100 / qMax(1, r.width() - 1), 100);
    if (dragging_ == MicSlider) { mic_ = value; QSettings().setValue(QStringLiteral("Talk/MicLevel"), value); if (call_) call_->audio().setMicGain(value); }
    else { speaker_ = value; QSettings().setValue(QStringLiteral("Talk/SpeakerLevel"), value); if (call_) call_->audio().setSpeakerVolume(value); }
    requestUpdate();
  }
  QString own_, buddy_;
  QPointer<TalkCall> call_;
  QTimer meterTimer_, countdownTimer_;
  int style_ = 0, countdown_ = 0, mic_ = 100, speaker_ = 100;
  quint32 dragging_ = 0;
  bool disconnected_ = false;
};

// ---- sessions and rendezvous ----
struct TalkSessions::Session { QString name; quint64 cookie = 0; bool caller = false, counterSent = false; QPointer<TalkCall> call; QPointer<TalkWindow> window; QTimer timer; };

TalkSessions::TalkSessions(OscarClient *client, OpenIm openIm, QObject *parent) : QObject(parent), client_(client), openIm_(std::move(openIm)) {
  connect(client_, &OscarClient::rendezvousReceived, this, [this](const aim::oscar::Rendezvous &rv) { if (rv.capability == aim::oscar::capVoice()) incoming(rv); });
  connect(client_, &OscarClient::loginStageChanged, this, [this](int stage) { if (stage == 0) for (Session *s : QList<Session *>(sessions_)) { if (s->call && s->call->isConnected()) continue; finish(s); } }); // calls are peer to peer and survive
}
TalkSessions::~TalkSessions() { for (Session *s : QList<Session *>(sessions_)) { if (s->call) s->call->hangUp(); finish(s); } }
TalkSessions::Session *TalkSessions::find(const QString &name) const { for (Session *s : sessions_) if (key(s->name) == key(name)) return s; return nullptr; }

void TalkSessions::send(Session *session, quint16 type, quint16 port, quint16 sequence) {
  aim::oscar::Rendezvous rv; rv.type = type; rv.cookie = session->cookie; rv.capability = aim::oscar::capVoice();
  QByteArray ip(4, 0); qToBigEndian(localAddress().toIPv4Address(), ip.data()); rv.values.append({0x03, ip}); // own LAN address
  if (port) { QByteArray value(2, 0); qToBigEndian(port, value.data()); rv.values.append({0x05, value}); }
  if (type == 0) { QByteArray value(2, 0); qToBigEndian(sequence, value.data()); rv.values.append({0x0a, value}); rv.values.append({0x2711, QByteArray::fromHex("00000001")}); } // service data 27 11 00 04 00 00 00 01
  client_->sendRendezvous(session->name, rv);
}
void TalkSessions::cancel(Session *session, quint16 reason) {
  aim::oscar::Rendezvous rv; rv.type = 1; rv.cookie = session->cookie; rv.capability = aim::oscar::capVoice(); QByteArray value(2, 0); qToBigEndian(reason, value.data()); rv.values = {{0x0b, value}};
  if (client_->connected()) client_->sendRendezvous(session->name, rv);
}
void TalkSessions::finish(Session *session) {
  sessions_.removeOne(session); session->timer.stop();
  if (session->call) { session->call->disconnect(this); session->call->deleteLater(); }
  if (session->window && !session->window->isVisible()) session->window->deleteLater();
  delete session;
}
void TalkSessions::attachCall(Session *session) {
  session->call = new TalkCall(session->cookie, session->caller ? TalkCall::Role::Caller : TalkCall::Role::Callee, prefs::checked(279, 41), this); // "Always Talk in Half Duplex mode"
  TalkCall *call = session->call; const QString name = session->name;
  session->window = new TalkWindow(client_->screenName(), name, session->caller);
  session->window->openIm = openIm_;
  session->window->setCall(call);
  session->window->userClosed = [this, name] { if (Session *s = find(name)) { if (s->call && s->call->isConnected()) s->call->hangUp(); else cancel(s, 1); finish(s); } };
  connect(call, &TalkCall::connected, this, [this, name] { if (Session *s = find(name)) s->timer.stop(); });
  connect(call, &TalkCall::remoteStoppedTalking, this, [] { playAimSound(AimSound::TalkStop); });
  connect(call, &TalkCall::ended, this, [this, name](bool) { playAimSound(AimSound::TalkEnd); if (Session *s = find(name)) { if (s->window) s->window->ended(); s->window.clear(); finish(s); } });
  connect(call, &TalkCall::failed, this, [this, name](const QString &) { if (Session *s = find(name)) { talkBox(s->window, named(1025, name)); if (s->window) s->window->dismiss(); finish(s); } });
  session->window->show();
}

void TalkSessions::preview() { auto *window = new TalkWindow(QStringLiteral("denveous"), QStringLiteral("edward"), true); window->show(); }
void TalkSessions::start(const QString &screenName) {
  const QString name = screenName.trimmed(); if (name.isEmpty() || !client_->connected()) return;
  if (find(name)) { talkBox(nullptr, aimString(1324)); return; }      // "You already have a talk session with this buddy."
  if (!sessions_.isEmpty()) { talkBox(nullptr, aimString(32)); return; } // only one Talk session at a time
#ifdef Q_OS_WIN
  // RT_DIALOG 244 "Start Talk Connection" (title STRING 1104) unless Preferences > Talk "Display start dialog" is off.
  if (prefs::checked(279, 904, true)) {
    bool go = false, dontShow = false;
    runOriginalDialog(nullptr, 244, [&](HWND dialog) { const QString title = named(1104, name); SetWindowTextW(dialog, reinterpret_cast<LPCWSTR>(title.utf16())); },
      [&](HWND dialog, int id, int) { if (id == 831 || id == 832 || id == IDCANCEL) { go = id == 831; dontShow = IsDlgButtonChecked(dialog, 825) == BST_CHECKED; EndDialog(dialog, id); return true; } return false; });
    if (dontShow) { QSettings().setValue(QStringLiteral("nativePreferences/279/904"), 0); prefs::invalidate(); }
    if (!go) return;
  }
#endif
  auto *session = new Session; session->name = name; session->cookie = QRandomGenerator::global()->generate64(); session->caller = true; sessions_.append(session);
  attachCall(session);
  if (!session->call->audioAvailable()) { talkBox(session->window, aimString(35)); if (session->window) session->window->dismiss(); finish(session); return; }
  playAimSound(AimSound::TalkBegin);
  send(session, 0, 0, 1); // PROPOSE: own IP, no port yet, sequence 1
  session->timer.setSingleShot(true);
  connect(&session->timer, &QTimer::timeout, this, [this, name] { if (Session *s = find(name)) { talkBox(s->window, named(1022, name)); cancel(s, 1); if (s->window) s->window->dismiss(); finish(s); } });
  session->timer.start(60000); // outgoing proposal: 60 s
}

void TalkSessions::incoming(const aim::oscar::Rendezvous &rv) {
  const QString name = rv.sender; Session *session = find(name);
  if (rv.type == 1) { // cancel
    if (!session || session->cookie != rv.cookie) return;
    const quint16 reason = tlv16(rv, 0x0b, 0xffff);
    const QString text = reason == 0 ? named(1320, name) : reason == 1 ? named(1017, name) : reason == 2 ? named(1016, name) : reason == 5 ? named(1019, name) : reason == 6 ? named(33, name) : reason == 7 ? aimString(73) : session->caller ? named(1021, name) : aimString(74);
    if (!(session->call && session->call->isConnected())) { talkBox(session->window, text); if (session->window) session->window->dismiss(); finish(session); }
    return;
  }
  if (rv.type == 2) { // ACCEPT: connect to the callee's listener, open ours and send the counter-proposal (sequence 2)
    if (!session || session->cookie != rv.cookie || !session->caller || !session->call) return;
    session->call->connectTo(addressesOf(rv), tlv16(rv, 0x05));
    const quint16 port = session->call->listen();
    if (!session->counterSent && port) { session->counterSent = true; send(session, 0, port, 2); }
    session->timer.start(30000); return;
  }
  if (rv.type != 0) return;
  const quint16 sequence = tlv16(rv, 0x0a, 1);
  if (session && session->cookie == rv.cookie) { // counter-proposal: connect to the caller's listener
    if (!session->caller && session->call && sequence > 1) session->call->connectTo(addressesOf(rv), tlv16(rv, 0x05));
    return;
  }
  auto reject = [&](quint16 reason) { Session temp; temp.name = name; temp.cookie = rv.cookie; cancel(&temp, reason); };
  if (!sessions_.isEmpty()) { reject(6); return; } // busy: one Talk session at a time
  // Accept policy, Preferences > Talk: buddies 767 allow / 770 approve / 895 deny, others 768 / 769 / 765.
  bool buddy = false; for (const auto &item : client_->roster()) buddy = buddy || (item.classId == 0 && key(item.name) == key(name));
  const bool allow = buddy ? prefs::checked(279, 767) : prefs::checked(279, 768), deny = buddy ? prefs::checked(279, 895) : prefs::checked(279, 765);
  if (deny) { reject(2); return; }
  playAimSound(AimSound::TalkBegin);
  if (!allow) {
#ifdef Q_OS_WIN
    // RT_DIALOG 245 "Receive Talk Connection" (title STRING 1105): Accept 792, Reject 2, Ignore 799, Warn 793.
    int choice = 0;
    runOriginalDialog(nullptr, 245, [&](HWND dialog) { const QString title = named(1105, name); SetWindowTextW(dialog, reinterpret_cast<LPCWSTR>(title.utf16())); },
      [&](HWND dialog, int id, int) { if (id == 792 || id == IDCANCEL || id == 799 || id == 793) { choice = id; EndDialog(dialog, id); return true; } return false; });
    if (choice == 793) { userActions::warn(nullptr, client_, name); reject(1); return; }
    if (choice == 799) { reject(2); return; }
    if (choice != 792) { reject(1); return; }
#endif
    if (find(name) || !sessions_.isEmpty()) return;
  }
  auto *accepted = new Session; accepted->name = name; accepted->cookie = rv.cookie; sessions_.append(accepted);
  attachCall(accepted);
  if (!accepted->call->audioAvailable()) { talkBox(accepted->window, aimString(35)); reject(6); if (accepted->window) accepted->window->dismiss(); finish(accepted); return; }
  const quint16 port = accepted->call->listen();
  if (!port) { reject(6); if (accepted->window) accepted->window->dismiss(); finish(accepted); return; }
  send(accepted, 2, port, 0); // ACCEPT: own IP + listener port
  accepted->timer.setSingleShot(true);
  connect(&accepted->timer, &QTimer::timeout, this, [this, name] { if (Session *s = find(name)) { if (s->call && s->call->isConnected()) return; talkBox(s->window, named(1025, name)); if (s->window) s->window->dismiss(); finish(s); } });
  accepted->timer.start(30000);
}
