#include "client.h"
#include <QHostAddress>
#include <utility>
#include <QRandomGenerator>
#include <QNetworkProxy>
#include <algorithm>

namespace {
void append16(QByteArray &out, quint16 value) { out.append(char(value >> 8)); out.append(char(value)); }
void append32(QByteArray &out, quint32 value) { out.append(char(value >> 24)); out.append(char(value >> 16)); out.append(char(value >> 8)); out.append(char(value)); }
quint16 read16(const QByteArray &bytes, qsizetype offset) { return (quint16(quint8(bytes[offset])) << 8) | quint8(bytes[offset + 1]); }
quint64 read64(const QByteArray &bytes,qsizetype offset){quint64 value=0;for(int i=0;i<8;++i)value=(value<<8)|quint8(bytes[offset+i]);return value;}
QString tlvText(const QVector<aim::oscar::Tlv> &tlvs, quint16 tag) { for (const auto &tlv : tlvs) if (tlv.tag == tag) return QString::fromUtf8(tlv.value); return {}; }
QByteArray tlvBytes(const QVector<aim::oscar::Tlv> &tlvs, quint16 tag) { for (const auto &tlv : tlvs) if (tlv.tag == tag) return tlv.value; return {}; }
bool splitHostPort(const QString &address, QString *host, quint16 *port) {
  QString portText;
  if (address.startsWith(QLatin1Char('['))) {
    const qsizetype end = address.indexOf(QLatin1Char(']'));
    if (end < 0 || end + 1 >= address.size() || address[end + 1] != QLatin1Char(':')) return false;
    *host = address.mid(1, end - 1);
    portText = address.mid(end + 2);
  } else {
    const qsizetype colon = address.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0) return false;
    *host = address.left(colon);
    portText = address.mid(colon + 1);
  }
  bool ok = false;
  const uint value = portText.toUInt(&ok);
  if (!ok || value == 0 || value > 65535) return false;
  *port = quint16(value);
  return !host->isEmpty();
}
quint32 rosterKey(const aim::oscar::FeedbagItem &item){return (quint32(item.groupId)<<16)|item.itemId;}
bool sameRosterItem(const aim::oscar::FeedbagItem &a,const aim::oscar::FeedbagItem &b){return aim::oscar::encodeFeedbagItems({a})==aim::oscar::encodeFeedbagItems({b});}
quint16 freeRosterId(const QVector<aim::oscar::FeedbagItem> &items,bool group){QSet<quint16> ids;for(const auto &item:items)ids.insert(group?item.groupId:item.itemId);for(quint32 id=1;id<=65535;++id)if(!ids.contains(quint16(id)))return quint16(id);return 0;}
bool namedGroup(const QVector<aim::oscar::FeedbagItem> &items,quint16 id){for(const auto &item:items)if(item.classId==1&&item.groupId==id&&id!=0)return true;return false;}
QString buddyKey(const QString &name){QString key=name.toCaseFolded();key.remove(QLatin1Char(' '));return key;}
void setOrder(aim::oscar::FeedbagItem &group,const QVector<quint16> &members){QVector<quint16> order;for(const auto &attribute:group.attributes)if(attribute.tag==0xc8)for(qsizetype offset=0;offset+1<attribute.value.size();offset+=2){quint16 id=read16(attribute.value,offset);if(members.contains(id)&&!order.contains(id))order.append(id);}for(quint16 id:members)if(!order.contains(id))order.append(id);QByteArray bytes;for(quint16 id:order)append16(bytes,id);for(auto &attribute:group.attributes)if(attribute.tag==0xc8){attribute.value=bytes;return;}group.attributes.append(aim::oscar::Tlv{0xc8,bytes});}
}

OscarClient::OscarClient(QObject *parent) : QObject(parent) {
  rosterTimeout_.setSingleShot(true);rosterTimeout_.setInterval(30000);connect(&rosterTimeout_,&QTimer::timeout,this,[this]{if(rosterRefreshing_){pending_.remove(rosterRefreshRequest_);emit operationFailed(QStringLiteral("Refresh Buddy List"),QStringLiteral("Server did not return the authoritative Buddy List"));cancelRosterEdit();}else if(rosterEditing_){emit operationFailed(QStringLiteral("Save Buddy List"),QStringLiteral("Server did not acknowledge the roster change"));finishRosterCluster(false);}});
  qRegisterMetaType<aim::oscar::UserInfo>();qRegisterMetaType<aim::oscar::ChatRoom>();qRegisterMetaType<aim::oscar::ChatInvitation>();qRegisterMetaType<QVector<aim::oscar::UserInfo>>();
  connect(&socket_, &QTcpSocket::connected, this, &OscarClient::onConnected);
  connect(&socket_, &QTcpSocket::readyRead, this, &OscarClient::onReadyRead);
  connect(&socket_, &QTcpSocket::errorOccurred, this, &OscarClient::onSocketError);
  connect(&socket_,&QTcpSocket::disconnected,this,[this]{if(!switchingSocket_&&phase_!=Phase::Idle&&phase_!=Phase::Failed)fail(QStringLiteral("OSCAR connection closed"));});
}
OscarClient::~OscarClient(){rosterTimeout_.stop();closeServices();}
void OscarClient::signOn(const QString &host, quint16 port, const QString &name, const QString &password) {
  cancelRosterEdit();
  closeServices();pending_.clear();maxMessageLength_=0;
  phase_ = Phase::Idle;
  socket_.abort();
  receiveBuffer_.clear();
  roster_.clear();
  onlineBuddies_.clear();
  authHost_ = host.trimmed();
  authPort_ = port;
  screenName_ = name.trimmed();
  password_ = password.toUtf8();
  sequence_ = 0;
  requestId_ = 1;
  phase_ = Phase::AuthConnecting;
  emit loginStageChanged(1);
  emit statusChanged(QStringLiteral("Connecting"));
  socket_.connectToHost(authHost_, authPort_);
}
void OscarClient::signOff() {
  cancelRosterEdit();
  closeServices();pending_.clear();if(away_){away_=false;emit awayChanged(false);}emit loginStageChanged(0);
  if (socket_.state() == QAbstractSocket::ConnectedState) sendFrame(4, {});
  phase_ = Phase::Idle;
  password_.fill(char(0));
  password_.clear();
  socket_.disconnectFromHost();
}
const QVector<aim::oscar::FeedbagItem> &OscarClient::roster() const { return roster_; }
bool OscarClient::isOnline(const QString &name) const { return onlineBuddies_.contains(name.toCaseFolded()); }
QString OscarClient::screenName() const { return screenName_; }
bool OscarClient::connected() const { return phase_ == Phase::Online; }
void OscarClient::onConnected() {
  if (phase_ == Phase::AuthConnecting) {
    sequence_ = 0;
    phase_ = Phase::AuthChallenge;
    emit loginStageChanged(2);
    emit statusChanged(QStringLiteral("Authenticating"));
    QByteArray signon;
    append32(signon, 1);
    sendFrame(1, signon);
    if (phase_ == Phase::Failed) return;
    sendSnac(0x17, 0x06, aim::oscar::encodeTlv(0x0001, screenName_.toUtf8()));
  } else if (phase_ == Phase::BosConnecting) {
    sequence_ = 0;
    phase_ = Phase::BosHost;
    QByteArray signon;
    append32(signon, 1);
    signon.append(aim::oscar::encodeTlv(0x0006, cookie_));
    sendFrame(1, signon);
    if (phase_ == Phase::Failed) return;
    emit statusChanged(QStringLiteral("Loading Buddy List"));
  }
}
void OscarClient::onReadyRead() {
  receiveBuffer_.append(socket_.readAll());
  while (true) {
    aim::oscar::FlapFrame frame;
    QString error;
    const auto result = aim::oscar::decodeNextFlap(receiveBuffer_, &frame, &error);
    if (result == aim::oscar::ParseResult::NeedMore) return;
    if (result == aim::oscar::ParseResult::Invalid) { fail(error); return; }
    handleFrame(frame);
    if (phase_ == Phase::Failed || phase_ == Phase::Idle) return;
  }
}
void OscarClient::onSocketError(QAbstractSocket::SocketError) {
  if (!switchingSocket_ && phase_ != Phase::Idle && phase_ != Phase::Failed)
    fail(socket_.errorString());
}
void OscarClient::sendFrame(quint8 channel, const QByteArray &payload) {
  const QByteArray frame = aim::oscar::encodeFlap(channel, sequence_++, payload);
  if (frame.isEmpty() || socket_.write(frame) != frame.size()) fail(QStringLiteral("Unable to write FLAP frame"));
}
void OscarClient::sendSnac(quint16 family, quint16 subgroup, const QByteArray &body) {
  sendFrame(2, aim::oscar::encodeSnac(family, subgroup, 0, requestId_++, body));
}
void OscarClient::handleFrame(const aim::oscar::FlapFrame &frame) {
  if (frame.channel == 3) { fail(QStringLiteral("OSCAR server rejected the connection")); return; }
  if (frame.channel == 4) { fail(QStringLiteral("OSCAR server closed the connection")); return; }
  if (frame.channel != 2) return;
  aim::oscar::Snac snac;
  QString error;
  if (!aim::oscar::decodeSnac(frame.payload, &snac, &error)) { fail(error); return; }
  handleSnac(snac);
}
void OscarClient::handleSnac(const aim::oscar::Snac &snac) {
  if(handleRosterEdits(snac))return;
  if(handleMessaging(snac))return;
  if (phase_ == Phase::AuthChallenge && snac.family == 0x17 && snac.subgroup == 0x07) {
    if (snac.body.size() < 2 || read16(snac.body, 0) != snac.body.size() - 2) { fail(QStringLiteral("Malformed BUCP challenge")); return; }
    const QByteArray authKey = snac.body.mid(2);
    const QByteArray hash = aim::oscar::weakMd5PasswordHash(password_, authKey);
    QByteArray body = aim::oscar::encodeTlv(0x0001, screenName_.toUtf8());
    body.append(aim::oscar::encodeTlv(0x0025, hash));
    sendSnac(0x17, 0x02, body);
    phase_ = Phase::AuthLogin;
    return;
  }
  if ((phase_ == Phase::AuthChallenge || phase_ == Phase::AuthLogin) && snac.family == 0x17 && snac.subgroup == 0x03) {
    QVector<aim::oscar::Tlv> tlvs;
    QString error;
    if (!aim::oscar::decodeTlvs(snac.body, &tlvs, &error)) { fail(error); return; }
    const QByteArray errorCode = tlvBytes(tlvs, 0x0008);
    if (!errorCode.isEmpty()) { const quint32 code = errorCode.size() >= 2 ? read16(errorCode, 0) : quint8(errorCode[0]); fail(QStringLiteral("Sign-on failed (OSCAR error %1)").arg(code)); return; }
    const QByteArray cookie = tlvBytes(tlvs, 0x0006);
    const QString reconnect = tlvText(tlvs, 0x0005);
    QString host;
    quint16 port = 0;
    if (cookie.isEmpty() || !splitHostPort(reconnect, &host, &port)) { fail(QStringLiteral("The server returned an invalid BOS endpoint")); return; }
    beginBos(host, port, cookie);
    return;
  }
  if (phase_ == Phase::BosHost && snac.family == 0x01 && snac.subgroup == 0x03) { beginRoster(); return; }
  if ((phase_ == Phase::BosRoster || phase_ == Phase::Online) && snac.family == 0x13 && snac.subgroup == 0x06) {
    QVector<aim::oscar::FeedbagItem> items;
    QString error;
    if (!aim::oscar::decodeFeedbagReply(snac.body, &items, &error)) { fail(error); return; }
    const bool initialSignOn = phase_ == Phase::BosRoster;
    auto before = roster_;
    roster_ = std::move(items);
    if (initialSignOn) {
      sendSnac(0x13, 0x07);
      if (phase_ == Phase::Failed) return;
      phase_ = Phase::Online;
      sendSnac(0x01, 0x02);
      if (phase_ == Phase::Failed) return;
      sendBuddyRequests();
      if (phase_ == Phase::Failed) return;
      sendSnac(0x02,0x04,aim::oscar::encodeTlv(5,QByteArray::fromHex("748f2420628711d18222444553540000")));
      if (phase_ == Phase::Failed) return;
      sendSnac(0x04,0x04);
      if (phase_ == Phase::Failed) return;
      password_.fill(char(0));
      password_.clear();
      emit statusChanged(QStringLiteral("Online"));
      emit rosterReady();
    }
    pending_.remove(snac.requestId);
    emit rosterChanged();
    if(!initialSignOn)syncBuddySubscriptions(before);
    if(rosterRefreshing_&&snac.requestId==rosterRefreshRequest_){rosterTimeout_.stop();pending_.remove(rosterRefreshRequest_);rosterRefreshing_=false;rosterEditing_=false;rosterSteps_.clear();emit rosterEditFinished(rosterEditOk_);}
    return;
  }
  if ((phase_ == Phase::BosRoster || phase_ == Phase::Online) && snac.family == 0x03 && (snac.subgroup == 0x0b || snac.subgroup == 0x0c)) {
    QString name;
    if (aim::oscar::decodeUserScreenName(snac.body, &name)) setBuddyOnline(name, snac.subgroup == 0x0b);
  }
}
void OscarClient::beginBos(const QString &host, quint16 port, const QByteArray &cookie) {
  emit loginStageChanged(3);
  cookie_ = cookie;
  bosHost_ = host;
  bosPort_ = port;
  switchingSocket_ = true;
  socket_.abort();
  switchingSocket_ = false;
  receiveBuffer_.clear();
  sequence_ = 0;
  phase_ = Phase::BosConnecting;
  emit statusChanged(QStringLiteral("Connecting to BOS"));
  socket_.connectToHost(bosHost_, bosPort_);
}
void OscarClient::beginRoster() {
  phase_ = Phase::BosRoster;
  QByteArray versions;
  for (const auto [family, version] : {qMakePair(quint16(0x01), quint16(4)), qMakePair(quint16(0x02), quint16(1)), qMakePair(quint16(0x03), quint16(1)), qMakePair(quint16(0x04), quint16(1)), qMakePair(quint16(0x13), quint16(1))}) { append16(versions, family); append16(versions, version); }
  sendSnac(0x01, 0x17, versions);
  if (phase_ == Phase::Failed) return;
  sendSnac(0x01, 0x06);
  if (phase_ == Phase::Failed) return;
  sendSnac(0x13, 0x02);
  if (phase_ == Phase::Failed) return;
  sendSnac(0x13, 0x04);
}
void OscarClient::sendBuddyRequests() {
  QByteArray body;
  for (const auto &item : roster_) if (item.classId == 0) { const QByteArray name = item.name.toUtf8(); if (!name.isEmpty() && name.size() <= 255) { body.append(char(name.size())); body.append(name); } }
  if (!body.isEmpty()) sendSnac(0x03, 0x04, body);
}
void OscarClient::setBuddyOnline(const QString &name, bool online) {
  const QString key = name.toCaseFolded();
  const bool changed = online ? !onlineBuddies_.contains(key) : onlineBuddies_.contains(key);
  if (online) onlineBuddies_.insert(key); else onlineBuddies_.remove(key);
  emit rosterChanged();
  if (changed) emit buddyPresenceChanged(name, online);
}
void OscarClient::fail(const QString &reason) {
  if (phase_ == Phase::Failed || phase_ == Phase::Idle) return;
  phase_ = Phase::Failed;
  cancelRosterEdit();
  closeServices();pending_.clear();if(away_){away_=false;emit awayChanged(false);}emit loginStageChanged(0);
  password_.fill(char(0));
  password_.clear();
  socket_.abort();
  emit failed(reason);
}
quint32 OscarClient::request(quint16 family,quint16 subgroup,const QByteArray &body,const QString &operation,const aim::oscar::ChatRoom &room){quint32 id=requestId_;pending_.insert(id,Pending{operation,family,room});sendSnac(family,subgroup,body);return id;}
bool OscarClient::sendMessage(const QString &recipient,const QString &text){QString operation=QStringLiteral("IM to %1").arg(recipient);if(!connected()){emit operationFailed(operation,QStringLiteral("Not signed on"));return false;}quint64 cookie=QRandomGenerator::global()->generate64();QByteArray body=aim::oscar::encodeInstantMessage(recipient,text,cookie);qsizetype textBytes=text.size();for(QChar c:text)if(c.unicode()>127){textBytes=text.size()*2;break;}if(body.isEmpty()||(maxMessageLength_&&textBytes>maxMessageLength_)){emit operationFailed(operation,body.isEmpty()?QStringLiteral("Invalid recipient or message size"):QStringLiteral("Message exceeds the server limit of %1 bytes").arg(maxMessageLength_));return false;}request(4,6,body,operation);return connected();}
void OscarClient::requestUserInfo(const QString &name){QByteArray screenName=name.trimmed().toUtf8();QString operation=QStringLiteral("Get Info for %1").arg(name);if(!connected()||screenName.isEmpty()||screenName.size()>255){emit operationFailed(operation,!connected()?QStringLiteral("Not signed on"):QStringLiteral("Invalid screen name"));return;}QByteArray body;append16(body,3);body.append(char(screenName.size()));body.append(screenName);request(2,5,body,operation);}
void OscarClient::createChatRoom(const QString &name,quint16 exchange){if(!connected()||name.trimmed().isEmpty()||name.toUtf8().size()>65500){emit operationFailed(QStringLiteral("Create chat room"),!connected()?QStringLiteral("Not signed on"):QStringLiteral("Invalid room name"));return;}aim::oscar::ChatRoom room;room.name=name.trimmed();room.exchange=exchange;chatCreates_.append(room);auto service=services_.value(QStringLiteral("nav"));if(service&&service->ready)flushChatCreates();else if(!chatNavRequested_){chatNavRequested_=true;requestService(0x0d);}}
void OscarClient::joinChatRoom(const aim::oscar::ChatRoom &room){if(!connected()||room.cookie.isEmpty()||room.cookie.toUtf8().size()>255){emit operationFailed(QStringLiteral("Join chat room"),!connected()?QStringLiteral("Not signed on"):QStringLiteral("Invalid room cookie"));return;}QString key=QStringLiteral("room:")+room.cookie;if(services_.contains(key)){auto service=services_.value(key);if(service->announced)emit chatRoomReady(service->room);return;}for(const Pending &pending:pending_)if(pending.family==0x0e&&pending.room.cookie==room.cookie)return;requestService(0x0e,room);}
void OscarClient::requestService(quint16 family,const aim::oscar::ChatRoom &room){QByteArray body;append16(body,family);if(family==0x0e){QByteArray metadata;append16(metadata,room.exchange);QByteArray cookie=room.cookie.toUtf8();metadata.append(char(cookie.size()));metadata.append(cookie);append16(metadata,room.instance);body.append(aim::oscar::encodeTlv(1,metadata));}quint32 id=request(1,4,body,family==0x0d?QStringLiteral("Chat navigation"):QStringLiteral("Join chat room"),room);if(pending_.contains(id))pending_[id].family=family;}
bool OscarClient::sendChatMessage(const QString &roomCookie,const QString &text){auto service=services_.value(QStringLiteral("room:")+roomCookie);if(!service||!service->ready){emit operationFailed(QStringLiteral("Send chat message"),QStringLiteral("Chat room is not connected"));return false;}QByteArray body=aim::oscar::encodeChatMessage(text,QRandomGenerator::global()->generate64());quint16 max=0;for(const auto &tlv:service->room.attributes)if(tlv.tag==0xd1&&tlv.value.size()==2)max=read16(tlv.value,0);if(body.isEmpty()||(max&&text.toUtf8().size()>max)){emit operationFailed(QStringLiteral("Send chat message"),body.isEmpty()?QStringLiteral("Invalid chat message size"):QStringLiteral("Chat message exceeds the room limit of %1 bytes").arg(max));return false;}return sendService(service,0x0e,5,body,QStringLiteral("Send chat message"));}
bool OscarClient::inviteToChat(const QString &recipient,const QString &roomCookie,const QString &text){auto service=services_.value(QStringLiteral("room:")+roomCookie);if(!connected()||!service||!service->ready){emit operationFailed(QStringLiteral("Invite to chat"),QStringLiteral("Chat room is not connected"));return false;}aim::oscar::ChatInvitation invitation;invitation.cookie=QRandomGenerator::global()->generate64();invitation.room=service->room;invitation.message=text;QByteArray body=aim::oscar::encodeChatInvitation(recipient,invitation);if(body.isEmpty()){emit operationFailed(QStringLiteral("Invite to chat"),QStringLiteral("Invalid chat invitation"));return false;}request(4,6,body,QStringLiteral("Invite %1 to chat").arg(recipient));return connected();}
void OscarClient::respondToChatInvitation(const aim::oscar::ChatInvitation &invitation,bool accept){if(!connected()){emit operationFailed(QStringLiteral("Chat invitation"),QStringLiteral("Not signed on"));return;}QByteArray body=aim::oscar::encodeChatInvitation(invitation.sender,invitation,accept?2:1);if(body.isEmpty()){emit operationFailed(QStringLiteral("Chat invitation"),QStringLiteral("Invalid invitation response"));return;}request(4,6,body,QStringLiteral("Chat invitation response"));if(accept&&connected())joinChatRoom(invitation.room);}
void OscarClient::leaveChatRoom(const QString &cookie){auto service=services_.value(QStringLiteral("room:")+cookie);if(service)closeService(service,{});for(auto it=pending_.begin();it!=pending_.end();){if(it->family==0x0e&&it->room.cookie==cookie)it=pending_.erase(it);else ++it;}}
void OscarClient::openService(quint16 family,const aim::oscar::ChatRoom &room,const QString &host,quint16 port,const QByteArray &cookie){auto service=QSharedPointer<Service>::create();service->key=family==0x0d?QStringLiteral("nav"):QStringLiteral("room:")+room.cookie;service->family=family;service->room=room;service->cookie=cookie;service->socket=new QTcpSocket(this);service->socket->setProxy(socket_.proxy());services_.insert(service->key,service);connect(service->socket,&QTcpSocket::connected,this,[this,service]{QByteArray body;append32(body,1);body.append(aim::oscar::encodeTlv(6,service->cookie));QByteArray frame=aim::oscar::encodeFlap(1,service->sequence++,body);if(service->socket->write(frame)!=frame.size())closeService(service,QStringLiteral("Unable to authenticate chat service"));});connect(service->socket,&QTcpSocket::readyRead,this,[this,service]{readService(service);});connect(service->socket,&QTcpSocket::errorOccurred,this,[this,service](QAbstractSocket::SocketError){closeService(service,service->socket->errorString());});connect(service->socket,&QTcpSocket::disconnected,this,[this,service]{if(!service->closing)closeService(service,QStringLiteral("Chat service connection closed"));});service->socket->connectToHost(host,port);}
bool OscarClient::sendService(const QSharedPointer<Service> &service,quint16 family,quint16 subgroup,const QByteArray &body,const QString &operation){if(service->closing)return false;quint32 id=requestId_++;if(!operation.isEmpty())service->requests.insert(id,operation);QByteArray frame=aim::oscar::encodeFlap(2,service->sequence++,aim::oscar::encodeSnac(family,subgroup,0,id,body));if(frame.isEmpty()||service->socket->write(frame)!=frame.size()){closeService(service,QStringLiteral("Unable to write chat service frame"));return false;}return true;}
void OscarClient::readService(const QSharedPointer<Service> &service){service->buffer.append(service->socket->readAll());while(!service->closing){aim::oscar::FlapFrame frame;QString error;auto result=aim::oscar::decodeNextFlap(service->buffer,&frame,&error);if(result==aim::oscar::ParseResult::NeedMore)return;if(result==aim::oscar::ParseResult::Invalid){closeService(service,error);return;}if(frame.channel==3||frame.channel==4){closeService(service,QStringLiteral("Chat service closed the connection"));return;}if(frame.channel!=2)continue;aim::oscar::Snac snac;if(!aim::oscar::decodeSnac(frame.payload,&snac,&error)){closeService(service,error);return;}handleService(service,snac);}}
void OscarClient::handleService(const QSharedPointer<Service> &service,const aim::oscar::Snac &snac){
  QString error;
  if(snac.subgroup==1&&(snac.family==1||snac.family==service->family)){QString operation=service->requests.take(snac.requestId);if(operation.isEmpty())operation=service->family==0x0d?QStringLiteral("Chat navigation"):QStringLiteral("Chat room");emit operationFailed(operation,snac.body.size()>=2?aim::oscar::oscarError(read16(snac.body,0)):QStringLiteral("Malformed chat service error"));if(!service->ready)closeService(service,QStringLiteral("Chat service negotiation failed"));return;}
  if(snac.family==1&&snac.subgroup==3){bool supported=false;for(qsizetype offset=0;offset+1<snac.body.size();offset+=2)supported=supported||read16(snac.body,offset)==service->family;if(!supported){closeService(service,QStringLiteral("Server does not support the requested chat service"));return;}QByteArray versions;append16(versions,1);append16(versions,4);append16(versions,service->family);append16(versions,1);sendService(service,1,0x17,versions);return;}
  if(snac.family==1&&snac.subgroup==0x18){for(qsizetype offset=0;offset+3<snac.body.size();offset+=4)if(read16(snac.body,offset)==1)service->osVersion=read16(snac.body,offset+2);sendService(service,1,6);return;}
  if(snac.family==1&&snac.subgroup==7){if(snac.body.size()<2){closeService(service,QStringLiteral("Truncated chat rate parameters"));return;}quint16 count=read16(snac.body,0);qsizetype size=service->osVersion>=2?35:30;if(snac.body.size()<2+count*size){closeService(service,QStringLiteral("Malformed chat rate parameters"));return;}QByteArray classes;for(quint16 i=0;i<count;++i)append16(classes,read16(snac.body,2+i*size));if(!sendService(service,1,8,classes))return;QByteArray versions;for(quint16 family:{quint16(1),service->family}){append16(versions,family);append16(versions,family==1?service->osVersion:1);append16(versions,0);append16(versions,0);}if(!sendService(service,1,2,versions))return;service->ready=true;if(service->family==0x0d){sendService(service,0x0d,2);flushChatCreates();}return;}
  if(service->family==0x0d&&snac.family==0x0d&&snac.subgroup==9){QVector<aim::oscar::Tlv> values;if(!aim::oscar::decodeTlvs(snac.body,&values,&error)){emit operationFailed(QStringLiteral("Chat navigation"),error);return;}for(const auto &value:values)if(value.tag==4){aim::oscar::ChatRoom room;if(!aim::oscar::decodeChatRoom(value.value,&room,&error)){emit operationFailed(service->requests.take(snac.requestId),error);return;}service->requests.remove(snac.requestId);joinChatRoom(room);}return;}
  if(service->family!=0x0e||snac.family!=0x0e)return;
  if(snac.subgroup==2){aim::oscar::ChatRoom room;if(!aim::oscar::decodeChatRoom(snac.body,&room,&error)){closeService(service,error);return;}if(room.cookie!=service->room.cookie){closeService(service,QStringLiteral("Chat room identity mismatch"));return;}service->room=room;service->announced=true;emit chatRoomReady(room);return;}
  if(snac.subgroup==3||snac.subgroup==4){qsizetype offset=0;QVector<aim::oscar::UserInfo> changes;while(offset<snac.body.size()){aim::oscar::UserInfo user;if(!aim::oscar::decodeUserInfo(snac.body,&offset,&user,&error)){closeService(service,error);return;}changes.append(user);}for(const auto &user:changes)if(snac.subgroup==3)service->participants.insert(user.screenName.toCaseFolded(),user);else service->participants.remove(user.screenName.toCaseFolded());QVector<aim::oscar::UserInfo> participants;for(const auto &user:service->participants)participants.append(user);std::sort(participants.begin(),participants.end(),[](const auto &a,const auto &b){return a.screenName.compare(b.screenName,Qt::CaseInsensitive)<0;});emit chatParticipantsChanged(service->room.cookie,participants);return;}
  if(snac.subgroup==6){QString sender,text;if(!aim::oscar::decodeChatMessage(snac.body,&sender,&text,&error)){emit operationFailed(QStringLiteral("Receive chat message"),error);return;}service->requests.remove(snac.requestId);emit chatMessageReceived(service->room.cookie,sender,text);return;}
}
void OscarClient::flushChatCreates(){auto service=services_.value(QStringLiteral("nav"));if(!service||!service->ready)return;QVector<aim::oscar::ChatRoom> rooms;rooms.swap(chatCreates_);for(const auto &room:rooms){QByteArray body=aim::oscar::encodeChatRoom(room,true);if(body.isEmpty())emit operationFailed(QStringLiteral("Create chat room"),QStringLiteral("Invalid room metadata"));else sendService(service,0x0d,8,body,QStringLiteral("Create chat room %1").arg(room.name));}}
void OscarClient::closeService(const QSharedPointer<Service> &service,const QString &reason){if(service->closing)return;service->closing=true;services_.remove(service->key);if(service->family==0x0d){chatNavRequested_=false;chatCreates_.clear();if(!reason.isEmpty())emit operationFailed(QStringLiteral("Chat navigation"),reason);}else{if(!service->announced&&!reason.isEmpty())emit operationFailed(QStringLiteral("Join chat room"),reason);emit chatRoomClosed(service->room.cookie,reason);}service->socket->disconnect(this);if(service->socket->state()==QAbstractSocket::ConnectedState)service->socket->write(aim::oscar::encodeFlap(4,service->sequence++,{}));service->socket->disconnectFromHost();service->socket->deleteLater();}
void OscarClient::closeServices(){auto services=services_.values();for(const auto &service:services)closeService(service,{});services_.clear();chatCreates_.clear();chatNavRequested_=false;}
bool OscarClient::handleMessaging(const aim::oscar::Snac &snac){
  if(phase_!=Phase::Online&&phase_!=Phase::BosRoster)return false;
  QString error;
  if(snac.family==4&&snac.subgroup==9&&pending_.contains(snac.requestId)){Pending pending=pending_.take(snac.requestId);if(snac.body.size()<4){emit operationFailed(pending.operation,QStringLiteral("Malformed warning reply"));return true;}emit warnCompleted(pending.operation.mid(5),read16(snac.body,0),read16(snac.body,2));return true;} // SNAC(04,09): delta applied, new level (tenths of a percent)
  if(snac.subgroup==1&&pending_.contains(snac.requestId)){Pending pending=pending_.take(snac.requestId);if(pending.family==0x0d){chatNavRequested_=false;chatCreates_.clear();}emit operationFailed(pending.operation,snac.body.size()>=2?aim::oscar::oscarError(read16(snac.body,0)):QStringLiteral("Malformed server error"));return true;}
  if(snac.family==1&&snac.subgroup==5){Pending pending=pending_.take(snac.requestId);if(pending.family!=0x0d&&pending.family!=0x0e)return true;QVector<aim::oscar::Tlv> values;if(!aim::oscar::decodeTlvs(snac.body,&values,&error)){emit operationFailed(pending.operation,error);if(pending.family==0x0d)chatNavRequested_=false;return true;}QByteArray group=tlvBytes(values,0x0d),cookie=tlvBytes(values,6);QString host;quint16 port=0;if(group.size()!=2||read16(group,0)!=pending.family||cookie.isEmpty()||!splitHostPort(tlvText(values,5),&host,&port)){emit operationFailed(pending.operation,QStringLiteral("Invalid chat service redirect"));if(pending.family==0x0d)chatNavRequested_=false;return true;}openService(pending.family,pending.room,host,port,cookie);return true;}
  if(snac.family==2&&snac.subgroup==6){aim::oscar::UserInfo info;Pending pending=pending_.take(snac.requestId);if(!aim::oscar::decodeUserInfoReply(snac.body,&info,&error))emit operationFailed(pending.operation.isEmpty()?QStringLiteral("Get Info"):pending.operation,error);else if(pending.operation==QStringLiteral("Set Away")||pending.operation==QStringLiteral("Clear Away")){bool actual=(info.flags&0x20)!=0;if(away_!=actual){away_=actual;emit awayChanged(actual);}if(actual!=(pending.operation==QStringLiteral("Set Away")))emit operationFailed(pending.operation,QStringLiteral("Server away state did not match the request"));}else emit userInfoReceived(info);return true;}
  if(snac.family==4&&snac.subgroup==5){if(snac.body.size()!=16){emit operationFailed(QStringLiteral("Message parameters"),QStringLiteral("Malformed ICBM parameters"));return true;}maxMessageLength_=read16(snac.body,6);QByteArray body;append16(body,0);body.append(snac.body.mid(2));sendSnac(4,2,body);return true;}
  if(snac.family==4&&snac.subgroup==0x0c){pending_.remove(snac.requestId);if(snac.body.size()<11){emit operationFailed(QStringLiteral("Message confirmation"),QStringLiteral("Malformed ICBM acknowledgment"));return true;}quint8 length=quint8(snac.body[10]);if(snac.body.size()!=11+length){emit operationFailed(QStringLiteral("Message confirmation"),QStringLiteral("Malformed ICBM acknowledgment recipient"));return true;}if(read16(snac.body,8)==1)emit messageAccepted(QString::fromUtf8(snac.body.mid(11,length)),read64(snac.body,0));return true;}
  if(snac.family==4&&snac.subgroup==7){aim::oscar::InstantMessage message;if(!aim::oscar::decodeInstantMessage(snac.body,&message,&error)){emit operationFailed(QStringLiteral("Receive message"),error);return true;}if(message.channel==1)emit messageReceived(message.sender.screenName,message.text);else if(message.channel==2){aim::oscar::ChatInvitation invitation;quint16 type=0;if(aim::oscar::decodeChatInvitation(message,&invitation,&type,&error)){if(type==0)emit chatInvitationReceived(invitation);else if(type==1)emit operationFailed(QStringLiteral("Chat invitation"),QStringLiteral("%1 declined the chat invitation").arg(invitation.sender));}else if(!error.isEmpty())emit operationFailed(QStringLiteral("Receive chat invitation"),error);}return true;}
  return false;
}
bool OscarClient::rosterEditPending()const{return rosterEditing_;}
bool OscarClient::rosterError(const QString &reason){emit operationFailed(QStringLiteral("Save Buddy List"),reason);return false;}
bool OscarClient::applyRoster(const QVector<aim::oscar::FeedbagItem> &desiredFullRoster){
  if(!connected())return rosterError(QStringLiteral("Not signed on"));if(rosterEditing_)return rosterError(QStringLiteral("A Buddy List change is awaiting server confirmation"));
  QVector<aim::oscar::FeedbagItem> desired=desiredFullRoster;bool master=false;for(const auto &item:desired)master=master||(item.classId==1&&item.groupId==0);if(!master)for(const auto &item:roster_)if(item.classId==1&&item.groupId==0){desired.append(item);master=true;break;}
  QHash<quint32,aim::oscar::FeedbagItem> old,updated;QVector<quint16> groups;for(const auto &item:roster_)old.insert(rosterKey(item),item);
  for(const auto &item:desired){if(updated.contains(rosterKey(item)))return rosterError(QStringLiteral("Duplicate Buddy List item IDs"));QByteArray bytes=aim::oscar::encodeFeedbagItems({item});if(bytes.isEmpty()||bytes.size()+10>65535)return rosterError(QStringLiteral("Buddy List item exceeds the wire size limit"));if(item.classId==1){if(item.groupId!=0&&item.name.trimmed().isEmpty())return rosterError(QStringLiteral("Invalid Buddy List group"));if(item.groupId!=0)groups.append(item.groupId);}else if(item.classId==0){if(item.itemId==0||item.groupId==0||item.name.trimmed().isEmpty()||item.name.toUtf8().size()>255)return rosterError(QStringLiteral("Invalid buddy item"));}else if(item.classId==3&&!old.contains(rosterKey(item))){if(item.itemId==0||item.groupId!=0||item.name.trimmed().isEmpty()||item.name.toUtf8().size()>255)return rosterError(QStringLiteral("Invalid block list entry"));}else if(!old.contains(rosterKey(item))||!sameRosterItem(old.value(rosterKey(item)),item))return rosterError(QStringLiteral("List Setup cannot change non-buddy records"));updated.insert(rosterKey(item),item);}
  for(const auto &item:roster_)if(item.classId!=0&&item.classId!=1&&!updated.contains(rosterKey(item)))return rosterError(QStringLiteral("List Setup must preserve non-buddy records"));
  for(const auto &item:desired)if(item.classId==0&&!groups.contains(item.groupId))return rosterError(QStringLiteral("Buddy item refers to a missing group"));
  if(!master&&!groups.isEmpty()){aim::oscar::FeedbagItem item;item.classId=1;desired.append(item);}
  for(auto &item:desired)if(item.classId==1){QVector<quint16> members;if(item.groupId==0)members=groups;else for(const auto &buddy:desired)if(buddy.classId==0&&buddy.groupId==item.groupId)members.append(buddy.itemId);setOrder(item,members);}
  updated.clear();for(const auto &item:desired)updated.insert(rosterKey(item),item);rosterSteps_.clear();
  for(const auto &item:roster_)if(item.classId==0&&(!updated.contains(rosterKey(item))||updated.value(rosterKey(item)).classId!=0))rosterSteps_.append(RosterStep{0x0a,item});
  for(const auto &item:desired)if(item.classId==1&&item.groupId!=0&&!old.contains(rosterKey(item)))rosterSteps_.append(RosterStep{8,item});
  for(const auto &item:desired)if(item.classId==0){if(!old.contains(rosterKey(item)))rosterSteps_.append(RosterStep{8,item});else if(old.value(rosterKey(item)).classId!=0)return rosterError(QStringLiteral("Buddy item ID collides with another record"));else if(!sameRosterItem(old.value(rosterKey(item)),item))rosterSteps_.append(RosterStep{9,item});}
  for(const auto &item:roster_)if(item.classId==1&&item.groupId!=0&&!updated.contains(rosterKey(item)))rosterSteps_.append(RosterStep{0x0a,item});
  for(const auto &item:desired)if(item.classId==3&&!old.contains(rosterKey(item)))rosterSteps_.append(RosterStep{8,item}); // new block (deny) entry: SNAC(13,08) class 3
  for(const auto &item:desired)if(item.classId==1){if(!old.contains(rosterKey(item))){if(item.groupId==0)rosterSteps_.append(RosterStep{8,item});}else if(old.value(rosterKey(item)).classId!=1)return rosterError(QStringLiteral("Group item ID collides with another record"));else if(!sameRosterItem(old.value(rosterKey(item)),item))rosterSteps_.append(RosterStep{9,item});}
  for(const auto &step:rosterSteps_)if(step.subgroup==0x0a)for(const auto &other:roster_)if(other.itemId==step.item.itemId&&rosterKey(other)!=rosterKey(step.item)&&updated.contains(rosterKey(other)))return rosterError(QStringLiteral("Server cannot safely delete this item because another retained record shares its item ID"));
  if(rosterSteps_.isEmpty()){emit rosterEditFinished(true);return true;}rosterEditing_=true;rosterRefreshing_=false;rosterEditOk_=true;rosterStep_=0;rosterRequest_=0;rosterRefreshRequest_=0;sendSnac(0x13,0x11);if(!connected())return false;advanceRosterEdit();return connected();
}
bool OscarClient::addGroup(const QString &name){if(name.trimmed().isEmpty())return rosterError(QStringLiteral("Enter a group name"));auto desired=roster_;quint16 id=freeRosterId(desired,true);if(!id)return rosterError(QStringLiteral("No unused group IDs remain"));aim::oscar::FeedbagItem item;item.name=name.trimmed();item.groupId=id;item.classId=1;desired.append(item);return applyRoster(desired);}
bool OscarClient::renameGroup(quint16 groupId,const QString &name){auto desired=roster_;for(auto &item:desired)if(item.classId==1&&item.groupId==groupId&&groupId!=0){item.name=name.trimmed();return applyRoster(desired);}return rosterError(QStringLiteral("Group not found"));}
bool OscarClient::removeGroup(quint16 groupId){if(!namedGroup(roster_,groupId))return rosterError(QStringLiteral("Group not found"));auto desired=roster_;desired.erase(std::remove_if(desired.begin(),desired.end(),[groupId](const auto &item){return item.groupId==groupId&&(item.classId==0||item.classId==1);}),desired.end());return applyRoster(desired);}
bool OscarClient::blockUser(const QString &name){const QString key=buddyKey(name);if(key.isEmpty())return rosterError(QStringLiteral("Enter a screen name"));for(const auto &item:roster_)if(item.classId==3&&buddyKey(item.name)==key)return true;auto desired=roster_;quint16 id=freeRosterId(desired,false);if(!id)return rosterError(QStringLiteral("No unused item IDs remain"));aim::oscar::FeedbagItem item;item.name=name.trimmed();item.groupId=0;item.itemId=id;item.classId=3;desired.append(item);return applyRoster(desired);}
bool OscarClient::isBlocked(const QString &name)const{const QString key=buddyKey(name);for(const auto &item:roster_)if(item.classId==3&&buddyKey(item.name)==key)return true;return false;}
bool OscarClient::warnUser(const QString &name,bool anonymous){const QString operation=QStringLiteral("Warn %1").arg(name);QByteArray screenName=name.trimmed().toUtf8();if(!connected()){emit operationFailed(operation,QStringLiteral("Not signed on"));return false;}if(screenName.isEmpty()||screenName.size()>255){emit operationFailed(operation,QStringLiteral("Invalid screen name"));return false;}QByteArray body;append16(body,anonymous?1:0);body.append(char(screenName.size()));body.append(screenName);request(4,8,body,operation);return true;} // SNAC(04,08): SendAs u16 (0 named, 1 anonymous) + u8-length name
bool OscarClient::addBuddy(quint16 groupId,const QString &name){if(!namedGroup(roster_,groupId))return rosterError(QStringLiteral("Group not found"));for(const auto &item:roster_)if(item.classId==0&&item.groupId==groupId&&buddyKey(item.name)==buddyKey(name))return rosterError(QStringLiteral("Buddy already exists in this group"));auto desired=roster_;quint16 id=freeRosterId(desired,false);if(!id)return rosterError(QStringLiteral("No unused buddy IDs remain"));aim::oscar::FeedbagItem item;item.name=name.trimmed();item.groupId=groupId;item.itemId=id;desired.append(item);return applyRoster(desired);}
bool OscarClient::renameBuddy(quint16 groupId,quint16 itemId,const QString &name){auto desired=roster_;for(auto &item:desired)if(item.classId==0&&item.groupId==groupId&&item.itemId==itemId){item.name=name.trimmed();return applyRoster(desired);}return rosterError(QStringLiteral("Buddy not found"));}
bool OscarClient::moveBuddy(quint16 groupId,quint16 itemId,quint16 destinationGroupId){if(!namedGroup(roster_,destinationGroupId))return rosterError(QStringLiteral("Destination group not found"));auto desired=roster_;for(auto &item:desired)if(item.classId==0&&item.groupId==groupId&&item.itemId==itemId){quint32 destination=(quint32(destinationGroupId)<<16)|itemId;for(const auto &other:desired)if(rosterKey(other)==destination&&other.groupId!=groupId){quint16 id=freeRosterId(desired,false);if(!id)return rosterError(QStringLiteral("No unused buddy IDs remain"));item.itemId=id;break;}item.groupId=destinationGroupId;return applyRoster(desired);}return rosterError(QStringLiteral("Buddy not found"));}
bool OscarClient::removeBuddy(quint16 groupId,quint16 itemId){auto desired=roster_;auto found=std::find_if(desired.begin(),desired.end(),[=](const auto &item){return item.classId==0&&item.groupId==groupId&&item.itemId==itemId;});if(found==desired.end())return rosterError(QStringLiteral("Buddy not found"));desired.erase(found);return applyRoster(desired);}
void OscarClient::advanceRosterEdit(){if(!rosterEditing_||rosterRefreshing_)return;if(rosterStep_>=rosterSteps_.size()){finishRosterCluster(true);return;}const auto &step=rosterSteps_[rosterStep_];rosterRequest_=request(0x13,step.subgroup,aim::oscar::encodeFeedbagItems({step.item}),QStringLiteral("Save Buddy List"));if(connected()&&rosterEditing_)rosterTimeout_.start();}
void OscarClient::finishRosterCluster(bool success){if(!rosterEditing_||rosterRefreshing_)return;rosterTimeout_.stop();rosterEditOk_=success;pending_.remove(rosterRequest_);sendSnac(0x13,0x12);if(!connected())return;rosterRefreshing_=true;rosterRefreshRequest_=request(0x13,4,{},QStringLiteral("Refresh Buddy List"));if(connected()&&rosterEditing_)rosterTimeout_.start();}
void OscarClient::cancelRosterEdit(){bool active=rosterEditing_;rosterTimeout_.stop();rosterEditing_=false;rosterRefreshing_=false;rosterSteps_.clear();incomingCluster_=false;incomingRoster_.clear();if(active)emit rosterEditFinished(false);}
void OscarClient::syncBuddySubscriptions(const QVector<aim::oscar::FeedbagItem> &before){QSet<QString> oldNames,newNames;for(const auto &item:before)if(item.classId==0)oldNames.insert(buddyKey(item.name));for(const auto &item:roster_)if(item.classId==0)newNames.insert(buddyKey(item.name));for(quint16 subgroup:{quint16(5),quint16(4)}){QByteArray body;const auto &source=subgroup==5?before:roster_;for(const auto &item:source)if(item.classId==0){QString name=buddyKey(item.name);if((subgroup==5?!newNames.contains(name):!oldNames.contains(name))){QByteArray bytes=item.name.toUtf8();if(!bytes.isEmpty()&&bytes.size()<=255){if(body.size()+bytes.size()+11>65535){sendSnac(3,subgroup,body);body.clear();}body.append(char(bytes.size()));body.append(bytes);}}}if(!body.isEmpty())sendSnac(3,subgroup,body);}}
bool OscarClient::handleRosterEdits(const aim::oscar::Snac &snac){
  if(snac.family!=0x13||phase_!=Phase::Online)return false;
  if(snac.subgroup==0x0e&&rosterEditing_&&!rosterRefreshing_&&snac.requestId==rosterRequest_){rosterTimeout_.stop();pending_.remove(rosterRequest_);if(snac.body.size()!=2||read16(snac.body,0)!=0){emit operationFailed(QStringLiteral("Save Buddy List"),snac.body.size()==2?QStringLiteral("Server rejected the roster item (SSI status 0x%1)").arg(read16(snac.body,0),4,16,QLatin1Char('0')):QStringLiteral("Malformed roster acknowledgment"));finishRosterCluster(false);}else{++rosterStep_;advanceRosterEdit();}return true;}
  if(snac.subgroup==1&&rosterEditing_&&(snac.requestId==rosterRequest_||snac.requestId==rosterRefreshRequest_)){QString reason=snac.body.size()>=2?aim::oscar::oscarError(read16(snac.body,0)):QStringLiteral("Malformed roster error");emit operationFailed(rosterRefreshing_?QStringLiteral("Refresh Buddy List"):QStringLiteral("Save Buddy List"),reason);pending_.remove(snac.requestId);if(rosterRefreshing_)cancelRosterEdit();else finishRosterCluster(false);return true;}
  if(snac.subgroup==0x11){incomingCluster_=true;incomingRoster_=roster_;return true;}
  if(snac.subgroup==0x12){if(incomingCluster_){auto before=roster_;roster_=std::move(incomingRoster_);incomingCluster_=false;emit rosterChanged();syncBuddySubscriptions(before);}return true;}
  if(snac.subgroup==8||snac.subgroup==9||snac.subgroup==0x0a){QVector<aim::oscar::FeedbagItem> changes;QString error;if(!aim::oscar::decodeFeedbagItems(snac.body,&changes,&error)){emit operationFailed(QStringLiteral("Update Buddy List"),error);return true;}auto before=roster_;auto &items=incomingCluster_?incomingRoster_:roster_;for(const auto &change:changes){auto found=std::find_if(items.begin(),items.end(),[&change](const auto &item){return rosterKey(item)==rosterKey(change);});if(snac.subgroup==0x0a){if(found!=items.end())items.erase(found);}else if(found==items.end())items.append(change);else *found=change;}if(!incomingCluster_){emit rosterChanged();syncBuddySubscriptions(before);}return true;}
  return false;
}

bool OscarClient::away()const{return away_;}
bool OscarClient::setAway(const QString &text){if(!connected()||text.isEmpty()){emit operationFailed(QStringLiteral("Set Away"),!connected()?QStringLiteral("Not signed on"):QStringLiteral("Enter an away message"));return false;}QString escaped=text.toHtmlEscaped().replace(QLatin1Char('\n'),QStringLiteral("<br>"));QString html=QStringLiteral("<html>");for(uint ch:escaped.toUcs4())if(ch<128)html.append(QChar(ushort(ch)));else html.append(QStringLiteral("&#%1;").arg(ch));html.append(QStringLiteral("</html>"));QByteArray bytes=html.toLatin1();if(bytes.size()>65000){emit operationFailed(QStringLiteral("Set Away"),QStringLiteral("Away message exceeds the wire size limit"));return false;}sendSnac(2,4,aim::oscar::encodeTlv(3,QByteArrayLiteral("text/aolrtf; charset=\"us-ascii\""))+aim::oscar::encodeTlv(4,bytes));if(!connected())return false;QByteArray name=screenName_.toUtf8(),query;append16(query,2);query.append(char(name.size()));query.append(name);request(2,5,query,QStringLiteral("Set Away"));return connected();}
bool OscarClient::clearAway(){if(!connected()){emit operationFailed(QStringLiteral("Clear Away"),QStringLiteral("Not signed on"));return false;}sendSnac(2,4,aim::oscar::encodeTlv(3,QByteArrayLiteral("text/aolrtf; charset=\"us-ascii\""))+aim::oscar::encodeTlv(4,{}));if(!connected())return false;QByteArray name=screenName_.toUtf8(),query;append16(query,2);query.append(char(name.size()));query.append(name);request(2,5,query,QStringLiteral("Clear Away"));return connected();}
