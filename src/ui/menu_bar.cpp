#include "menu_bar.h"
#include <QFontMetrics>
#include <QHash>
#include <QPainter>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

PaintedMenuBar::PaintedMenuBar(int resourceId) { if (resourceId) items = loadMenuResource(resourceId); }
QFont PaintedMenuBar::font() {
#ifdef Q_OS_WIN
  // The font a native menu bar uses (SPI_GETNONCLIENTMETRICS lfMenuFont).
  NONCLIENTMETRICSW metrics{}; metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) { QFont font(QString::fromWCharArray(metrics.lfMenuFont.lfFaceName)); font.setPixelSize(std::abs(metrics.lfMenuFont.lfHeight)); font.setWeight(QFont::Weight(qBound(1, int(metrics.lfMenuFont.lfWeight) / 10, 1000))); return font; }
#endif
  return QFont(QStringLiteral("MS Sans Serif"), 8);
}
QVector<QRect> PaintedMenuBar::layout(const QList<MenuItem> &items, int left, int top, int width) {
#ifdef Q_OS_WIN
  // Let Windows lay the bar out: build the real menu on a hidden window of the same client width and read back the
  // item rectangles (GetMenuBarInfo), so spacing and wrapping are exactly those of a native menu bar.
  static QHash<QString, QVector<QRect>> cache; QString key = QString::number(width); for (const MenuItem &item : items) key += QLatin1Char('|') + item.text;
  if (!cache.contains(key)) {
    QVector<QRect> measured; HMENU bar = CreateMenu();
    for (const MenuItem &item : items) AppendMenuW(bar, MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(CreatePopupMenu()), reinterpret_cast<LPCWSTR>(item.text.utf16()));
    RECT frame{0, 0, width, 100}; AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND window = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, -32000, -32000, frame.right - frame.left, frame.bottom - frame.top, nullptr, bar, GetModuleHandleW(nullptr), nullptr);
    if (window) {
      MENUBARINFO barInfo{}; barInfo.cbSize = sizeof(barInfo);
      if (GetMenuBarInfo(window, OBJID_MENU, 0, &barInfo)) for (int i = 0; i < items.size(); ++i) { MENUBARINFO itemInfo{}; itemInfo.cbSize = sizeof(itemInfo); if (!GetMenuBarInfo(window, OBJID_MENU, i + 1, &itemInfo)) { measured.clear(); break; } measured.append(QRect(itemInfo.rcBar.left - barInfo.rcBar.left, itemInfo.rcBar.top - barInfo.rcBar.top, itemInfo.rcBar.right - itemInfo.rcBar.left, itemInfo.rcBar.bottom - itemInfo.rcBar.top)); }
      DestroyWindow(window); // destroys the menu too
    } else DestroyMenu(bar);
    cache.insert(key, measured);
  }
  const QVector<QRect> &native = cache[key];
  if (native.size() == items.size() && !native.isEmpty()) { QVector<QRect> rects; for (const QRect &r : native) rects.append(r.translated(left, top)); return rects; }
#endif
  QVector<QRect> rects; const QFontMetrics metrics(font()); int x = left, y = top;
  for (const MenuItem &item : items) {
    const int itemWidth = metrics.horizontalAdvance(item.label()) + Padding * 2;
    if (x > left && x + itemWidth > left + width) { x = left; y += RowHeight; }
    rects.append(QRect(x, y, itemWidth, RowHeight)); x += itemWidth;
  }
  return rects;
}
int PaintedMenuBar::rows(const QVector<QRect> &rects, int top) { return rects.isEmpty() ? 0 : (rects.last().bottom() + 1 - top + RowHeight / 2) / RowHeight; }
void PaintedMenuBar::paint(QPainter &painter, const QVector<QRect> &rects, int hovered, int open) const {
  for (int i = 0; i < rects.size() && i < items.size(); ++i) {
    if (i == open || i == hovered) { painter.fillRect(rects[i], QColor(229, 243, 255)); painter.setPen(QColor(204, 232, 255)); painter.drawRect(rects[i].adjusted(0, 0, -1, -1)); }
    painter.setFont(font()); painter.setPen(QColor(20, 20, 20)); painter.drawText(rects[i].adjusted(Padding, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, items[i].label());
  }
}
int PaintedMenuBar::hit(const QVector<QRect> &rects, const QPoint &point) { for (int i = 0; i < rects.size(); ++i) if (rects[i].contains(point)) return i; return -1; }
