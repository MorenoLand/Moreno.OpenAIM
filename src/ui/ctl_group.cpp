#include "ctl_group.h"
#include <QFile>
#include <QFontMetrics>
#include <QHash>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <algorithm>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
class Reader {
public:
  explicit Reader(const QByteArray &bytes) : data_(bytes) {}
  quint16 u16(qsizetype offset) const { return offset + 2 <= data_.size() ? qFromLittleEndian<quint16>(data_.constData() + offset) : 0; }
  quint32 u32(qsizetype offset) const { return offset + 4 <= data_.size() ? qFromLittleEndian<quint32>(data_.constData() + offset) : 0; }
private:
  const QByteArray &data_;
};
using Kind = CtlObject::Kind;
using Flag = CtlObject::Flag;
bool isGroup(const CtlObject &o) { return o.kind == Kind::Group || o.kind == Kind::TabGroup; }
// Win32 MulDiv: 64-bit intermediate, rounded to nearest with halves away from zero.
int mulDiv(int number, int numerator, int denominator) {
  if (!denominator) return -1;
  const qint64 product = qint64(number) * numerator; const bool negative = (product < 0) != (denominator < 0);
  const qint64 half = std::abs(qint64(denominator)) / 2; const qint64 magnitude = (std::abs(product) + half) / std::abs(qint64(denominator));
  return int(negative ? -magnitude : magnitude);
}
Kind controlKind(quint32 type) {
  switch (type) {
  case 2: case 0xd: case 0xe: return Kind::Button;
  case 3: return Kind::ListBox; case 0xb: return Kind::ComboBox; case 5: return Kind::Edit; case 9: return Kind::Tree;
  case 0x10: return Kind::ListView; case 0x12: return Kind::TreeView; case 0x11: return Kind::Trackbar;
  default: return Kind::Unknown;
  }
}
QMargins defaultMargins(Kind kind) {
  if (kind == Kind::Group || kind == Kind::Separator) return {};             // groups/separators do not take msg 7 defaults
  if (kind == Kind::TabGroup) return QMargins(10, 32, 10, 10);              // 0x122054ac
  return QMargins(1, 1, 1, 1);                                              // base class msg 7, 0x12203df4
}

// ---- ideal / minimum size (0x1220419d) ----
QSize buttonSize(const CtlObject &o, const CtlEnvironment &env, bool artButton) {
  const QString text = env.string(o.textId);
  const bool measureText = !text.isEmpty() && (!artButton || o.buttonType != 2);
  const QSize t = measureText ? env.textExtent(o.fontId, text, 0, CtlEnvironment::ButtonText) : QSize(0, 0);
  if (!artButton) return QSize(t.width() + 24, t.height() + 8);
  const QSize a = o.art[0] ? env.artSize(o.art[0]) : QSize(0, 0);
  int w = 0, h = 0;
  switch (o.placement) {
  case 0: w = std::max(a.width(), t.width()); h = o.gap + a.height() + t.height(); break; // art above text
  case 1: w = o.gap + a.width() + t.width(); h = std::max(a.height(), t.height()); break;  // art left of text
  default: w = a.width(); h = a.height(); break;                                            // art only
  }
  switch (o.buttonType) { case 2: return QSize(w + 2, h + 2); case 0: return QSize(w + 10, h + 9); default: return QSize(w + 24, h + 8); }
}
void idealSize(CtlObject &o, const CtlEnvironment &env);
void groupIdeal(CtlObject &g, const CtlEnvironment &env) {
  g.ideal = g.minimum = QSize(0, 0); g.visibleChildren = 0; int maxW = 0, maxH = 0;
  auto accumulate = [&g](QSize &total, const QSize &child, bool minimum) {
    Q_UNUSED(minimum);
    if (!g.vertical) total = QSize(total.width() + child.width(), std::max(total.height(), child.height()));
    else total = QSize(std::max(total.width(), child.width()), total.height() + child.height());
  };
  if (g.kind == Kind::TabGroup) {
    // 0x122057b9: the largest page wins; hidden pages are measured too.
    for (const auto &page : g.children) {
      const quint32 saved = page->flags; page->flags &= ~quint32(Flag::Hidden); idealSize(*page, env); page->flags = saved;
      g.ideal = g.ideal.expandedTo(page->ideal); g.minimum = g.minimum.expandedTo(page->minimum);
      if (page->hidden()) page->ideal = page->minimum = QSize(0, 0);
    }
    g.visibleChildren = 1; return;
  }
  for (const auto &child : g.children) {
    if (!child->hidden()) ++g.visibleChildren;
    idealSize(*child, env);
    accumulate(g.ideal, child->ideal, false); accumulate(g.minimum, child->minimum, true);
    if (!(child->flags & Flag::NoMaxSize)) { maxW = std::max(maxW, child->ideal.width()); maxH = std::max(maxH, child->ideal.height()); }
  }
  if (g.flags & Flag::Uniform) {
    g.ideal = QSize(0, 0);
    for (const auto &child : g.children) { if (!(child->flags & (Flag::Hidden | Flag::NoMaxSize))) child->ideal = QSize(maxW, maxH); accumulate(g.ideal, child->ideal, false); }
  }
  if (g.hint.width() && g.ideal.width() < g.hint.width()) g.ideal.setWidth(g.hint.width());
  if (g.hint.height() && g.ideal.height() < g.hint.height()) g.ideal.setHeight(g.hint.height());
}
void idealSize(CtlObject &o, const CtlEnvironment &env) {
  o.ideal = o.minimum = QSize(0, 0);
  if (o.hidden()) return;
  if (o.flags & Flag::DialogUnits) {
    const QSize unit = env.averageCharSize(o.fontId);
    o.base = QSize(o.base.width() * unit.width(), o.base.height() * unit.height()); o.hint = QSize(o.hint.width() * unit.width(), o.hint.height() * unit.height());
    o.flags &= ~quint32(Flag::DialogUnits);
  }
  if (o.flags & Flag::FixedSize) { o.ideal = o.base; o.minimum = o.hint; }
  else {
    const int scroll = env.verticalScrollWidth();
    switch (o.kind) {
    case Kind::Group: case Kind::TabGroup: groupIdeal(o, env); break;
    case Kind::Button: o.ideal = buttonSize(o, env, false); break;
    case Kind::ArtButton: o.ideal = buttonSize(o, env, true); break;
    case Kind::ListBox: case Kind::ListView: case Kind::TreeView: case Kind::Edit: case Kind::Tree:
      o.ideal = QSize(o.base.width() + scroll, o.base.height()); o.minimum = QSize(o.hint.width() + scroll, o.hint.height()); break;
    case Kind::ComboBox: case Kind::PersistentCombo: { const int h = 3 * env.averageCharSize(o.fontId).height() / 2; o.ideal = QSize(o.base.width() + scroll, h); o.minimum = QSize(o.hint.width() + scroll, h); break; }
    case Kind::RateMeter: o.ideal = QSize(48, 8); break; // RatemeterGetMinimumSize: (15+1)*3 x 8
    case Kind::Separator: o.ideal = o.parent && !o.parent->vertical ? QSize(2, o.base.height()) : QSize(o.base.width(), 2); break;
    case Kind::Static:
      o.ideal = o.base; o.minimum = o.hint;
      if (o.base.height() == 0) o.ideal = env.textExtent(o.fontId, env.string(o.textId), o.base.width(), o.base.width() ? CtlEnvironment::StaticWrapped : CtlEnvironment::StaticLine);
      break;
    case Kind::Ate:
      o.ideal = o.base; o.minimum = o.hint;
      // Style bit 0x1000 gives the ATE wrapper its 21 px formatting bar above the edit area (wndutils 0x1228e2d0,
      // Research/ate_toolbar.md); the bar is part of the wrapper window, so it adds to the pane's size.
      if (o.style & 0x1000) { o.ideal.rheight() += 21; o.minimum.rheight() += 21; }
      break;
    case Kind::Trackbar: case Kind::TabBody: case Kind::Unknown: o.ideal = o.base; o.minimum = o.hint; break;
    }
  }
  if (!(o.flags & Flag::StretchH)) o.minimum.setWidth(o.ideal.width());
  if (!(o.flags & Flag::StretchV)) o.minimum.setHeight(o.ideal.height());
  const int pad = ((o.style & 0x800000) || (o.flags & Flag::Padding)) ? 4 : 0;
  const QSize extra(o.margins.left() + o.margins.right() + pad, o.margins.top() + o.margins.bottom() + pad);
  o.ideal += extra; o.minimum += extra;
}

// ---- arrange (0x122047a4) ----
void arrange(CtlObject &o, int w, int h) {
  if (!isGroup(o)) { o.size = o.hidden() ? QSize(0, 0) : QSize(w, h); return; }
  CtlObject &g = o;
  if (g.hidden()) { g.size = g.content = QSize(0, 0); return; }
  auto stretchX = [](const CtlObject &c) { return (c.flags & Flag::StretchH) && !c.hidden(); };
  auto stretchY = [](const CtlObject &c) { return (c.flags & Flag::StretchV) && !c.hidden(); };
  int fixedMain = 0, fixedCross = 0, stretchMain = 0, stretchCross = 0;
  for (const auto &c : g.children) {
    c->flags &= ~quint32(Flag::Sized);
    if (g.vertical) { if (stretchX(*c)) stretchCross = std::max(stretchCross, c->ideal.width()); else fixedCross = std::max(fixedCross, c->ideal.width()); if (stretchY(*c)) stretchMain += c->ideal.height(); else fixedMain += c->ideal.height(); }
    else { if (stretchX(*c)) stretchMain += c->ideal.width(); else fixedMain += c->ideal.width(); if (stretchY(*c)) stretchCross = std::max(stretchCross, c->ideal.height()); else fixedCross = std::max(fixedCross, c->ideal.height()); }
  }
  Q_UNUSED(fixedCross); Q_UNUSED(stretchCross);
  w = std::max(w, g.minimum.width()); h = std::max(h, g.minimum.height());
  int free = g.vertical ? h - g.margins.bottom() - g.margins.top() - fixedMain : w - g.margins.right() - g.margins.left() - fixedMain;
  int total = stretchMain;
  bool changed = true; int sumMain = 0, maxCross = 0;
  while (changed) {
    changed = false; sumMain = 0; maxCross = 0;
    for (const auto &c : g.children) {
      int cw, ch;
      if (g.vertical) {
        cw = stretchX(*c) ? std::max(w - g.margins.right() - g.margins.left(), c->minimum.width()) : c->ideal.width();
        if (c->flags & Flag::Sized) ch = c->size.height();
        else if (stretchY(*c) && total != 0) {
          ch = mulDiv(c->ideal.height(), free, total);
          if (c->minimum.height() > ch && c->minimum.height() - ch > 1) { ch = c->minimum.height(); c->flags |= Flag::Sized; changed = true; free -= ch; total -= c->ideal.height(); }
        } else ch = c->ideal.height();
        arrange(*c, cw, ch); sumMain += ch; maxCross = std::max(maxCross, cw);
      } else {
        ch = stretchY(*c) ? std::max(h - g.margins.bottom() - g.margins.top(), c->minimum.height()) : c->ideal.height();
        if (c->flags & Flag::Sized) cw = c->size.width();
        else if (stretchX(*c) && total != 0) {
          cw = mulDiv(c->ideal.width(), free, total);
          if (c->minimum.width() > cw && c->minimum.width() - cw > 1) { cw = c->minimum.width(); c->flags |= Flag::Sized; changed = true; free -= cw; total -= c->ideal.width(); }
        } else cw = c->ideal.width();
        arrange(*c, cw, ch); sumMain += cw; maxCross = std::max(maxCross, ch);
      }
    }
  }
  g.content = g.vertical ? QSize(maxCross, sumMain) : QSize(sumMain, maxCross);
  g.size = QSize(w, h);
}

// ---- move (0x12204aa5) ----
int alignOffset(int mode, int available, int size) { if (mode == 3) return 0; if (mode == 4) return available - size; const int slack = available - size; return slack / 2; }
void packDistribution(int mode, int available, int used, int count, int &start, int &gap) {
  const int slack = std::max(0, available - used); start = gap = 0;
  switch (mode) {
  case 0: gap = count > 1 ? slack / (count - 1) : 0; break;
  case 1: start = gap = slack / (count + 1); break;
  case 2: start = slack / 2; break;
  case 4: start = slack; break;
  default: break;
  }
}
void move(CtlObject &o, int l, int t, int r, int b) {
  o.pos = QPoint(l, t);
  if (!isGroup(o)) return;
  CtlObject &g = o;
  int x = l, y = t, W = r - l, H = b - t;
  if (g.kind == Kind::TabGroup) {
    // 0x12205599: pages are laid out inside the tab window, whose origin is (pos + margins - (5,27)); content x starts 2 px in.
    // The generic move rect is (2 - ml, 0) in tab-window coordinates; translated by the window origin that is (pos.x - 3, pos.y + mt - 27).
    x = g.pos.x() - 3; y = g.pos.y() + g.margins.top() - 27;
  }
  if (g.flags & Flag::Padding) { x += 2; y += 2; W -= 2; H -= 2; }
  if (g.hidden()) return;
  int start = 0, gap = 0;
  if (!g.vertical) {
    packDistribution(g.pack, W - g.margins.right() - g.margins.left(), g.content.width(), g.visibleChildren, start, gap);
    int cx = x + g.margins.left() + start; const int cy = y + g.margins.top();
    for (const auto &c : g.children) { if (c->hidden()) continue; const int off = alignOffset(c->align, H - g.margins.bottom() - g.margins.top(), c->size.height()); move(*c, cx, cy + off, cx + c->size.width(), cy + off + c->size.height()); cx += gap + c->size.width(); }
  } else {
    packDistribution(g.pack, H - g.margins.bottom() - g.margins.top(), g.content.height(), g.visibleChildren, start, gap);
    int cy = y + g.margins.top() + start; const int cx = x + g.margins.left();
    for (const auto &c : g.children) { if (c->hidden()) continue; const int off = alignOffset(c->align, W - g.margins.right() - g.margins.left(), c->size.width()); move(*c, cx + off, cy, cx + off + c->size.width(), cy + c->size.height()); cy += gap + c->size.height(); }
  }
}

QHash<quint32, QString> loadStrings() {
  QHash<quint32, QString> strings; QFile file(QStringLiteral(":/aim/strings.json"));
  if (file.open(QIODevice::ReadOnly)) { const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object(); for (auto it = object.begin(); it != object.end(); ++it) strings.insert(it.key().toUInt(), it.value().toString()); }
  return strings;
}
struct FontDescription { int height = 0; int weight = 400; bool italic = false, underline = false, strikeOut = false; QString face; bool valid = false; };
FontDescription fontDescription(quint32 fontId) {
  FontDescription d; if (!fontId) return d;
  QFile file(QStringLiteral(":/aim/fontdesc/%1").arg(fontId)); if (!file.open(QIODevice::ReadOnly)) return d;
  const QByteArray data = file.readAll(); const Reader r(data);
  d.height = qint16(r.u16(2)); // FONTDESC word 0 == 0 for every shipped font: lfHeight = (int16)word 1 (oscore 0x1218612e)
  d.weight = r.u16(10); d.italic = r.u16(12); d.underline = r.u16(14); d.strikeOut = r.u16(16);
  d.face = QString::fromLatin1(data.mid(0x1c).constData()); d.valid = true;
  return d;
}
#ifdef Q_OS_WIN
HFONT createFont(quint32 fontId) {
  const FontDescription d = fontDescription(fontId); if (!d.valid) return nullptr;
  LOGFONTW logical{}; logical.lfHeight = d.height; logical.lfWeight = d.weight; logical.lfItalic = d.italic; logical.lfUnderline = d.underline; logical.lfStrikeOut = d.strikeOut; logical.lfCharSet = DEFAULT_CHARSET;
  const std::wstring face = d.face.toStdWString(); wcsncpy_s(logical.lfFaceName, face.c_str(), _TRUNCATE);
  return CreateFontIndirectW(&logical);
}
#endif
}

bool CtlObject::shown() const { for (const CtlObject *o = this; o; o = o->parent) if (o->flags & (Flag::Hidden | Flag::NotShown)) return false; return true; }
CtlObject *CtlObject::find(quint32 objectId) {
  if (objectId && id == objectId) return this;
  for (const auto &child : children) if (CtlObject *found = child->find(objectId)) return found;
  return nullptr;
}

QFont CtlEnvironment::font(quint32 fontId) {
  const FontDescription d = fontDescription(fontId);
  if (!d.valid) return QFont(QStringLiteral("MS Sans Serif"), 8);
  QFont font(d.face); font.setWeight(QFont::Weight(qBound(1, d.weight / 10, 1000))); font.setItalic(d.italic); font.setUnderline(d.underline); font.setStrikeOut(d.strikeOut);
  // Negative LOGFONT heights are character heights, positive ones cell heights (approximated by the pixel size).
  font.setPixelSize(std::max(1, std::abs(d.height))); return font;
}
QSize CtlEnvironment::averageCharSize(quint32 fontId) const {
#ifdef Q_OS_WIN
  HDC dc = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr); HFONT font = createFont(fontId); HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
  TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
  if (old) SelectObject(dc, old); if (font) DeleteObject(font); DeleteDC(dc);
  return QSize(metrics.tmAveCharWidth, metrics.tmHeight + metrics.tmExternalLeading);
#else
  if (!fontId) return QSize(7, 16); // System font at 96 dpi
  const QFontMetrics metrics(font(fontId)); return QSize(metrics.averageCharWidth(), metrics.height() + metrics.leading());
#endif
}
QSize CtlEnvironment::textExtent(quint32 fontId, const QString &text, int wrapWidth, TextMode mode) const {
#ifdef Q_OS_WIN
  HDC dc = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr); HFONT font = createFont(fontId); HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
  RECT rect{0, 0, wrapWidth > 0 ? wrapWidth : 0, 0};
  const UINT format = mode == ButtonText ? DT_CALCRECT | DT_NOCLIP | DT_CENTER : mode == StaticWrapped ? DT_CALCRECT | DT_NOPREFIX | DT_WORDBREAK : DT_CALCRECT | DT_NOPREFIX; // 0x1220605c / 0x1220d9d4
  DrawTextW(dc, reinterpret_cast<LPCWSTR>(text.utf16()), int(text.size()), &rect, format);
  if (old) SelectObject(dc, old); if (font) DeleteObject(font); DeleteDC(dc);
  return QSize(rect.right - rect.left, rect.bottom - rect.top);
#else
  const QFontMetrics metrics(font(fontId));
  QString shown = text; if (mode == ButtonText) { shown.replace(QStringLiteral("&&"), QStringLiteral("\x01")); shown.remove(QLatin1Char('&')); shown.replace(QChar(1), QLatin1Char('&')); }
  if (mode != StaticWrapped || wrapWidth <= 0) return QSize(metrics.horizontalAdvance(shown), metrics.height());
  return metrics.boundingRect(QRect(0, 0, wrapWidth, 100000), Qt::TextWordWrap, shown).size();
#endif
}
QSize CtlEnvironment::artSize(quint32 artId) const {
  static QHash<quint32, QSize> cache; auto it = cache.find(artId); if (it != cache.end()) return *it;
  const QSize size = QImageReader(QStringLiteral(":/aim/art/%1").arg(artId), "bmp").size();
  cache.insert(artId, size.isValid() ? size : QSize(0, 0)); return cache.value(artId);
}
int CtlEnvironment::verticalScrollWidth() const {
#ifdef Q_OS_WIN
  return GetSystemMetrics(SM_CXVSCROLL);
#else
  return 17;
#endif
}
QString CtlEnvironment::string(quint32 id) const { static const QHash<quint32, QString> strings = loadStrings(); return id ? strings.value(id) : QString(); }
const CtlEnvironment &aimEnvironment() { static const CtlEnvironment environment; return environment; }

std::shared_ptr<CtlObject> parseCtlGroup(const QByteArray &data) {
  std::shared_ptr<CtlObject> root; CtlObject *current = nullptr;
  QSize pendingHint; QMargins pendingMargins; quint16 pendingAlign = 0; bool hasMargins = false, hasAlign = false;
  auto transfer = [&](CtlObject &o) { // 0x12204536: records 4/5/6 apply to the next created object
    if (pendingHint.width()) { o.hint.setWidth(pendingHint.width()); pendingHint.setWidth(0); }
    if (pendingHint.height()) { o.hint.setHeight(pendingHint.height()); pendingHint.setHeight(0); }
    if (hasMargins) { o.margins = pendingMargins; hasMargins = false; }
    if (hasAlign) { o.align = pendingAlign; hasAlign = false; }
  };
  auto attach = [&](std::shared_ptr<CtlObject> o, bool opensGroup) {
    if (o->margins.isNull()) o->margins = defaultMargins(o->kind);
    transfer(*o);
    if (current) { o->parent = current; current->children.append(o); } else root = o;
    if (opensGroup) current = o.get();
  };
  qsizetype offset = 0;
  while (offset + 4 <= data.size()) {
    const Reader header(data); const quint16 type = header.u16(offset), length = header.u16(offset + 2);
    const QByteArray payload = data.mid(offset + 4, length); const Reader p(payload);
    offset += 4 + length;
    auto o = std::make_shared<CtlObject>();
    switch (type) {
    case 1: o->kind = Kind::Group; o->id = p.u32(0); o->vertical = p.u16(4); o->pack = p.u16(6); o->titleId = p.u32(8); o->flags = p.u32(12) | Flag::IsGroup; attach(o, true); break;
    case 2: if (!current || !current->parent) offset = data.size(); else current = current->parent; break;
    case 3: if (!current) { offset = data.size(); break; } o->kind = controlKind(p.u32(0)); o->id = p.u32(4); o->base = QSize(p.u16(8), p.u16(10)); o->fontId = p.u32(12); o->textId = p.u32(16); o->tooltipId = p.u32(20); o->style = p.u32(24); o->flags = p.u32(28); attach(o, false); break;
    case 4: pendingHint = QSize(p.u16(0), p.u16(2)); break;
    case 5: pendingMargins = QMargins(p.u16(0), p.u16(2), p.u16(4), p.u16(6)); hasMargins = true; break;
    case 6: pendingAlign = p.u16(0); hasAlign = true; break;
    case 0x402: if (!current) break; o->kind = Kind::Separator; o->id = p.u32(0); o->base = QSize(p.u16(4), 0); o->flags = p.u32(6); attach(o, false); break;
    case 0x403: if (!current) break; o->kind = Kind::RateMeter; o->id = p.u32(0); o->style = p.u32(0x18); o->flags = p.u32(0x1c); attach(o, false); break;
    case 0x404: if (!current) break; o->kind = Kind::Static; o->id = p.u32(0); o->base = QSize(p.u16(8), p.u16(10)); o->fontId = p.u32(12); o->textId = p.u32(16); o->tooltipId = p.u32(20); o->style = p.u32(24); o->flags = p.u32(28); attach(o, false); break;
    case 0x405: if (!current) break; o->kind = Kind::TabGroup; o->id = p.u32(0); o->vertical = false; o->pack = 2; o->flags = p.u32(12) | Flag::IsGroup; attach(o, true); break;
    case 0x406: if (!current) break; o->kind = Kind::PersistentCombo; o->id = p.u32(0); o->profileKeyId = p.u32(6); o->style = p.u32(22); o->flags = p.u32(26) & ~quint32(Flag::Padding); attach(o, false); break; // 0x20 cleared when the window is created
    case 0x407: if (!current) break; o->kind = Kind::ArtButton; o->id = p.u32(0); o->buttonType = p.u16(4); o->placement = p.u16(6); o->gap = p.u16(8); for (int i = 0; i < 3; ++i) o->art[i] = p.u32(10 + i * 4); o->fontId = p.u32(22); o->textId = p.u32(26); o->tooltipId = p.u32(30); o->style = p.u32(34); o->flags = p.u32(38); attach(o, false); break;
    case 0x801: if (!current) break; o->kind = Kind::TabBody; o->id = p.u32(0); o->base = QSize(p.u16(4), p.u16(6)); o->style = p.u32(12); o->flags = p.u32(16); attach(o, false); break;
    case 0x802: { if (!current) break; o->kind = Kind::Ate; o->id = p.u32(0); o->base = QSize(p.u16(4), p.u16(6)); const quint32 flags = p.u32(16); o->flags = flags & ~quint32(Flag::Padding); o->style = p.u32(12) | ((flags & Flag::Padding) ? 0x8000 : 0); attach(o, false); break; }
    default: break; // record types nobody claims are skipped
    }
  }
  if (root) { std::function<void(CtlObject &)> pages = [&](CtlObject &o) { if (o.kind == Kind::TabGroup && !o.children.isEmpty()) ctlSetPage(o, 0); for (const auto &child : o.children) pages(*child); }; pages(*root); }
  return root;
}
std::shared_ptr<CtlObject> loadCtlGroup(int resourceId) { QFile file(QStringLiteral(":/aim/ctlgroups/%1").arg(resourceId)); return file.open(QIODevice::ReadOnly) ? parseCtlGroup(file.readAll()) : nullptr; }

void ctlShowControl(CtlObject &root, quint32 id, bool show) { if (CtlObject *o = root.find(id)) { if (show) o->flags &= ~quint32(Flag::NotShown | Flag::Hidden); else o->flags |= Flag::NotShown | Flag::Hidden; } }
void ctlSetPage(CtlObject &tabGroup, quint32 pageId) {
  bool first = true;
  for (const auto &page : tabGroup.children) { const bool show = pageId ? page->id == pageId : first; first = false; if (show) page->flags &= ~quint32(Flag::Hidden); else page->flags |= Flag::Hidden; }
}
QSize ctlIdealSize(CtlObject &root, const CtlEnvironment &environment) { idealSize(root, environment); return root.ideal; }
void ctlMove(CtlObject &root, const QRect &rect, const CtlEnvironment &environment) {
  idealSize(root, environment);
  arrange(root, rect.width(), rect.height());
  move(root, rect.left(), rect.top(), rect.left() + rect.width(), rect.top() + rect.height());
}
