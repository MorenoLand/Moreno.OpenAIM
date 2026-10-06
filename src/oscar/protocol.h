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
QByteArray encodeInstantMessage(const QString &recipient, const QString &text, quint64 cookie);
bool decodeInstantMessage(const QByteArray &bytes, InstantMessage *message, QString *error);
QByteArray encodeChatRoom(const ChatRoom &room, bool create = false);
bool decodeChatRoom(const QByteArray &bytes, ChatRoom *room, QString *error);
QByteArray encodeChatMessage(const QString &text, quint64 cookie);
bool decodeChatMessage(const QByteArray &bytes, QString *sender, QString *text, QString *error);
QByteArray encodeChatInvitation(const QString &recipient, const ChatInvitation &invitation, quint16 type = 0);
bool decodeChatInvitation(const InstantMessage &message, ChatInvitation *invitation, quint16 *type, QString *error);
QString oscarError(quint16 code);
}
Q_DECLARE_METATYPE(aim::oscar::UserInfo)
Q_DECLARE_METATYPE(aim::oscar::ChatRoom)
Q_DECLARE_METATYPE(aim::oscar::ChatInvitation)
