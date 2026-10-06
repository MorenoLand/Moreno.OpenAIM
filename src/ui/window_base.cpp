#include "window_base.h"
#include <QEvent>
#include <QIcon>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QFile>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

WindowBase::WindowBase(const QString &title, const QSize &size, QWindow *parent) : QWindow(), backingStore_(this), icon_(QIcon(QStringLiteral(":/aim/window-icon.ico")).pixmap(16,16).toImage()), title_(title), canvasSize_(size) {
  setSurfaceType(QSurface::RasterSurface);
#ifdef Q_OS_WIN
  setFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint | Qt::MSWindowsFixedSizeDialogHint);
#else
  setFlags(Qt::Window | Qt::FramelessWindowHint);
#endif
  setTitle(title_);
  setIcon(QIcon(QStringLiteral(":/aim/window-icon.ico")));
  QSize clientSize = size;
#ifdef Q_OS_WIN
  clientSize -= QSize(2,TitleBarHeight+3);
#endif
  resize(clientSize);
  setMinimumSize(clientSize);
  setMaximumSize(clientSize);
  if (parent) { QObject::setParent(parent); setTransientParent(parent); setPosition(parent->position() + QPoint((parent->width() - width()) / 2, (parent->height() - height()) / 2)); }
#ifdef Q_OS_WIN
  create(); HWND window=reinterpret_cast<HWND>(winId()); LONG_PTR nativeStyle=GetWindowLongPtrW(window,GWL_STYLE)&~(WS_THICKFRAME|WS_MAXIMIZEBOX); SetWindowLongPtrW(window,GWL_STYLE,nativeStyle); SetWindowLongPtrW(window,GWL_EXSTYLE,WS_EX_WINDOWEDGE); RECT frame{0,0,clientSize.width(),clientSize.height()};AdjustWindowRectEx(&frame,DWORD(nativeStyle),FALSE,WS_EX_WINDOWEDGE);SetWindowPos(window,nullptr,0,0,frame.right-frame.left,frame.bottom-frame.top,SWP_NOMOVE|SWP_NOZORDER|SWP_FRAMECHANGED);int corners=2;DwmSetWindowAttribute(window,33,&corners,sizeof(corners));
#endif
}
bool WindowBase::event(QEvent *event) {
  if (event->type() == QEvent::Close && !routingClose_) { routingClose_=true; closeRequested(); routingClose_=false; event->ignore(); return true; }
  if (event->type() == QEvent::Expose) {
#ifdef Q_OS_WIN
    HWND window=reinterpret_cast<HWND>(winId());LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE),fixed=resizable_?(style&~WS_POPUP)|WS_THICKFRAME|WS_MAXIMIZEBOX:style&~(WS_POPUP|WS_THICKFRAME|WS_MAXIMIZEBOX);if(style!=fixed){SetWindowLongPtrW(window,GWL_STYLE,fixed);SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);}
#endif
    renderNow();
  }
  if (event->type() == QEvent::UpdateRequest) { renderNow(); return true; }
  if (event->type() == QEvent::Leave) contentLeave();
  return QWindow::event(event);
}
void WindowBase::resizeEvent(QResizeEvent *event) { if(resizable_)canvasSize_=canvasSizeFor(event->size()); backingStore_.resize(event->size()); renderNow(); }
QSize WindowBase::clientSizeFor(const QSize &canvas) const {
#ifdef Q_OS_WIN
  return canvas-QSize(2,TitleBarHeight+3);
#else
  return canvas;
#endif
}
QSize WindowBase::canvasSizeFor(const QSize &client) const {
#ifdef Q_OS_WIN
  return client+QSize(2,TitleBarHeight+3);
#else
  return client;
#endif
}
void WindowBase::setCanvasSize(const QSize &size) { canvasSize_=size;const QSize clientSize=clientSizeFor(size);
  if(!resizable_){setMaximumSize(clientSize);setMinimumSize(clientSize);}resize(clientSize);requestUpdate();
}
// buddyui.ocm creates the Buddy List with WS_OVERLAPPEDWINDOW and answers WM_GETMINMAXINFO with the control group's ideal size; sign-on/dialog windows omit WS_THICKFRAME.
void WindowBase::setResizable(const QSize &minimumCanvas) { resizable_=true;setMinimumSize(clientSizeFor(minimumCanvas));setMaximumSize(QSize(16777215,16777215));
#ifdef Q_OS_WIN
  setFlags(flags()&~Qt::MSWindowsFixedSizeDialogHint);HWND window=reinterpret_cast<HWND>(winId());SetWindowLongPtrW(window,GWL_STYLE,(GetWindowLongPtrW(window,GWL_STYLE)&~WS_POPUP)|WS_THICKFRAME|WS_MAXIMIZEBOX);SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
#endif
}
void WindowBase::renderNow() {
  if (!isExposed() || width() <= 0 || height() <= 0) return;
#ifdef Q_OS_WIN
  int dark=0;DwmSetWindowAttribute(reinterpret_cast<HWND>(winId()),20,&dark,sizeof(dark));
#endif
  const QRegion region(0, 0, width(), height());
  backingStore_.beginPaint(region);
  QPainter painter(backingStore_.paintDevice());
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.fillRect(QRect(0, 0, width(), height()), QColor(244, 244, 244));
#ifdef Q_OS_WIN
  painter.translate(-1,-TitleBarHeight);
#else
  painter.save(); drawFrame(painter); painter.restore();
#endif
  paintContent(painter);
  painter.end();
  backingStore_.endPaint();
  backingStore_.flush(region);
}
void WindowBase::drawFrame(QPainter &painter) {
  painter.fillRect(QRect(1, 1, width() - 2, TitleBarHeight - 1), QColor(246, 246, 246));
  painter.setPen(QColor(172, 172, 172));
  painter.drawLine(1, TitleBarHeight - 1, width() - 2, TitleBarHeight - 1);
  painter.setPen(QColor(249, 249, 249));
  painter.drawLine(1, 1, width() - 2, 1);
  painter.drawLine(1, 1, 1, height() - 2);
  painter.setPen(QColor(85, 85, 85));
  painter.drawRect(0, 0, width() - 1, height() - 1);
  if (!icon_.isNull()) painter.drawImage(QRect(5, 4, 16, 16), icon_);
  QFont font(QStringLiteral("MS Sans Serif"), 8);
  painter.setFont(font);
  painter.setPen(QColor(30, 30, 30));
  painter.drawText(QPoint(25, 16), title_);
  for (int i = 0; i < 3; ++i) {
    if (!(captionMask_ & (1 << i))) continue;
    const QRect r = titleButtonRect(i);
    painter.fillRect(r, QColor(246, 246, 246));
    painter.setPen(QColor(40, 40, 40));
    if (i == 0) painter.drawLine(r.center().x() - 4, r.center().y() + 4, r.center().x() + 4, r.center().y() + 4);
    else if (i == 1) painter.drawRect(r.center().x() - 4, r.center().y() - 4, 8, 8);
    else {
      painter.drawLine(r.center().x() - 4, r.center().y() - 4, r.center().x() + 4, r.center().y() + 4);
      painter.drawLine(r.center().x() + 4, r.center().y() - 4, r.center().x() - 4, r.center().y() + 4);
    }
  }
}
QRect WindowBase::titleButtonRect(int index) const {
  if (!(captionMask_ & (1 << index))) return {};
  int count = 0, before = 0;
  for (int i = 0; i < 3; ++i) if (captionMask_ & (1 << i)) { ++count; if (i < index) ++before; }
  return QRect(width() - count * 45 + before * 45, 1, 45, TitleBarHeight - 2);
}
void WindowBase::setCaptionButtons(bool minimize, bool maximize, bool closeButton) { captionMask_ = (minimize ? 1 : 0) | (maximize ? 2 : 0) | (closeButton ? 4 : 0);
#ifdef Q_OS_WIN
  Qt::WindowFlags flags=Qt::Window|Qt::WindowTitleHint|Qt::WindowSystemMenuHint;if(!resizable_)flags|=Qt::MSWindowsFixedSizeDialogHint;
  if(minimize) flags|=Qt::WindowMinimizeButtonHint; if(maximize) flags|=Qt::WindowMaximizeButtonHint; if(closeButton) flags|=Qt::WindowCloseButtonHint; setFlags(flags);
#endif
  renderNow(); }
void WindowBase::mousePressEvent(QMouseEvent *event) {
  const QPoint point = event->position().toPoint();
#ifndef Q_OS_WIN
  if (resizable_ && event->button() == Qt::LeftButton) {
    constexpr int grip = 4; Qt::Edges edges;
    if (point.x() < grip) edges |= Qt::LeftEdge; else if (point.x() >= width() - grip) edges |= Qt::RightEdge;
    if (point.y() < grip) edges |= Qt::TopEdge; else if (point.y() >= height() - grip) edges |= Qt::BottomEdge;
    if (edges && startSystemResize(edges)) return;
  }
  if (event->button() == Qt::LeftButton && point.y() < TitleBarHeight) {
    for (int i = 0; i < 3; ++i) if ((captionMask_ & (1 << i)) && titleButtonRect(i).contains(point)) { if (i == 0) setVisibility(QWindow::Minimized); else if (i == 2) closeRequested(); return; }
    if (point.x() < width() - ((captionMask_ & 1) + ((captionMask_ >> 1) & 1) + ((captionMask_ >> 2) & 1)) * 45) { startSystemMove(); return; }
  }
#endif
  contentMousePress(canvasPoint(point), event->button());
}
void WindowBase::mouseReleaseEvent(QMouseEvent *event) { contentMouseRelease(canvasPoint(event->position().toPoint()), event->button()); }
void WindowBase::mouseDoubleClickEvent(QMouseEvent *event) { contentMouseDoubleClick(canvasPoint(event->position().toPoint()),event->button()); }
void WindowBase::mouseMoveEvent(QMouseEvent *event) { contentMouseMove(canvasPoint(event->position().toPoint())); }
void WindowBase::keyPressEvent(QKeyEvent *event) { contentKeyPress(event); }
void WindowBase::contentMousePress(const QPoint &, Qt::MouseButton) {}
void WindowBase::contentMouseRelease(const QPoint &, Qt::MouseButton) {}
void WindowBase::contentMouseDoubleClick(const QPoint &, Qt::MouseButton) {}
void WindowBase::contentMouseMove(const QPoint &) {}
void WindowBase::contentLeave() {}
void WindowBase::contentKeyPress(QKeyEvent *event) { QWindow::keyPressEvent(event); }
void WindowBase::closeRequested() { close(); }
bool WindowBase::confirmExit(bool &suppress) {
#ifdef Q_OS_WIN
  QFile file(QStringLiteral(":/aim/dialogs/204"));if(!file.open(QIODevice::ReadOnly))return false;QByteArray bytes=file.readAll();
  auto handler=[](HWND window,UINT message,WPARAM parameter,LPARAM data)->INT_PTR { if(message==WM_INITDIALOG){SetWindowLongPtrW(window,DWLP_USER,data);SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1))));return TRUE;}if(message==WM_COMMAND&&(LOWORD(parameter)==360||LOWORD(parameter)==361)){if(auto *value=reinterpret_cast<bool*>(GetWindowLongPtrW(window,DWLP_USER)))*value=IsDlgButtonChecked(window,362)==BST_CHECKED;EndDialog(window,LOWORD(parameter)==360?IDYES:IDNO);return TRUE;}if(message==WM_CLOSE){EndDialog(window,IDNO);return TRUE;}return FALSE;};
  return DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<const DLGTEMPLATE*>(bytes.constData()),reinterpret_cast<HWND>(winId()),handler,reinterpret_cast<LPARAM>(&suppress))==IDYES;
#else
  Q_UNUSED(suppress);return false;
#endif
}
void WindowBase::drawText(QPainter &painter, const QPoint &at, const QString &text, const QColor &color, const QFont &font) const { painter.setPen(color); painter.setFont(font); painter.drawText(at, text); }
