#pragma once
#include "window_base.h"
#include <functional>

class ConnectionWindow final : public WindowBase {
public:
  using SaveHandler = std::function<void(const QString &, quint16)>;
  ConnectionWindow(const QString &host, quint16 port, QWindow *parent);
  void setSaveHandler(SaveHandler handler);
protected:
  void paintContent(QPainter &painter) override;
  void contentMousePress(const QPoint &point, Qt::MouseButton button) override;
  void contentKeyPress(QKeyEvent *event) override;
private:
  void save();
  QString host_;
  QString port_;
  QString status_;
  SaveHandler saveHandler_;
  int activeField_ = 0;
};
