#include "gdi_text.h"
#include "ctl_group.h"
#include <QFile>
#include <QFontMetrics>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QtEndian>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

GdiFont GdiFont::fromFontDesc(quint32 fontId) {
  GdiFont font; if (!fontId) return font;
  QFile file(QStringLiteral(":/aim/fontdesc/%1").arg(fontId)); if (!file.open(QIODevice::ReadOnly)) return font;
  const QByteArray data = file.readAll(); if (data.size() < 0x1d) return font;
  auto word = [&data](int index) { return qFromLittleEndian<quint16>(data.constData() + index * 2); };
  font.height = qint16(word(1)); font.weight = word(5); font.italic = word(6); font.underline = word(7);
  font.face = QString::fromLatin1(data.mid(0x1c).constData());
  return font;
}

#ifdef Q_OS_WIN
namespace {
HFONT handle(const GdiFont &font) {
  static QHash<QString, HFONT> fonts;
  const QString key = QStringLiteral("%1|%2|%3|%4|%5").arg(font.face).arg(font.height).arg(font.weight).arg(font.italic).arg(font.underline);
  auto it = fonts.find(key); if (it != fonts.end()) return *it;
  LOGFONTW logical{}; logical.lfHeight = font.height; logical.lfWeight = font.weight; logical.lfItalic = font.italic; logical.lfUnderline = font.underline; logical.lfCharSet = DEFAULT_CHARSET;
  const std::wstring face = font.face.toStdWString(); wcsncpy_s(logical.lfFaceName, face.c_str(), _TRUNCATE);
  return fonts.insert(key, CreateFontIndirectW(&logical)).value();
}
}
#endif

QSize gdiTextSize(const GdiFont &font, const QString &text, unsigned flags, int wrapWidth) {
#ifdef Q_OS_WIN
  HDC dc = CreateCompatibleDC(nullptr); HGDIOBJ old = SelectObject(dc, handle(font));
  RECT rect{0, 0, wrapWidth, 0}; DrawTextW(dc, reinterpret_cast<LPCWSTR>(text.utf16()), int(text.size()), &rect, flags | DT_CALCRECT);
  SelectObject(dc, old); DeleteDC(dc);
  return QSize(rect.right - rect.left, rect.bottom - rect.top);
#else
  QFont qfont(font.face); qfont.setPixelSize(std::abs(font.height)); qfont.setWeight(QFont::Weight(qBound(1, font.weight / 10, 1000)));
  const QFontMetrics metrics(qfont); return (flags & GdiWordBreak) && wrapWidth > 0 ? metrics.boundingRect(QRect(0, 0, wrapWidth, 100000), Qt::TextWordWrap, text).size() : QSize(metrics.horizontalAdvance(text), metrics.height());
#endif
}

void drawGdiText(QPainter &painter, const QRect &rect, const QString &text, const GdiFont &font, const QColor &color, const QColor &background, unsigned flags) {
  if (rect.width() <= 0 || rect.height() <= 0 || text.isEmpty()) return;
#ifdef Q_OS_WIN
  BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = rect.width(); info.bmiHeader.biHeight = -rect.height(); info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
  void *bits = nullptr; HDC dc = CreateCompatibleDC(nullptr); HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap || !bits) { if (bitmap) DeleteObject(bitmap); DeleteDC(dc); return; }
  HGDIOBJ oldBitmap = SelectObject(dc, bitmap), oldFont = SelectObject(dc, handle(font));
  RECT area{0, 0, rect.width(), rect.height()}; HBRUSH brush = CreateSolidBrush(RGB(background.red(), background.green(), background.blue())); FillRect(dc, &area, brush); DeleteObject(brush);
  SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(color.red(), color.green(), color.blue()));
  DrawTextW(dc, reinterpret_cast<LPCWSTR>(text.utf16()), int(text.size()), &area, flags);
  GdiFlush();
  QImage image(static_cast<const uchar *>(bits), rect.width(), rect.height(), rect.width() * 4, QImage::Format_RGB32);
  painter.drawImage(rect.topLeft(), image);
  SelectObject(dc, oldFont); SelectObject(dc, oldBitmap); DeleteObject(bitmap); DeleteDC(dc);
#else
  QFont qfont(font.face); qfont.setPixelSize(std::abs(font.height)); qfont.setWeight(QFont::Weight(qBound(1, font.weight / 10, 1000))); qfont.setItalic(font.italic); qfont.setUnderline(font.underline);
  painter.fillRect(rect, background); painter.setFont(qfont); painter.setPen(color);
  int alignment = (flags & GdiCenter) ? Qt::AlignHCenter : (flags & GdiRight) ? Qt::AlignRight : Qt::AlignLeft; alignment |= (flags & GdiVCenter) ? Qt::AlignVCenter : Qt::AlignTop; if (flags & GdiWordBreak) alignment |= Qt::TextWordWrap;
  QString shown = text; if (!(flags & GdiNoPrefix)) { shown.replace(QStringLiteral("&&"), QStringLiteral("\x01")); shown.remove(QLatin1Char('&')); shown.replace(QChar(1), QLatin1Char('&')); }
  painter.drawText(rect, alignment, shown);
#endif
}
