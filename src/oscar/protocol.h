#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>
#include <QMetaType>

namespace aim::oscar {
struct FlapFrame { quint8 channel = 0; quint16 sequence = 0; QByteArray payload; };
struct Snac { quint16 family = 0; quint16 subgroup = 0; quint16 flags = 0; quint32 requestId = 0; QByteArray body; };
struct Tlv { quint16 tag = 0; QByteArray value; };
struct FeedbagItem { QString name; quint16 groupId = 0; quint16 itemId = 0; quint16 classId = 0; QVector<Tlv> attributes; };
struct UserInfo { QString screenName; quint16 warningLevel = 0; quint16 flags = 0; quint32 memberSince = 0; quint32 signOnTime = 0; quint16 idleMinutes = 0; QString profileMime; QString profile; QString awayMime; QString away; QVector<Tlv> attributes; };
struct ChatRoom { QString name; QString cookie; quint16 exchange = 4; quint16 instance = 0; quint8 detail = 2; QVector<Tlv> attributes; };
struct ChatInvitation { QString sender; QString message; quint64 cookie = 0; ChatRoom room; };
// ICBM channel-2 rendezvous (TLV 5 fragment: u16 type | 8-byte cookie | 16-byte capability | TLVs).
// Types: 0 propose, 1 cancel, 2 accept. TLVs: 02 rendezvous IP, 03 requester IP, 04 verified IP, 05 port,
// 0A sequence, 0B cancel reason, 2711 service data.
struct Rendezvous { QString sender; quint16 type = 0; quint64 cookie = 0; QByteArray capability; QVector<Tlv> values; };
inline QByteArray capDirectIm() { return QByteArray::fromHex("094613454c7f11d18222444553540000"); }   // "IM Image"
inline QByteArray capVoice() { return QByteArray::fromHex("094613414c7f11d18222444553540000"); }      // Talk
inline QByteArray capChat() { return QByteArray::fromHex("748f2420628711d18222444553540000"); }
struct InstantMessage { quint64 cookie = 0; quint16 channel = 0; UserInfo sender; QString text; QVector<Tlv> attributes; };
enum class ParseResult { NeedMore, Ready, Invalid };
QByteArray encodeFlap(quint8 channel, quint16 sequence, const QByteArray &payload);
ParseResult decodeNextFlap(QByteArray &buffer, FlapFrame *frame, QString *error);
QByteArray encodeSnac(quint16 family, quint16 subgroup, quint16 flags, quint32 requestId, const QByteArray &body);
bool decodeSnac(const QByteArray &payload, Snac *snac, QString *error);
QByteArray encodeTlv(quint16 tag, const QByteArray &value);
bool decodeTlvs(const QByteArray &bytes, QVector<Tlv> *tlvs, QString *error);
QByteArray weakMd5PasswordHash(const QByteArray &password, const QByteArray &authKey);
bool decodeFeedbagReply(const QByteArray &body, QVector<FeedbagItem> *items, QString *error);
QByteArray encodeFeedbagItems(const QVector<FeedbagItem> &items);
bool decodeFeedbagItems(const QByteArray &body, QVector<FeedbagItem> *items, QString *error);
bool decodeUserScreenName(const QByteArray &body, QString *screenName);
bool decodeUserInfo(const QByteArray &bytes, qsizetype *offset, UserInfo *user, QString *error);
bool decodeUserInfoReply(const QByteArray &bytes, UserInfo *user, QString *error);
QByteArray encodeInstantMessage(const QString &recipient, const QString &text, quint64 cookie, bool autoResponse = false); // autoResponse: TLV 4 instead of the ack request TLV 3
bool decodeInstantMessage(const QByteArray &bytes, InstantMessage *message, QString *error);
QByteArray encodeChatRoom(const ChatRoom &room, bool create = false);
bool decodeChatRoom(const QByteArray &bytes, ChatRoom *room, QString *error);
QByteArray encodeChatMessage(const QString &text, quint64 cookie);
bool decodeChatMessage(const QByteArray &bytes, QString *sender, QString *text, QString *error);
QByteArray encodeRendezvous(const QString &recipient, const Rendezvous &rendezvous);
bool decodeRendezvous(const InstantMessage &message, Rendezvous *rendezvous, QString *error);
QByteArray encodeChatInvitation(const QString &recipient, const ChatInvitation &invitation, quint16 type = 0);
bool decodeChatInvitation(const InstantMessage &message, ChatInvitation *invitation, quint16 *type, QString *error);
QString oscarError(quint16 code);
}
Q_DECLARE_METATYPE(aim::oscar::UserInfo)
Q_DECLARE_METATYPE(aim::oscar::ChatRoom)
Q_DECLARE_METATYPE(aim::oscar::ChatInvitation)
Q_DECLARE_METATYPE(aim::oscar::Rendezvous)
