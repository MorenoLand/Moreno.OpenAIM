#include "ate_toolbar.h"
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QPainter>
#include <QWindow>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
const QColor Face(240, 240, 240), Shadow(160, 160, 160), Highlight(255, 255, 255);
constexpr int CellWidth = 17, ButtonWidth = 23, ButtonHeight = 17, SeparatorWidth = 8, Spacing = 1, Inset = 8;
QImage keyed(const QString &path, QRgb key) {
  QImage image = QImage(path).convertToFormat(QImage::Format_ARGB32);
  for (int y = 0; y < image.height(); ++y) { auto *line = reinterpret_cast<QRgb *>(image.scanLine(y)); for (int x = 0; x < image.width(); ++x) if ((line[x] & 0x00ffffffu) == key) line[x] = 0; }
  return image;
}
}

AteToolbar::AteToolbar(Set set) {
  // Button tables at wndutils 0x1229ccf4..0x1229cf4c: {command, tooltip string}; cell = index in the strip, -1 = separator.
  auto add = [this](int command, quint32 tooltip, int cell) { Item item; item.command = command; item.tooltip = tooltip; item.cell = cell; items_.append(item); };
  auto separator = [this] { items_.append(Item{}); };
  int strip = 1014;
  if (set == Set::Chat) {
    strip = 1015; add(TextColor, 285, 0); separator(); add(Bold, 290, 1); add(Italic, 291, 2); add(Underline, 292, 3); separator(); add(Link, 293, 4); add(Smiley, 72, 5);
  } else {
    strip = set == Set::Away ? 1001 : 1014;
    add(TextColor, 285, 0); add(BackgroundColor, 286, 1); separator(); add(Smaller, 287, 2); add(NormalSize, 288, 3); add(Larger, 289, 4); separator();
    add(Bold, 290, 5); add(Italic, 291, 6); add(Underline, 292, 7); separator(); add(Link, 293, 8);
    if (set == Set::InstantMessage) { add(ConnectImage, 1184, 9); add(Greeting, 1802, 10); add(Smiley, 72, 11); } // VALUERES 135 = 1: greeting cell present
    else add(Smiley, 72, 9);
  }
  strip_ = keyed(QStringLiteral(":/aim/art/%1").arg(strip), 0x00c0c0c0u); // LoadBitmap(id, 0, 0xC0C0C0)
}

void AteToolbar::layout(const QRect &bar) {
  bar_ = bar;
  int width = 0; for (const Item &item : items_) width += item.separator() ? SeparatorWidth : ButtonWidth; width += Spacing * int(items_.size() - 1);
  int x = bar.left() + Inset + ((bar.width() - 2 * Inset) - width) / 2; // align 0x20: centred inside an 8 px inset
  for (Item &item : items_) { const int w = item.separator() ? SeparatorWidth : ButtonWidth; item.rect = QRect(x, bar.top() + 2, w, ButtonHeight); x += w + Spacing; }
}

void AteToolbar::paint(QPainter &p, int hovered, int pressed, const QList<int> &checkedCommands) const {
  const QRect r = bar_;
  p.fillRect(r, Face);
  for (int i = 0; i < items_.size(); ++i) {
    const Item &item = items_[i]; const QRect b = item.rect;
    if (item.separator()) { p.setPen(Shadow); p.drawLine(b.left() + 3, b.top() + 1, b.left() + 3, b.bottom() - 1); p.setPen(Highlight); p.drawLine(b.left() + 4, b.top() + 1, b.left() + 4, b.bottom() - 1); continue; }
    const bool sunken = i == pressed || checkedCommands.contains(item.command), raised = !sunken && i == hovered;
    if (sunken || raised) {
      // DrawButton 0x1228b96b: hot = raised 1 px bevel plus inner shadow; pressed/checked = sunken, image +1,+1.
      p.setPen(sunken ? Shadow : Highlight); p.drawLine(b.left(), b.bottom(), b.left(), b.top()); p.drawLine(b.left(), b.top(), b.right(), b.top());
      p.setPen(sunken ? Highlight : Shadow); p.drawLine(b.right(), b.top(), b.right(), b.bottom()); p.drawLine(b.right(), b.bottom(), b.left(), b.bottom());
      if (raised) { p.setPen(Shadow); p.drawLine(b.right() - 1, b.top() + 1, b.right() - 1, b.bottom() - 1); p.drawLine(b.left() + 1, b.bottom() - 1, b.right() - 1, b.bottom() - 1); }
    }
    const QPoint at = b.topLeft() + QPoint(2, 3) + (sunken ? QPoint(1, 1) : QPoint());
    p.drawImage(at, strip_, QRect(item.cell * CellWidth, 0, CellWidth, strip_.height()));
  }
  // CCtrlBar border: 2 px etched frame.
  p.setPen(Highlight); p.drawRect(r.adjusted(1, 1, -1, -1));
  p.setPen(Shadow); p.drawRect(r.adjusted(0, 0, -2, -2));
  p.setPen(Highlight); p.drawPoint(r.left(), r.bottom()); p.drawPoint(r.right(), r.top());
}

int AteToolbar::hit(const QPoint &point) const { for (int i = 0; i < items_.size(); ++i) if (!items_[i].separator() && items_[i].rect.contains(point)) return i; return -1; }

QString AteToolbar::smileyCode(int glyph) {
  // RT_STRING ids 340,342,341,343,37,38,39,48,41..47,40 (leading token), glyph order of strip 1008.
  static const char *codes[16] = {":-)", ";-)", ":-(", ":-P", "=-O", ":-*", ">:o", ":-D", ":-$", ":-!", ":-[", "O:-)", ":-\\", ":'(", ":-X", "8-)"};
  return glyph >= 0 && glyph < 16 ? QString::fromLatin1(codes[glyph]) : QString();
}
QImage AteToolbar::smileyImage(int glyph) {
  static QList<QImage> glyphs = [] {
    QList<QImage> result; const QImage strip = QImage(QStringLiteral(":/aim/art/1008")).convertToFormat(QImage::Format_ARGB32), mask = QImage(QStringLiteral(":/aim/art/1009")).convertToFormat(QImage::Format_ARGB32);
    for (int g = 0; g < 16; ++g) { QImage cell = strip.copy(g * 19, 0, 19, 19); for (int y = 0; y < 19; ++y) for (int x = 0; x < 19; ++x) if (!mask.isNull() && (mask.pixel(g * 19 + x, y) & 0x00ffffffu) == 0x00ffffffu) cell.setPixel(x, y, 0); result.append(cell); } // mask: white = transparent
    return result;
  }();
  return glyph >= 0 && glyph < glyphs.size() ? glyphs[glyph] : QImage();
}

#ifdef Q_OS_WIN
namespace {
class SmileyDraw final : public QAbstractNativeEventFilter {
public:
  explicit SmileyDraw(HWND owner) : owner_(owner) { QCoreApplication::instance()->installNativeEventFilter(this); }
  ~SmileyDraw() override { QCoreApplication::instance()->removeNativeEventFilter(this); }
  bool nativeEventFilter(const QByteArray &, void *message, qintptr *result) override {
    MSG *msg = static_cast<MSG *>(message); if (msg->hwnd != owner_) return false;
    if (msg->message == WM_MEASUREITEM) { auto *m = reinterpret_cast<MEASUREITEMSTRUCT *>(msg->lParam); if (m->CtlType != ODT_MENU) return false; m->itemWidth = 17; m->itemHeight = 23; if (result) *result = TRUE; return true; } // 0x1201aa99
    if (msg->message == WM_DRAWITEM) {
      auto *d = reinterpret_cast<DRAWITEMSTRUCT *>(msg->lParam); if (d->CtlType != ODT_MENU) return false;
      FillRect(d->hDC, &d->rcItem, GetSysColorBrush((d->itemState & ODS_SELECTED) ? COLOR_HIGHLIGHT : COLOR_MENU));
      const QImage glyph = AteToolbar::smileyImage(int(d->itemData)).convertToFormat(QImage::Format_ARGB32_Premultiplied);
      BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = glyph.width(); info.bmiHeader.biHeight = -glyph.height(); info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
      void *bits = nullptr; HDC memory = CreateCompatibleDC(d->hDC); HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
      if (bitmap && bits) { memcpy(bits, glyph.constBits(), size_t(glyph.sizeInBytes())); HGDIOBJ old = SelectObject(memory, bitmap); BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA}; AlphaBlend(d->hDC, d->rcItem.left + 4, d->rcItem.top + 2, glyph.width(), glyph.height(), memory, 0, 0, glyph.width(), glyph.height(), blend); SelectObject(memory, old); }
      if (bitmap) DeleteObject(bitmap); DeleteDC(memory);
      if (result) *result = TRUE; return true;
    }
    return false;
  }
private:
  HWND owner_;
};
}
#endif

int AteToolbar::pickSmiley(QWindow *owner, const QPoint &globalPosition) {
#ifdef Q_OS_WIN
  if (!owner) return -1;
  // ate32 0x1201af40: owner-draw popup, MF_MENUBREAK every 4th item -> 4 columns filled top to bottom.
  static const int order[16] = {0, 2, 1, 3, 4, 5, 6, 15, 8, 9, 10, 11, 12, 13, 14, 7};
  HMENU menu = CreatePopupMenu();
  for (int i = 0; i < 16; ++i) AppendMenuW(menu, MF_OWNERDRAW | (i && i % 4 == 0 ? MF_MENUBREAK : 0), UINT_PTR(0x32 + order[i]), reinterpret_cast<LPCWSTR>(quintptr(order[i])));
  HWND window = reinterpret_cast<HWND>(owner->winId()); SmileyDraw draw(window);
  const int id = int(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_CENTERALIGN, globalPosition.x(), globalPosition.y(), 0, window, nullptr));
  DestroyMenu(menu);
  return id >= 0x32 && id < 0x32 + 16 ? id - 0x32 : -1;
#else
  Q_UNUSED(owner); Q_UNUSED(globalPosition); return -1;
#endif
}
