#include "ctl_group.h"
#include <QFile>
#include <QtEndian>

namespace {
class Reader {
public:
  explicit Reader(const QByteArray &bytes) : data_(bytes) {}
  quint16 u16(qsizetype offset) const { return offset + 2 <= data_.size() ? qFromLittleEndian<quint16>(data_.constData() + offset) : 0; }
  quint32 u32(qsizetype offset) const { return offset + 4 <= data_.size() ? qFromLittleEndian<quint32>(data_.constData() + offset) : 0; }
private:
  const QByteArray &data_;
};
}

CtlObject *CtlObject::find(quint32 objectId) {
  if (id == objectId) return this;
  for (const auto &child : children) if (CtlObject *found = child->find(objectId)) return found;
  return nullptr;
}

std::shared_ptr<CtlObject> parseCtlGroup(const QByteArray &data) {
  auto root = std::make_shared<CtlObject>(); root->kind = CtlObject::Kind::Group; root->vertical = true; root->flags = CtlObject::IsGroup;
  QList<CtlObject *> stack{root.get()};
  QSize pendingHint; QMargins pendingMargins; quint16 pendingAlign = 0; bool hasHint = false, hasMargins = false, hasAlign = false;
  auto add = [&](std::shared_ptr<CtlObject> object) {
    if (hasHint) object->sizeHint = pendingHint;
    if (hasMargins) object->margins = pendingMargins;
    if (hasAlign) object->align = pendingAlign;
    hasHint = hasMargins = hasAlign = false;
    stack.last()->children.append(object);
    return object.get();
  };
  qsizetype offset = 0;
  while (offset + 4 <= data.size()) {
    const Reader header(data); const quint16 type = header.u16(offset), length = header.u16(offset + 2);
    const QByteArray payload = data.mid(offset + 4, length); const Reader p(payload);
    offset += 4 + length;
    auto object = std::make_shared<CtlObject>(); object->raw = payload; object->id = p.u32(0);
    switch (type) {
    case 1: object->kind = CtlObject::Kind::Group; object->vertical = p.u16(4) != 0; object->packMode = p.u16(6); object->titleId = p.u32(8); object->flags = p.u32(12) | CtlObject::IsGroup; stack.append(add(object)); break;
    case 2: if (stack.size() > 1) stack.removeLast(); break;
    case 3: object->kind = CtlObject::Kind::Control; object->controlClass = p.u32(0); object->id = p.u32(4); object->minimum = QSize(p.u16(8), p.u16(10)); object->fontId = p.u32(12); object->textId = p.u32(16); object->tooltipId = p.u32(20); object->style = p.u32(24); object->flags = p.u32(28); add(object); break;
    case 4: pendingHint = QSize(p.u16(0), p.u16(2)); hasHint = true; break;
    case 5: pendingMargins = QMargins(p.u16(0), p.u16(2), p.u16(4), p.u16(6)); hasMargins = true; break;
    case 6: pendingAlign = p.u16(0); hasAlign = true; break;
    case 0x402: object->kind = CtlObject::Kind::Separator; object->minimum = QSize(p.u16(4), 0); object->flags = p.u32(6); add(object); break;
    case 0x403: object->kind = CtlObject::Kind::RateMeter; add(object); break;
    case 0x404: object->kind = CtlObject::Kind::Static; object->staticOrigin = QPoint(p.u16(4), p.u16(6)); object->minimum = QSize(p.u16(8), p.u16(10)); object->fontId = p.u32(12); object->textId = p.u32(16); object->tooltipId = p.u32(20); object->style = p.u32(24); object->flags = p.u32(28); add(object); break;
    case 0x406: object->kind = CtlObject::Kind::Combo; object->profileKeyId = p.u32(6); object->style = p.u32(22); object->flags = p.u32(26); add(object); break;
    case 0x407: object->kind = CtlObject::Kind::Button; object->buttonType = p.u16(4); object->placement = p.u16(6); object->gap = p.u16(8); for (int i = 0; i < 3; ++i) object->art[i] = p.u32(10 + i * 4); object->fontId = p.u32(22); object->textId = p.u32(26); object->tooltipId = p.u32(30); object->style = p.u32(34); object->flags = p.u32(38); object->valueId = p.u32(42); add(object); break;
    case 0x802: object->kind = CtlObject::Kind::Ate; object->minimum = QSize(p.u16(4), p.u16(6)); object->style = p.u32(12); object->flags = p.u32(16); add(object); break;
    default: break; // unknown record types are skipped by length, as the original loader does
    }
  }
  return root->children.size() == 1 && root->children.first()->kind == CtlObject::Kind::Group ? root->children.first() : root;
}

std::shared_ptr<CtlObject> loadCtlGroup(int resourceId) { QFile file(QStringLiteral(":/aim/ctlgroups/%1").arg(resourceId)); return file.open(QIODevice::ReadOnly) ? parseCtlGroup(file.readAll()) : nullptr; }
