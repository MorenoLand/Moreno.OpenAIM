#pragma once
#include <QColor>
#include <QRect>
#include <QString>

class QPainter;

// Text drawn the way the original client drew it: through GDI with the original LOGFONT (raster fonts such as
// "MS Sans Serif" and "Small Fonts" cannot be rendered by Qt). Other platforms fall back to QPainter text.
struct GdiFont {
  QString face = QStringLiteral("MS Sans Serif");
  int height = -11; // LOGFONT lfHeight
  int weight = 400;
  bool italic = false, underline = false;
  static GdiFont dialog() { return {}; }                    // MS Sans Serif 8 pt, the dialog template font
  static GdiFont fromFontDesc(quint32 fontId);              // AimRes FONTDESC
  GdiFont bold() const { GdiFont font = *this; font.weight = 700; return font; }
};
enum GdiTextFlag : unsigned { GdiLeft = 0x0, GdiCenter = 0x1, GdiRight = 0x2, GdiVCenter = 0x4, GdiWordBreak = 0x10, GdiSingleLine = 0x20, GdiNoPrefix = 0x800, GdiEndEllipsis = 0x8000, GdiHidePrefix = 0x100000 };
QSize gdiTextSize(const GdiFont &font, const QString &text, unsigned flags = GdiSingleLine, int wrapWidth = 0);
// Draws onto a solid background colour (GDI text has no alpha).
void drawGdiText(QPainter &painter, const QRect &rect, const QString &text, const GdiFont &font, const QColor &color, const QColor &background, unsigned flags = GdiSingleLine);
