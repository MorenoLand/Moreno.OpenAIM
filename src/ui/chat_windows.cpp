#include "chat_windows.h"
#include "ate_link.h"
#include "preferences.h"
#include "art.h"
#include "buddy_info_window.h"
#include "ctl_window.h"
#include "native_dialog.h"
#include "sounds.h"
#include "user_actions.h"
#include <QDateTime>
#include <QGuiApplication>
#include <QHash>
#include <QLocale>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QTextBlock>
#include <QTextCursor>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
QString aimString(quint32 id) { return aimEnvironment().string(id); }
QString normalized(QString name) { name.remove(QLatin1Char(' ')); return name.toCaseFolded(); }
void errorBox(QWindow *owner, const QString &text) {
#ifdef Q_OS_WIN
  MessageBoxW(owner && owner->handle() ? reinterpret_cast<HWND>(owner->winId()) : nullptr, reinterpret_cast<LPCWSTR>(text.utf16()), L"AOL Instant Messenger (SM)", MB_OK | MB_ICONEXCLAMATION | MB_TASKMODAL);
#else
  Q_UNUSED(owner); Q_UNUSED(text);
#endif
}
QSize fixedClient(CtlObject *group) {
  // ChatUI 0x11b02f6d: outer = ideal + 2*SM_CXBORDER x ideal + SM_CYCAPTION + 2*SM_CYBORDER + 10, i.e. client = ideal + (0, 10).
  return group ? ctlIdealSize(*group, aimEnvironment()) + QSize(0, 10) : QSize(356, 312);
}
void centre(QWindow *window) { if (QScreen *screen = QGuiApplication::primaryScreen()) window->setFramePosition(screen->availableGeometry().center() - QPoint(window->frameGeometry().width() / 2, window->frameGeometry().height() / 2)); }
// Per-user name colours (ChatUI table 0x11b101c0); own name red.
const QRgb UserColours[] = {0x0000FF, 0x7FFFD4, 0xFA8072, 0xF5F5DC, 0xFF7F50, 0x00FFFF, 0xFFFF00, 0xA9A9A9, 0x006400, 0xA52A2A, 0xFF8C00, 0x808080};
int nextColour = 0;
}

// ---------------------------------------------------------------- Chat Room (CTLGROUP 104) ----------
class ChatRoomWindow final : public CtlWindow {
public:
  enum State { Init, Waiting, Creating, CreationFailed, Joining, Rejoining, InRoom, Exited, Error };
  enum : quint32 { History = 479, Compose = 480, Send = 481, People = 482, Count = 488, InvitedLabel = 640, Invited = 636, Im = 573, Ignore = 646, Talk = 17, Info = 691, More = 751, Less = 752, Lower = 753 };
  ChatRoomWindow(ChatWindows *manager, const QString &name, int cascade) : CtlWindow(QString(), 104, 102), manager_(manager), name_(name) {
    setIcon(QIcon(QStringLiteral(":/aim/icons/119")));
    editableAtes_.insert(Compose); setReadOnlyFollowBottom(History);
    // Default 520x412 outer at (80,40) + cascade, min width 440 (0x11b092d6, 0x11b06ec8); saved under "ChatWnd".
    const QRect saved = QSettings().value(QStringLiteral("windows/ChatWnd")).toRect();
    const QSize outer = saved.isValid() ? saved.size() : QSize(520, 412);
    setResizable(canvasForClient(QSize(440 - 16, 300)));
    setCanvasSize(canvasForClient(outer - QSize(16, 39)));
    setFramePosition((saved.isValid() ? saved.topLeft() : QPoint(80, 40)) + QPoint(16, 20) * cascade);
    // Lower pane shown unless Chat\SimpleMode == 1 (missing key = expanded).
    expanded_ = QSettings().value(QStringLiteral("Chat/SimpleMode"), 2).toInt() != 1; applyExpanded();
    setState(Creating); setFocusControl(Compose);
  }
  QString name() const { return name_; }
  QString cookie() const { return room_.cookie; }
  State state() const { return state_; }
  void setState(State state) { state_ = state; updateTitle(); requestUpdate(); }
  void joined(const aim::oscar::ChatRoom &room) {
    room_ = room; if (!room.name.isEmpty()) name_ = room.name;
    if (!entered_) { notice(QString(aimString(706)).replace(QStringLiteral("%s"), name_.toHtmlEscaped())); entered_ = true; }
    setState(InRoom);
    for (const QString &name : pendingInvites_) inviteNow(name); pendingInvites_.clear();
  }
  void queueInvites(const QStringList &names, const QString &message) { message_ = message; for (const QString &n : names) { if (state_ == InRoom) inviteNow(n); else pendingInvites_.append(n); } }
  void participants(const QVector<aim::oscar::UserInfo> &users) {
    QStringList now; for (const auto &u : users) now.append(u.screenName);
    const QString self = manager_->client()->screenName();
    for (const QString &n : now) if (!containsName(people_, n)) {
      people_.append(n); colours_[normalized(n)] = normalized(n) == normalized(self) ? 0xFF0000 : UserColours[nextColour++ % 12];
      invited_.removeIf([&](const QString &i) { return normalized(i) == normalized(n); });
      if (entered_ && normalized(n) != normalized(self) && joinNotices()) notice(QString(aimString(778)).replace(QStringLiteral("%s"), n.toHtmlEscaped()));
    }
    for (const QString &n : QStringList(people_)) if (!containsName(now, n)) { people_.removeIf([&](const QString &p) { return normalized(p) == normalized(n); }); if (joinNotices()) notice(QString(aimString(779)).replace(QStringLiteral("%s"), n.toHtmlEscaped())); }
    updateInvitedSection(); requestUpdate();
  }
  void message(const QString &sender, const QString &text) {
    if (ignored_.contains(normalized(sender))) return;
    const QString self = manager_->client()->screenName(); const bool own = normalized(sender) == normalized(self);
    const QRgb colour = own ? 0xFF0000 : colours_.value(normalized(sender), 0x0000FF);
    QTextCursor cursor(&document(History)); cursor.movePosition(QTextCursor::End); if (!first_) cursor.insertBlock(); first_ = false;
    cursor.insertHtml(QStringLiteral("<font color=#%1><b>%2</b>:</font>&nbsp;").arg(colour, 6, 16, QLatin1Char('0')).arg(sender.toHtmlEscaped()));
    const int start = cursor.position(); ate::insertMessageHtml(cursor, text, QColor(Qt::white)); ate::insertSmileys(document(History), start); requestUpdate();
    if (!own) playAimSound(AimSound::ChatReceive);
#ifdef Q_OS_WIN
    if (!own && prefs::chatFlash() && !isActive() && handle()) { FLASHWINFO flash{sizeof(flash), reinterpret_cast<HWND>(winId()), FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0}; FlashWindowEx(&flash); } // "Flash window when messages are received"
#endif
  }
  void closedByServer() { setState(Exited); }
protected:
  QString staticText(const CtlObject &o) const override {
    if (o.id == Count) { int n = int(people_.size()); return n == 0 ? aimString(1845) : n == 1 ? aimString(647) : QString(aimString(646)).replace(QStringLiteral("%d"), QString::number(n)); }
    return CtlWindow::staticText(o);
  }
  QStringList listRows(quint32 id) const override { if (id == People) { QStringList rows; for (const QString &p : people_) rows << (ignored_.contains(normalized(p)) ? p + QStringLiteral(" (ignored)") : p); return rows; } if (id == Invited) return invited_; return {}; }
  QColor listRowColor(quint32 id, int row) const override { if (id == People && row < people_.size()) return QColor::fromRgb(colours_.value(normalized(people_[row]), 0)); return Qt::black; }
  void listActivated(quint32 id, int row) override { if ((id == People && row < people_.size()) || (id == Invited && row < invited_.size())) manager_->action()(139, id == People ? people_[row] : invited_[row]); }
  bool controlEnabled(quint32 id) const override {
    if (id == Send) return state_ == InRoom && !const_cast<ChatRoomWindow *>(this)->editor(Compose).text().trimmed().isEmpty();
    if (id == Im || id == Ignore || id == Talk || id == Info) return state_ != Exited && state_ != Error;
    return true;
  }
  bool submitEditor(quint32 id) override { if (id == Compose) { send(); return true; } return false; }
  bool isComposePane(quint32 id) const override { return id == Compose; }
  qreal documentZoom(quint32 id) const override { return id == History ? prefs::textMagnification() : 1.0; }
  void command(int id) override {
    const QString selected = selectedRow(People) >= 0 && selectedRow(People) < people_.size() ? people_[selectedRow(People)] : QString();
    switch (id) {
    case Send: send(); return;
    case 2: closeRequested(); return;
    case Im: case 490: if (!selected.isEmpty()) manager_->action()(139, selected); return;
    case Talk: if (!selected.isEmpty() && normalized(selected) != normalized(manager_->client()->screenName())) manager_->action()(18, selected); return;
    case Info: case 491: if (!selected.isEmpty()) manager_->action()(138, selected); return;
    case 674: if (!selected.isEmpty()) userActions::addBuddy(this, manager_->client(), selected); return;
    case Ignore: case 564: if (!selected.isEmpty() && normalized(selected) != normalized(manager_->client()->screenName())) { if (ignored_.contains(normalized(selected))) ignored_.remove(normalized(selected)); else ignored_.insert(normalized(selected)); requestUpdate(); } return;
    case More: expanded_ = true; applyExpanded(); return;
    case Less: expanded_ = false; applyExpanded(); return;
    case 574: case 563: manager_->inviteToRoom(this, {}); return;     // People > Invite a Buddy...
    case 396: editor(Compose).copy(true); requestUpdate(); return;
    case 397: editor(Compose).copy(false); return;
    case 398: editor(Compose).paste(false); requestUpdate(); return;
    case 575: editor(Compose).cursor.select(QTextCursor::Document); requestUpdate(); return;
    default: return; // Talk, Save, Print, Log Manager, Chat Room Info, Help are not implemented
    }
  }
  void closeRequested() override {
    QSettings settings; settings.setValue(QStringLiteral("Chat/SimpleMode"), expanded_ ? 0 : 1);
    if (isVisible() && visibility() == QWindow::Windowed) settings.setValue(QStringLiteral("windows/ChatWnd"), QRect(framePosition(), frameGeometry().size()));
    if (!room_.cookie.isEmpty()) manager_->client()->leaveChatRoom(room_.cookie); // leaving closes the room's Chat connection
    manager_->roomClosed(this); hide(); deleteLater();
  }
private:
  static bool containsName(const QStringList &list, const QString &name) { for (const QString &n : list) if (normalized(n) == normalized(name)) return true; return false; }
  static bool joinNotices() { return prefs::chatAnnouncements(); } // Preferences > IM/Chat "Show announcements..."
  void notice(const QString &html) { QTextCursor cursor(&document(History)); cursor.movePosition(QTextCursor::End); if (!first_) cursor.insertBlock(); first_ = false; ate::insertMessageHtml(cursor, QStringLiteral("<font color=#000000>%1</font>").arg(html), QColor(Qt::white)); requestUpdate(); }
  void updateTitle() {
    // STRING 719 "Chat Room: %s" + state suffix 764-772 (none while in the room).
    static const quint32 suffix[] = {764, 765, 766, 767, 768, 769, 0, 771, 772};
    QString title = QString(aimString(719)).replace(QStringLiteral("%s"), name_); if (title.size() > 0x80) title = title.left(129) + QStringLiteral("...");
    if (suffix[state_]) title += aimString(suffix[state_]);
    setTitle(title);
  }
  void applyExpanded() { if (!group_) return; ctlShowControl(*group_, Lower, expanded_); ctlShowControl(*group_, Less, expanded_); ctlShowControl(*group_, More, !expanded_); updateInvitedSection(); requestUpdate(); }
  void updateInvitedSection() { if (!group_) return; ctlShowControl(*group_, Invited, !invited_.isEmpty()); ctlShowControl(*group_, InvitedLabel, !invited_.isEmpty()); }
  void inviteNow(const QString &name) {
    if (containsName(people_, name)) return; // already in the room (STRING 777 dialog in the original)
    if (manager_->client()->inviteToChat(name, room_.cookie, message_)) { if (!containsName(invited_, name)) invited_.append(name); updateInvitedSection(); requestUpdate(); }
  }
  void send() {
    if (!controlEnabled(Send)) return;
    TextEditor &compose = editor(Compose); QString text = compose.document.toPlainText();
    if (text.size() > 8000) { errorBox(this, formatAimString(aimString(1886), {QString::number(text.size()), QStringLiteral("8000")})); return; }
    if (!manager_->client()->sendChatMessage(room_.cookie, ate::html(compose.document, prefs::composeWindowColor()))){ errorBox(this, aimString(1594)); return; }
    compose.clear(); compose.cursor.setCharFormat(prefs::composeFormat()); playAimSound(AimSound::ChatSend); requestUpdate(); // shown when the server reflects it
  }
  ChatWindows *manager_;
  QString name_, message_;
  aim::oscar::ChatRoom room_;
  State state_ = Init;
  bool expanded_ = true, entered_ = false, first_ = true;
  QStringList people_, invited_, pendingInvites_;
  QHash<QString, QRgb> colours_;
  QSet<QString> ignored_;
};

// ---------------------------------------------------------------- Chat Invitation (CTLGROUP 106) -----
class ChatInviteWindow final : public CtlWindow {
public:
  enum : quint32 { Names = 504, Message = 505, RoomName = 506, Help = 9, Cancel = 2, SendButton = 508 };
  ChatInviteWindow(ChatWindows *manager, const QStringList &names, ChatRoomWindow *room) : CtlWindow(aimString(684), 106), manager_(manager), room_(room) {
    setIcon(QIcon(QStringLiteral(":/aim/icons/119"))); setCaptionButtons(false, false, true);
    editableAtes_.insert(Message);
    editor(Names).setText(names.join(QStringLiteral("\n")));
    editor(Message).setText(aimString(701)); // "Join me in this Chat."
    editor(RoomName).setText(room ? room->name() : defaultRoomName());
    setCanvasSize(canvasForClient(fixedClient(group_.get()))); centre(this);
    setFocusControl(names.isEmpty() ? Names : 0);
  }
protected:
  bool controlEnabled(quint32 id) const override { if (id == RoomName) return room_.isNull(); if (id == SendButton) return manager_->client()->connected(); return true; }
  void editorChanged(quint32 id) override { if (id == Message && editor(Message).text().size() > 129) { editor(Message).setText(editor(Message).text().left(129)); } }
  bool submitEditor(quint32 id) override { if (id != Names) { send(); return true; } return false; }
  void command(int id) override { if (id == Cancel) closeRequested(); else if (id == SendButton) send(); }
  void closeRequested() override { hide(); deleteLater(); }
private:
  QString defaultRoomName() const {
    // STRING 702 "%s Chat" + "%0.2d" of a tick-derived number, repeated until no open room has the name (0x11b03b0e).
    const QString base = QString(aimString(702)).replace(QStringLiteral("%s"), manager_->client()->screenName());
    for (int attempt = 0;; ++attempt) { const QString name = base + QStringLiteral("%1").arg(int(((QDateTime::currentMSecsSinceEpoch() + attempt) & 0xFF) % 100), 2, 10, QLatin1Char('0')); if (!manager_->findRoom(name) || attempt > 200) return name; }
  }
  void send() {
    // Validation order of 0x11b036c8.
    if (!manager_->client()->connected()) { errorBox(this, aimString(1621)); return; }
    const QString own = normalized(manager_->client()->screenName()); QStringList names;
    for (QString token : editor(Names).text().split(QRegularExpression(QStringLiteral("[,\\r\\n\\t]+")), Qt::SkipEmptyParts)) {
      token = token.trimmed(); if (token.isEmpty() || normalized(token) == own) continue;
      if (!QRegularExpression(QStringLiteral("^[A-Za-z][A-Za-z0-9 @._-]{1,31}$")).match(token).hasMatch()) { errorBox(this, QString(aimString(685)).replace(QStringLiteral("%s"), token.size() > 100 ? token.left(0x60) + QStringLiteral("...") : token)); setFocusControl(Names); return; }
      bool duplicate = false; for (const QString &n : names) duplicate = duplicate || normalized(n) == normalized(token); if (!duplicate) names.append(token);
    }
    if (names.isEmpty()) { userActions::infoBox(this, aimString(714)); setFocusControl(Names); return; }
    const QString message = editor(Message).text(); const QString room = editor(RoomName).text().trimmed();
    if (room.isEmpty()) { errorBox(this, aimString(720)); setFocusControl(RoomName); return; }
    if (room_.isNull() && room.size() > 80) { errorBox(this, QString(aimString(728)).replace(QStringLiteral("%d"), QStringLiteral("80"))); setFocusControl(RoomName); return; }
    if (room_) room_->queueInvites(names, message);
    else if (ChatRoomWindow *open = manager_->findRoom(room)) open->queueInvites(names, message);
    else if (!manager_->startRoom(room, names, message)) { errorBox(this, aimString(755)); return; }
    closeRequested();
  }
  ChatWindows *manager_;
  QPointer<ChatRoomWindow> room_;
};

// ---------------------------------------------------------------- Decline (CTLGROUP 107) ---------------
class ChatDeclineWindow final : public CtlWindow {
public:
  ChatDeclineWindow(ChatWindows *manager, const QString &inviter, std::function<void()> declined) : CtlWindow(aimString(798), 107), manager_(manager), inviter_(inviter), declined_(std::move(declined)) {
    setIcon(QIcon(QStringLiteral(":/aim/icons/119"))); setCaptionButtons(false, false, true);
    setCanvasSize(canvasForClient(fixedClient(group_.get()))); centre(this);
  }
protected:
  QString staticText(const CtlObject &o) const override { if (o.id == 689) return QString(aimString(791)).replace(QStringLiteral("%s"), inviter_); return CtlWindow::staticText(o); }
  void command(int id) override {
    if (id == 687) userActions::warn(this, manager_->client(), inviter_);
    else if (id == 688) userActions::block(this, manager_->client(), inviter_);
    else if (id == 1) { if (declined_) declined_(); closeRequested(); }
    else if (id == 2) closeRequested();
  }
  void closeRequested() override { hide(); deleteLater(); }
private:
  ChatWindows *manager_;
  QString inviter_;
  std::function<void()> declined_;
};

// ---------------------------------------------------------------- Invitation received (CTLGROUP 105) --
class ChatInviteReceiveWindow final : public CtlWindow {
public:
  ChatInviteReceiveWindow(ChatWindows *manager, const aim::oscar::ChatInvitation &invitation) : CtlWindow(QString(aimString(687)).replace(QStringLiteral("%s"), invitation.sender), 105), manager_(manager), invitation_(invitation) {
    setIcon(QIcon(QStringLiteral(":/aim/icons/119"))); setCaptionButtons(false, false, true);
    document(571).setHtml(invitation.message.toHtmlEscaped());
    // STRING 784: "At <time date> <inviter> (warning level n%) is inviting you to join the chat room, "<room>"."
    const QDateTime now = QDateTime::currentDateTime();
    info_ = formatAimString(aimString(784), {QLocale::system().toString(now.time(), QLocale::LongFormat) + QLatin1Char(' ') + QLocale::system().toString(now.date(), QLocale::ShortFormat), invitation.sender, QStringLiteral("0"), invitation.room.name});
    setCanvasSize(canvasForClient(fixedClient(group_.get()))); centre(this);
  }
protected:
  QString staticText(const CtlObject &o) const override { if (o.id == 567 && o.kind == CtlObject::Kind::Static) return info_; if (o.id == 569) return QString(aimString(785)).replace(QStringLiteral("%s"), invitation_.sender); return CtlWindow::staticText(o); }
  void command(int id) override {
    if (id == 509) { handled_ = true; manager_->accept(invitation_); closeRequested(); }          // Go Chat
    else if (id == 690) { auto *dialog = new ChatDeclineWindow(manager_, invitation_.sender, [this] { decline(); closeRequested(); }); dialog->show(); }
    else if (id == 567) manager_->action()(138, invitation_.sender);                                // (hidden) Info
    else if (id == 2) closeRequested();
  }
  void closeRequested() override { decline(); hide(); deleteLater(); }
private:
  void decline() { if (handled_) return; handled_ = true; manager_->client()->respondToChatInvitation(invitation_, false); } // reason 1: declined
  ChatWindows *manager_;
  aim::oscar::ChatInvitation invitation_;
  QString info_;
  bool handled_ = false;
};

// ---------------------------------------------------------------- manager -------------------------------
ChatWindows::ChatWindows(OscarClient *client, Action action, QObject *parent) : QObject(parent), client_(client), action_(std::move(action)) {
  connect(client_, &OscarClient::chatRoomReady, this, [this](const aim::oscar::ChatRoom &room) {
    for (const auto &window : rooms_) if (window && ((!window->cookie().isEmpty() && window->cookie() == room.cookie) || (window->cookie().isEmpty() && normalized(window->name()) == normalized(room.name)))) { window->joined(room); return; }
  });
  connect(client_, &OscarClient::chatParticipantsChanged, this, [this](const QString &cookie, const QVector<aim::oscar::UserInfo> &users) { for (const auto &window : rooms_) if (window && window->cookie() == cookie) window->participants(users); });
  connect(client_, &OscarClient::chatMessageReceived, this, [this](const QString &cookie, const QString &sender, const QString &text) { for (const auto &window : rooms_) if (window && window->cookie() == cookie) window->message(sender, text); });
  connect(client_, &OscarClient::chatRoomClosed, this, [this](const QString &cookie, const QString &) { for (const auto &window : rooms_) if (window && window->cookie() == cookie) window->closedByServer(); });
  connect(client_, &OscarClient::chatInvitationReceived, this, [this](const aim::oscar::ChatInvitation &invitation) {
    if (prefs::blockChatInvitations()) { client_->respondToChatInvitation(invitation, false); return; } // reason 2 in the original
    auto *window = new ChatInviteReceiveWindow(this, invitation); window->show(); window->requestActivate();
  });
}
ChatWindows::~ChatWindows() { for (const auto &window : rooms_) delete window.data(); }
ChatRoomWindow *ChatWindows::findRoom(const QString &name) const { for (const auto &window : rooms_) if (window && normalized(window->name()) == normalized(name)) return window; return nullptr; }
void ChatWindows::invite(const QStringList &names) {
  if (!client_->connected()) { errorBox(nullptr, aimString(1621)); return; }
  // With a Chat Room open, RT_DIALOG 101 asks "Current Room" (576) / "New Room" (577) first.
  ChatRoomWindow *current = nullptr; for (const auto &window : rooms_) if (window) current = window;
  if (current) {
#ifdef Q_OS_WIN
    const INT_PTR choice = runOriginalDialog(nullptr, 101, [](HWND) {}, [](HWND dialog, int id, int) { if (id == 576 || id == 577) { EndDialog(dialog, id); return true; } return false; });
    if (choice == 576) { inviteToRoom(current, names); return; }
    if (choice != 577) return;
#endif
  }
  auto *window = new ChatInviteWindow(this, names, nullptr); window->show(); window->requestActivate();
}
void ChatWindows::inviteToRoom(ChatRoomWindow *room, const QStringList &names) { auto *window = new ChatInviteWindow(this, names, room); window->show(); window->requestActivate(); }
bool ChatWindows::startRoom(const QString &name, const QStringList &names, const QString &message) {
  rooms_.removeIf([](const QPointer<ChatRoomWindow> &window) { return window.isNull(); });
  if (rooms_.size() >= 3) return false; // at most 3 Chat Rooms (0x11b0959a)
  auto *window = new ChatRoomWindow(this, name, int(rooms_.size())); rooms_.append(window);
  window->queueInvites(names, message); window->show(); window->requestActivate();
  client_->createChatRoom(name, 4); // private exchange 4
  return true;
}
void ChatWindows::accept(const aim::oscar::ChatInvitation &invitation) {
  rooms_.removeIf([](const QPointer<ChatRoomWindow> &window) { return window.isNull(); });
  for (const auto &window : rooms_) if (window && window->cookie() == invitation.room.cookie) { window->showNormal(); window->raise(); return; }
  if (rooms_.size() >= 3) { errorBox(nullptr, aimString(755)); return; }
  auto *window = new ChatRoomWindow(this, invitation.room.name, int(rooms_.size())); rooms_.append(window);
  window->setState(ChatRoomWindow::Joining); window->show(); window->requestActivate();
  client_->respondToChatInvitation(invitation, true);
}
void ChatWindows::preview() {
  // Developer preview (--ui-preview=chat): the three windows with sample data, without a connection.
  auto *invite = new ChatInviteWindow(this, {QStringLiteral("ExampleBuddy")}, nullptr); invite->show();
  aim::oscar::ChatInvitation invitation; invitation.sender = QStringLiteral("ExampleBuddy"); invitation.message = aimString(701); invitation.room.name = QStringLiteral("ExampleBuddy Chat17");
  auto *received = new ChatInviteReceiveWindow(this, invitation); received->setFramePosition(received->framePosition() + QPoint(60, 60)); received->show();
  auto *room = new ChatRoomWindow(this, QStringLiteral("ExampleUser Chat58"), 0); rooms_.append(room);
  aim::oscar::ChatRoom info; info.name = room->name(); info.cookie = QStringLiteral("preview"); room->joined(info);
  aim::oscar::UserInfo self; self.screenName = QStringLiteral("ExampleUser"); aim::oscar::UserInfo other; other.screenName = QStringLiteral("ExampleBuddy");
  room->participants({self}); room->participants({self, other});
  room->message(QStringLiteral("ExampleBuddy"), QStringLiteral("Hi everyone")); room->message(QStringLiteral("ExampleUser"), QStringLiteral("Hello ExampleBuddy")); room->show();
}
void ChatWindows::roomClosed(ChatRoomWindow *room) { rooms_.removeIf([room](const QPointer<ChatRoomWindow> &window) { return window.isNull() || window.data() == room; }); }
