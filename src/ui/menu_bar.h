#pragma once
#include "menu_template.h"
#include <QFont>
#include <QRect>
#include <QVector>
#include <algorithm>

class QPainter;

// A menu bar painted from an original RT_MENU template. Items flow left to right and wrap onto further rows the way
// a native Win32 menu bar does when the window is narrow.
class PaintedMenuBar {
public:
  static constexpr int RowHeight = 20, Padding = 7; // native menu bar item padding measured from the reference screenshots
  explicit PaintedMenuBar(int resourceId = 0);
  QList<MenuItem> items;
  QVector<QRect> layout(int left, int top, int width) const { return layout(items, left, top, width); }
  static QVector<QRect> layout(const QList<MenuItem> &items, int left, int top, int width);
  static int bottom(const QVector<QRect> &rects, int top) { int value = top; for (const QRect &r : rects) value = std::max(value, r.bottom() + 1); return value; }
  static int rows(const QVector<QRect> &rects, int top);
  void paint(QPainter &painter, const QVector<QRect> &rects, int hovered, int open) const;
  static int hit(const QVector<QRect> &rects, const QPoint &point);
  static QFont font();
};
