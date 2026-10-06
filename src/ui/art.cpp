#include "art.h"
#include <QHash>
#include <QPainter>

namespace art {
QImage image(quint32 artId) {
  static QHash<quint32, QImage> cache; auto it = cache.find(artId); if (it != cache.end()) return *it;
  QImage result = QImage(QStringLiteral(":/aim/art/%1").arg(artId)).convertToFormat(QImage::Format_ARGB32);
  if (!result.isNull()) { const QRgb key = result.pixel(0, 0) & 0x00ffffffu; for (int y = 0; y < result.height(); ++y) { auto *line = reinterpret_cast<QRgb *>(result.scanLine(y)); for (int x = 0; x < result.width(); ++x) if ((line[x] & 0x00ffffffu) == key) line[x] = 0; } }
  cache.insert(artId, result); return result;
}
QImage disabled(const QImage &source) {
  QImage result = source.convertToFormat(QImage::Format_ARGB32);
  for (int y = 0; y < result.height(); ++y) { auto *line = reinterpret_cast<QRgb *>(result.scanLine(y)); for (int x = 0; x < result.width(); ++x) { const int value = 128 + qGray(line[x]) / 2; line[x] = qRgba(value, value, value, qAlpha(line[x])); } }
  return result;
}
void drawEtched(QPainter &p, const QRect &r) {
  // 0x12204c94: shadow pen then highlight pen, 1 px each.
  const int L = r.left(), T = r.top(), R = r.left() + r.width(), B = r.top() + r.height();
  p.setPen(Shadow); p.drawLine(L, B, L, T); p.drawLine(L, T, R - 1, T); p.drawLine(R - 1, T, R - 1, B - 1); p.drawLine(R - 1, B - 1, L, B - 1);
  p.setPen(Highlight); p.drawLine(L + 1, B - 2, L + 1, T + 1); p.drawLine(L + 1, T + 1, R - 1, T + 1); p.drawLine(R, T, R, B); p.drawLine(R, B, L - 1, B);
}
void drawSunken(QPainter &p, const QRect &r) {
  p.setPen(Shadow); p.drawLine(r.left(), r.bottom(), r.left(), r.top()); p.drawLine(r.left(), r.top(), r.right(), r.top());
  p.setPen(Highlight); p.drawLine(r.right(), r.top(), r.right(), r.bottom()); p.drawLine(r.right(), r.bottom(), r.left(), r.bottom());
  p.setPen(DarkShadow); p.drawLine(r.left() + 1, r.bottom() - 1, r.left() + 1, r.top() + 1); p.drawLine(r.left() + 1, r.top() + 1, r.right() - 1, r.top() + 1);
  p.setPen(Face); p.drawLine(r.right() - 1, r.top() + 1, r.right() - 1, r.bottom() - 1); p.drawLine(r.right() - 1, r.bottom() - 1, r.left() + 1, r.bottom() - 1);
}
void drawEtchedLine(QPainter &p, const QRect &r) {
  if (r.height() > 2) { p.setPen(Shadow); p.drawLine(r.left(), r.top(), r.left(), r.bottom()); p.setPen(Highlight); p.drawLine(r.left() + 1, r.top(), r.left() + 1, r.bottom()); }
  else { p.setPen(Shadow); p.drawLine(r.left(), r.top(), r.right(), r.top()); p.setPen(Highlight); p.drawLine(r.left(), r.top() + 1, r.right(), r.top() + 1); }
}
}
