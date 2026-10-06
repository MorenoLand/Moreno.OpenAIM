#pragma once
#include <QColor>
#include <QImage>
#include <QRect>

class QPainter;

// Shared drawing of original AimRes artwork and classic 3-D decorations.
namespace art {
inline const QColor Face(240, 240, 240), Shadow(160, 160, 160), Highlight(255, 255, 255), DarkShadow(100, 100, 100);
QImage image(quint32 artId);                 // RT_BITMAP with its colour key (top-left pixel) made transparent
QImage disabled(const QImage &source);
void drawEtched(QPainter &painter, const QRect &rect);   // CtlGroupPaintBackground frame for 0x20 groups
void drawSunken(QPainter &painter, const QRect &rect);   // WS_EX_CLIENTEDGE style 2 px sunken border
void drawEtchedLine(QPainter &painter, const QRect &rect); // _Oscar_Separator: 2 px etched line
}
