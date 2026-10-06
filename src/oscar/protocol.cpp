#include "protocol.h"
#include <QCryptographicHash>
#include <QStringConverter>

namespace aim::oscar {
namespace {
quint16 read16(const char *p) { return (quint16(quint8(p[0])) << 8) | quint8(p[1]); }
quint32 read32(const char *p) { return (quint32(quint8(p[0])) << 24) | (quint32(quint8(p[1])) << 16) | (quint32(quint8(p[2])) << 8) | quint8(p[3]); }
void append16(QByteArray &out, quint16 v) { out.append(char(v >> 8)); out.append(char(v)); }
void append32(QByteArray &out, quint32 v) { out.append(char(v >> 24)); out.append(char(v >> 16)); out.append(char(v >> 8)); out.append(char(v)); }
}
QByteArray encodeFlap(quint8 channel, quint16 sequence, const QByteArray &payload) {
  if (payload.size() > 0xffff) return {};
  QByteArray out;
  out.reserve(payload.size() + 6);
  out.append(char(0x2a));
  out.append(char(channel));
  append16(out, sequence);
  append16(out, quint16(payload.size()));
  out.append(payload);
  return out;
}
ParseResult decodeNextFlap(QByteArray &buffer, FlapFrame *frame, QString *error) {
  if (buffer.size() < 6) return ParseResult::NeedMore;
  if (quint8(buffer[0]) != 0x2a) {
    if (error) *error = QStringLiteral("Invalid FLAP start marker");
    return ParseResult::Invalid;
  }
  const quint16 length = read16(buffer.constData() + 4);
  const qsizetype frameSize = qsizetype(length) + 6;
  if (buffer.size() < frameSize) return ParseResult::NeedMore;
  frame->channel = quint8(buffer[1]);
  frame->sequence = read16(buffer.constData() + 2);
  frame->payload = buffer.mid(6, length);
  buffer.remove(0, frameSize);
  return ParseResult::Ready;
}
QByteArray encodeSnac(quint16 family, quint16 subgroup, quint16 flags, quint32 requestId, const QByteArray &body) {
  QByteArray out;
  out.reserve(body.size() + 10);
  append16(out, family);
  append16(out, subgroup);
  append16(out, flags);
  append32(out, requestId);
  out.append(body);
  return out;
}
bool decodeSnac(const QByteArray &payload, Snac *snac, QString *error) {
  if (payload.size() < 10) {
    if (error) *error = QStringLiteral("Truncated SNAC header");
    return false;
  }
  snac->family = read16(payload.constData());
  snac->subgroup = read16(payload.constData() + 2);
  snac->flags = read16(payload.constData() + 4);
  snac->requestId = read32(payload.constData() + 6);
  qsizetype offset = 10;
  if (snac->flags & 0x8000) { if (payload.size() < 12) { if (error) *error = QStringLiteral("Truncated SNAC extension"); return false; } offset = 12 + read16(payload.constData() + 10); if (offset > payload.size()) { if (error) *error = QStringLiteral("Truncated SNAC extension data"); return false; } }
  snac->body = payload.mid(offset);
  return true;
}
QByteArray encodeTlv(quint16 tag, const QByteArray &value) {
  if (value.size() > 0xffff) return {};
  QByteArray out;
  out.reserve(value.size() + 4);
  append16(out, tag);
  append16(out, quint16(value.size()));
  out.append(value);
  return out;
}
bool decodeTlvs(const QByteArray &bytes, QVector<Tlv> *tlvs, QString *error) {
  tlvs->clear();
  qsizetype offset = 0;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < 4) {
      if (error) *error = QStringLiteral("Truncated TLV header");
      return false;
    }
    const quint16 tag = read16(bytes.constData() + offset);
    const quint16 length = read16(bytes.constData() + offset + 2);
    offset += 4;
    if (bytes.size() - offset < length) {
      if (error) *error = QStringLiteral("Truncated TLV value");
      return false;
    }
    Tlv tlv;
    tlv.tag = tag;
    tlv.value = bytes.mid(offset, length);
    tlvs->append(tlv);
    offset += length;
  }
  return true;
}
QByteArray weakMd5PasswordHash(const QByteArray &password, const QByteArray &authKey) {
  return QCryptographicHash::hash(authKey + password + QByteArrayLiteral("AOL Instant Messenger (SM)"), QCryptographicHash::Md5);
}
bool decodeFeedbagReply(const QByteArray &body, QVector<FeedbagItem> *items, QString *error) {
  items->clear();
  const auto invalid = [&]() { items->clear(); if (error) *error = QStringLiteral("Malformed feedbag reply"); return false; };
  if (body.size() < 7) {
    if (error) *error = QStringLiteral("Truncated feedbag reply");
    return false;
  }
  qsizetype offset = 1;
  const quint16 count = read16(body.constData() + offset);
  offset += 2;
  for (quint16 i = 0; i < count; ++i) {
    if (body.size() - offset < 2) return invalid();
    const quint16 nameLength = read16(body.constData() + offset);
    offset += 2;
    if (body.size() - offset < qsizetype(nameLength) + 8) return invalid();
    FeedbagItem item;
    item.name = QString::fromUtf8(body.constData() + offset, nameLength);
    offset += nameLength;
    item.groupId = read16(body.constData() + offset);
    item.itemId = read16(body.constData() + offset + 2);
    item.classId = read16(body.constData() + offset + 4);
    offset += 6;
    const quint16 attributeLength = read16(body.constData() + offset);
    offset += 2;
    if (body.size() - offset < qsizetype(attributeLength) + 4) return invalid();
    if (!decodeTlvs(body.mid(offset,attributeLength),&item.attributes,error)) return false;
    offset += attributeLength;
    items->append(item);
  }
  if (body.size() - offset != 4) return invalid();
  return true;
}
bool decodeUserScreenName(const QByteArray &body, QString *screenName) {
  if (body.isEmpty()) return false;
  const quint8 length = quint8(body[0]);
  if (body.size() < qsizetype(length) + 1) return false;
  *screenName = QString::fromUtf8(body.constData() + 1, length);
  return !screenName->isEmpty();
}
namespace {
void append64(QByteArray &out, quint64 value) { append32(out,quint32(value>>32)); append32(out,quint32(value)); }
quint64 read64(const char *p) { return (quint64(read32(p))<<32)|read32(p+4); }
struct Reader {
  const QByteArray &bytes; qsizetype offset = 0;
  bool take(qsizetype count,QByteArray *value){if(count<0||count>bytes.size()-offset)return false;*value=bytes.mid(offset,count);offset+=count;return true;}
  bool number16(quint16 *value){if(bytes.size()-offset<2)return false;*value=read16(bytes.constData()+offset);offset+=2;return true;}
  bool name(QString *value){if(offset>=bytes.size())return false;quint8 size=quint8(bytes[offset++]);QByteArray text;if(!take(size,&text))return false;*value=QString::fromUtf8(text);return true;}
  bool tlvs(QVector<Tlv> *values,bool counted){values->clear();quint16 count=0;if(counted&&!number16(&count))return false;for(quint32 i=0;counted?i<count:offset<bytes.size();++i){Tlv value;quint16 length;if(!number16(&value.tag)||!number16(&length)||!take(length,&value.value))return false;values->append(value);}return true;}
};
QByteArray valueOf(const QVector<Tlv> &values,quint16 tag){for(const Tlv &value:values)if(value.tag==tag)return value.value;return {};}
bool invalid(QString *error,const QString &message){if(error)*error=message;return false;}
bool encodedText(const QByteArray &bytes,const QString &charset,QString *text,QString *error){QString name=charset.trimmed().toLower();QStringConverter::Encoding encoding=QStringConverter::Utf8;if(name=="us-ascii"||name=="ascii"||name=="iso-8859-1"||name=="iso 8859"||name=="latin1")encoding=QStringConverter::Latin1;else if(name=="unicode-2-0"||name=="ucs-2"||name=="utf-16be")encoding=QStringConverter::Utf16BE;else if(!name.isEmpty()&&name!="utf-8"&&name!="utf8")return invalid(error,QStringLiteral("Unsupported message charset: %1").arg(charset));QStringDecoder decoder(encoding);*text=decoder(bytes);if(decoder.hasError())return invalid(error,QStringLiteral("Malformed encoded message text"));return true;}
QString mimeCharset(const QString &mime){qsizetype start=mime.indexOf(QStringLiteral("charset="),0,Qt::CaseInsensitive);if(start<0)return {};QString charset=mime.mid(start+8).section(';',0,0).trimmed();if(charset.startsWith('"')&&charset.endsWith('"'))charset=charset.mid(1,charset.size()-2);return charset;}
bool screenNameBytes(const QString &name,QByteArray *bytes){*bytes=name.trimmed().toUtf8();return !bytes->isEmpty()&&bytes->size()<=255;}
bool appendCounted(QByteArray &bytes,const QVector<Tlv> &values){if(values.size()>65535)return false;append16(bytes,quint16(values.size()));for(const Tlv &value:values){QByteArray tlv=encodeTlv(value.tag,value.value);if(tlv.isEmpty())return false;bytes.append(tlv);}return true;}
}
bool decodeUserInfo(const QByteArray &bytes,qsizetype *offset,UserInfo *user,QString *error){Reader reader{bytes,*offset};*user={};if(!reader.name(&user->screenName)||user->screenName.isEmpty()||!reader.number16(&user->warningLevel)||!reader.tlvs(&user->attributes,true))return invalid(error,QStringLiteral("Malformed OSCAR user information"));*offset=reader.offset;QByteArray value=valueOf(user->attributes,1);if(value.size()==2)user->flags=read16(value.constData());value=valueOf(user->attributes,3);if(value.size()==4)user->signOnTime=read32(value.constData());value=valueOf(user->attributes,4);if(value.size()==2)user->idleMinutes=read16(value.constData());value=valueOf(user->attributes,5);if(value.size()==4)user->memberSince=read32(value.constData());return true;}
bool decodeUserInfoReply(const QByteArray &bytes,UserInfo *user,QString *error){qsizetype offset=0;if(!decodeUserInfo(bytes,&offset,user,error))return false;QVector<Tlv> info;if(!decodeTlvs(bytes.mid(offset),&info,error))return false;user->profileMime=QString::fromLatin1(valueOf(info,1));user->awayMime=QString::fromLatin1(valueOf(info,3));return encodedText(valueOf(info,2),mimeCharset(user->profileMime),&user->profile,error)&&encodedText(valueOf(info,4),mimeCharset(user->awayMime),&user->away,error);}
QByteArray encodeInstantMessage(const QString &recipient,const QString &text,quint64 cookie){QByteArray name;if(!screenNameBytes(recipient,&name)||text.isEmpty())return {};bool ascii=true;for(QChar ch:text)if(ch.unicode()>127){ascii=false;break;}QByteArray content;if(ascii)content=text.toLatin1();else for(QChar ch:text)append16(content,ch.unicode());if(content.size()>65520)return {};QByteArray fragment=QByteArray::fromHex("050100030101020101");append16(fragment,quint16(content.size()+4));append16(fragment,ascii?0:2);append16(fragment,0);fragment.append(content);QByteArray out;append64(out,cookie);append16(out,1);out.append(char(name.size()));out.append(name);out.append(encodeTlv(2,fragment));out.append(encodeTlv(3,{}));return out.size()+10<=65535?out:QByteArray();}
bool decodeInstantMessage(const QByteArray &bytes,InstantMessage *message,QString *error){if(bytes.size()<10)return invalid(error,QStringLiteral("Truncated instant message"));*message={};message->cookie=read64(bytes.constData());message->channel=read16(bytes.constData()+8);qsizetype offset=10;if(!decodeUserInfo(bytes,&offset,&message->sender,error)||!decodeTlvs(bytes.mid(offset),&message->attributes,error))return false;if(message->channel!=1)return true;QByteArray fragments=valueOf(message->attributes,2);Reader reader{fragments};bool found=false;while(reader.offset<fragments.size()){if(fragments.size()-reader.offset<4)return invalid(error,QStringLiteral("Truncated instant message fragment"));quint8 id=quint8(fragments[reader.offset++]);reader.offset++;quint16 length;QByteArray data;if(!reader.number16(&length)||!reader.take(length,&data))return invalid(error,QStringLiteral("Malformed instant message fragment"));if(id!=1)continue;if(data.size()<4)return invalid(error,QStringLiteral("Truncated instant message text"));quint16 charset=read16(data.constData());QString encoding=charset==0?QStringLiteral("us-ascii"):charset==2?QStringLiteral("utf-16be"):charset==3?QStringLiteral("iso-8859-1"):QString();if(encoding.isEmpty())return invalid(error,QStringLiteral("Unsupported ICBM charset %1").arg(charset));QString text;if(!encodedText(data.mid(4),encoding,&text,error))return false;message->text.append(text);found=true;}return found?true:invalid(error,QStringLiteral("Instant message contains no text fragment"));}
QByteArray encodeChatRoom(const ChatRoom &room,bool create){QByteArray cookie=(create?QStringLiteral("create"):room.cookie).toUtf8();if(cookie.isEmpty()||cookie.size()>255)return {};QByteArray out;append16(out,room.exchange);out.append(char(cookie.size()));out.append(cookie);append16(out,room.instance);out.append(char(room.detail));QVector<Tlv> values=room.attributes;if(create){values.clear();values.append(Tlv{0xd3,room.name.toUtf8()});}if(!appendCounted(out,values))return {};return out;}
bool decodeChatRoom(const QByteArray &bytes,ChatRoom *room,QString *error){Reader reader{bytes};*room={};if(!reader.number16(&room->exchange)||!reader.name(&room->cookie)||!reader.number16(&room->instance)||reader.offset>=bytes.size())return invalid(error,QStringLiteral("Malformed chat room metadata"));room->detail=quint8(bytes[reader.offset++]);if(!reader.tlvs(&room->attributes,true)||reader.offset!=bytes.size())return invalid(error,QStringLiteral("Malformed chat room attributes"));room->name=QString::fromUtf8(valueOf(room->attributes,0xd3));return !room->cookie.isEmpty()?true:invalid(error,QStringLiteral("Chat room cookie is empty"));}
QByteArray encodeChatMessage(const QString &text,quint64 cookie){if(text.isEmpty())return {};QByteArray data=text.toUtf8();if(data.size()>65000)return {};QByteArray info=encodeTlv(1,data)+encodeTlv(2,QByteArrayLiteral("utf-8"))+encodeTlv(3,QByteArrayLiteral("en"));QByteArray out;append64(out,cookie);append16(out,3);out.append(encodeTlv(1,{}));out.append(encodeTlv(5,info));out.append(encodeTlv(6,{}));return out;}
bool decodeChatMessage(const QByteArray &bytes,QString *sender,QString *text,QString *error){if(bytes.size()<10)return invalid(error,QStringLiteral("Truncated chat message"));QVector<Tlv> values;if(!decodeTlvs(bytes.mid(10),&values,error))return false;QByteArray userBytes=valueOf(values,3);UserInfo user;qsizetype offset=0;if(!decodeUserInfo(userBytes,&offset,&user,error)||offset!=userBytes.size())return invalid(error,QStringLiteral("Malformed chat sender"));*sender=user.screenName;QVector<Tlv> info;if(!decodeTlvs(valueOf(values,5),&info,error))return false;bool hasText=false;for(const Tlv &v:info)hasText=hasText||v.tag==1;if(!hasText)return invalid(error,QStringLiteral("Chat message has no text"));return encodedText(valueOf(info,1),QString::fromLatin1(valueOf(info,2)),text,error);}
QByteArray encodeChatInvitation(const QString &recipient,const ChatInvitation &invitation,quint16 type){QByteArray name;if(!screenNameBytes(recipient,&name)||type>2||(type==0&&invitation.message.toUtf8().size()>65000))return {};QByteArray fragment;append16(fragment,type);append64(fragment,invitation.cookie);fragment.append(QByteArray::fromHex("748f2420628711d18222444553540000"));if(type==0){QByteArray sequence;append16(sequence,1);QByteArray room;QByteArray cookie=invitation.room.cookie.toUtf8();if(cookie.isEmpty()||cookie.size()>255)return {};append16(room,invitation.room.exchange);room.append(char(cookie.size()));room.append(cookie);append16(room,invitation.room.instance);fragment.append(encodeTlv(0x0a,sequence));fragment.append(encodeTlv(0x0c,invitation.message.toUtf8()));fragment.append(encodeTlv(0x0d,QByteArrayLiteral("utf-8")));fragment.append(encodeTlv(0x0e,QByteArrayLiteral("en")));fragment.append(encodeTlv(0x2711,room));}else if(type==1){QByteArray reason;append16(reason,1);fragment.append(encodeTlv(0x0b,reason));}QByteArray out;append64(out,invitation.cookie);append16(out,2);out.append(char(name.size()));out.append(name);QByteArray tlv=encodeTlv(5,fragment);if(tlv.isEmpty())return {};out.append(tlv);out.append(encodeTlv(3,{}));return out.size()+10<=65535?out:QByteArray();}
bool decodeChatInvitation(const InstantMessage &message,ChatInvitation *invitation,quint16 *type,QString *error){QByteArray fragment=valueOf(message.attributes,5);if(fragment.size()<26)return invalid(error,QStringLiteral("Truncated chat invitation"));if(fragment.mid(10,16)!=QByteArray::fromHex("748f2420628711d18222444553540000"))return false;*invitation={};invitation->sender=message.sender.screenName;*type=read16(fragment.constData());invitation->cookie=read64(fragment.constData()+2);if(invitation->cookie!=message.cookie)return invalid(error,QStringLiteral("Chat invitation cookie mismatch"));QVector<Tlv> values;if(!decodeTlvs(fragment.mid(26),&values,error))return false;if(*type!=0)return true;QByteArray bytes=valueOf(values,0x2711);Reader reader{bytes};if(!reader.number16(&invitation->room.exchange)||!reader.name(&invitation->room.cookie)||!reader.number16(&invitation->room.instance)||reader.offset!=bytes.size())return invalid(error,QStringLiteral("Malformed chat invitation room"));return encodedText(valueOf(values,0x0c),QString::fromLatin1(valueOf(values,0x0d)),&invitation->message,error);}
QString oscarError(quint16 code){QString text;switch(code){case 1:text=QStringLiteral("Invalid OSCAR request");break;case 2:case 3:text=QStringLiteral("Server rate limit exceeded");break;case 4:text=QStringLiteral("User is not logged on or is unavailable");break;case 5:text=QStringLiteral("Service unavailable");break;case 8:text=QStringLiteral("Service unsupported by server");break;case 0x0d:text=QStringLiteral("Request denied");break;case 0x0e:text=QStringLiteral("Malformed OSCAR request");break;case 0x0f:text=QStringLiteral("Insufficient permissions");break;case 0x10:text=QStringLiteral("Blocked by your privacy settings");break;case 0x11:text=QStringLiteral("Sender warning level is too high");break;case 0x12:text=QStringLiteral("Recipient warning level is too high");break;case 0x13:text=QStringLiteral("User temporarily unavailable");break;case 0x14:text=QStringLiteral("Requested item not found");break;case 0x15:text=QStringLiteral("Server list limit exceeded");break;case 0x17:text=QStringLiteral("Server queue full");break;case 0x1a:text=QStringLiteral("Request timed out");break;default:text=QStringLiteral("OSCAR request failed");break;}return QStringLiteral("%1 (0x%2)").arg(text).arg(code,4,16,QLatin1Char('0'));}
QByteArray encodeFeedbagItems(const QVector<FeedbagItem> &items){QByteArray out;for(const auto &item:items){QByteArray name=item.name.toUtf8(),attributes;for(const Tlv &value:item.attributes){QByteArray bytes=encodeTlv(value.tag,value.value);if(bytes.isEmpty())return {};attributes.append(bytes);}if(name.size()>65535||attributes.size()>65535)return {};append16(out,quint16(name.size()));out.append(name);append16(out,item.groupId);append16(out,item.itemId);append16(out,item.classId);append16(out,quint16(attributes.size()));out.append(attributes);}return out;}
bool decodeFeedbagItems(const QByteArray &body,QVector<FeedbagItem> *items,QString *error){items->clear();Reader reader{body};while(reader.offset<body.size()){FeedbagItem item;quint16 size;QByteArray name,attributes;if(!reader.number16(&size)||!reader.take(size,&name)||!reader.number16(&item.groupId)||!reader.number16(&item.itemId)||!reader.number16(&item.classId)||!reader.number16(&size)||!reader.take(size,&attributes)||!decodeTlvs(attributes,&item.attributes,error))return invalid(error,QStringLiteral("Malformed feedbag edit item"));item.name=QString::fromUtf8(name);items->append(item);}return true;}
}
