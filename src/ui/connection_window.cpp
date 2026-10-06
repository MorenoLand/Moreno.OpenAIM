#include "connection_window.h"
#include <QFontMetrics>
#include <QKeyEvent>
#include <utility>

ConnectionWindow::ConnectionWindow(const QString &host, quint16 port, QWindow *parent) : WindowBase(QStringLiteral("Connection"), QSize(340, 180), parent), host_(host), port_(port ? QString::number(port) : QString()) { setCaptionButtons(false, false, true); }
void ConnectionWindow::setSaveHandler(SaveHandler handler) { saveHandler_ = std::move(handler); }
void ConnectionWindow::paintContent(QPainter &p) {
  QFont font(QStringLiteral("MS Sans Serif"), 8);
  drawText(p, QPoint(16, 48), QStringLiteral("OSCAR server"), QColor(20, 20, 20), font);
  drawText(p, QPoint(18, 67), QStringLiteral("Host:"), QColor(20, 20, 20), font);
  drawText(p, QPoint(244, 67), QStringLiteral("Port:"), QColor(20, 20, 20), font);
  const QRect hostRect(18, 72, 213, 22), portRect(244, 72, 77, 22);
  for (int i = 0; i < 2; ++i) {
    const QRect r = i == 0 ? hostRect : portRect;
    p.fillRect(r, Qt::white);
    p.setPen(QColor(120, 120, 120)); p.drawLine(r.left(), r.top(), r.right(), r.top()); p.drawLine(r.left(), r.top(), r.left(), r.bottom());
    p.setPen(QColor(255, 255, 255)); p.drawLine(r.left(), r.bottom(), r.right(), r.bottom()); p.drawLine(r.right(), r.top(), r.right(), r.bottom());
    const QString value = i == 0 ? host_ : port_;
    drawText(p, QPoint(r.left() + 3, r.top() + 15), value, QColor(15, 15, 15), font);
    if (activeField_ == i) {
      const int x = r.left() + 3 + QFontMetrics(font).horizontalAdvance(value);
      p.setPen(QColor(20, 20, 20)); p.drawLine(x, r.top() + 3, x, r.bottom() - 3);
    }
  }
  if (!status_.isEmpty()) drawText(p, QPoint(18, 119), status_, QColor(170, 0, 0), font);
  p.setPen(QColor(130, 130, 130)); p.setBrush(QColor(245, 245, 245)); p.drawRect(QRect(178, 137, 67, 25)); p.drawRect(QRect(251, 137, 67, 25));
  drawText(p, QPoint(196, 153), QStringLiteral("OK"), QColor(20, 20, 20), font);
  drawText(p, QPoint(264, 153), QStringLiteral("Cancel"), QColor(20, 20, 20), font);
}
void ConnectionWindow::contentMousePress(const QPoint &point, Qt::MouseButton button) {
  if (button != Qt::LeftButton) return;
  if (QRect(18, 72, 213, 22).contains(point)) activeField_ = 0;
  else if (QRect(244, 72, 77, 22).contains(point)) activeField_ = 1;
  else if (QRect(178, 137, 67, 25).contains(point)) { save(); return; }
  else if (QRect(251, 137, 67, 25).contains(point)) { close(); return; }
  renderNow();
}
void ConnectionWindow::contentKeyPress(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape) { close(); return; }
  if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) { save(); return; }
  QString &value = activeField_ == 0 ? host_ : port_;
  if (event->key() == Qt::Key_Tab) { activeField_ = 1 - activeField_; renderNow(); return; }
  if (event->key() == Qt::Key_Backspace) value.chop(1);
  else if (!event->text().isEmpty() && event->text().at(0).isPrint()) value.append(event->text());
  renderNow();
}
void ConnectionWindow::save() {
  bool ok = false;
  const uint port = port_.toUInt(&ok);
  if (host_.trimmed().isEmpty() || !ok || port == 0 || port > 65535) { status_ = QStringLiteral("Enter a host and valid port."); renderNow(); return; }
  if (saveHandler_) saveHandler_(host_.trimmed(), quint16(port));
  close();
}
