#pragma once
#include <QBackingStore>
#include <QImage>
#include <QPainter>
#include <QWindow>

class QMouseEvent;
class QKeyEvent;
class QResizeEvent;

class WindowBase : public QWindow {
public:
  WindowBase(const QString &title, const QSize &size, QWindow *parent = nullptr);
protected:
  static constexpr int TitleBarHeight = 24;
  static QImage transparentBitmap(const QString &path, QRgb key = 0x00ff00ffu) {
    QImage image = QImage(path).convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) { auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(y)); for (int x = 0; x < image.width(); ++x) if ((pixels[x] & 0x00ffffffu) == (key & 0x00ffffffu)) pixels[x] = 0; }
    return image;
  }
  void renderNow();
  int canvasWidth() const { return canvasSize_.width(); }
  int canvasHeight() const { return canvasSize_.height(); }
  QPoint canvasPoint(const QPoint &point) const {
#ifdef Q_OS_WIN
    return point + QPoint(1, TitleBarHeight);
#else
    return point;
#endif
  }
  QPoint canvasToGlobal(const QPoint &point) const {
#ifdef Q_OS_WIN
    return mapToGlobal(point - QPoint(1, TitleBarHeight));
#else
    return mapToGlobal(point);
#endif
  }
  void setCaptionButtons(bool minimize, bool maximize, bool closeButton);
  bool confirmExit(bool &suppress);
  void setCanvasSize(const QSize &size);
  void setResizable(const QSize &minimumCanvas);
  virtual void paintContent(QPainter &painter) = 0;
  virtual void contentMousePress(const QPoint &point, Qt::MouseButton button);
  virtual void contentMouseRelease(const QPoint &point, Qt::MouseButton button);
  virtual void contentMouseDoubleClick(const QPoint &point, Qt::MouseButton button);
  virtual void contentMouseMove(const QPoint &point);
  virtual void contentLeave();
  virtual void contentKeyPress(QKeyEvent *event);
  virtual void closeRequested();
  bool event(QEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void drawText(QPainter &painter, const QPoint &at, const QString &text, const QColor &color, const QFont &font) const;
private:
  void drawFrame(QPainter &painter);
  QRect titleButtonRect(int index) const;
  QSize clientSizeFor(const QSize &canvas) const;
  QSize canvasSizeFor(const QSize &client) const;
  QBackingStore backingStore_;
  QImage icon_;
  QString title_;
  int captionMask_ = 7;
  QSize canvasSize_;
  bool routingClose_ = false;
  bool resizable_ = false;
};
