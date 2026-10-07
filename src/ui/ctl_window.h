#pragma once
#include "window_base.h"
#include "ate_toolbar.h"
#include "ctl_group.h"
#include "menu_bar.h"
#include "text_editor.h"
#include <QHash>
#include <QStringList>
#include <QTextDocument>
#include <map>
#include <memory>
#include <QSet>

// A window whose client area is an original CTLGROUP laid out by the ported engine. It draws and operates the
// generic control kinds (statics, separators, art buttons, push buttons, edit controls, ATE panes, lists, rate meter);
// subclasses supply texts, list contents and command handling.
class CtlWindow : public WindowBase {
public:
  CtlWindow(const QString &title, int ctlGroupId, int menuId = 0);
protected:
  std::shared_ptr<CtlObject> group_;
  PaintedMenuBar menu_;
  // client-area size helpers (outer = window including frame)
  static QSize canvasForClient(const QSize &client);
  QRect clientRect() const;
  QRect layoutRect() const;           // client minus the painted menu bar
  void relayout();
  CtlObject *object(quint32 id) const { return group_ ? group_->find(id) : nullptr; }
  // controls
  TextEditor &editor(quint32 id);     // edit control or editable ATE
  QTextDocument &document(quint32 id); // read-only ATE
  void setFocusControl(quint32 id) { focus_ = id; requestUpdate(); }
  quint32 focusControl() const { return focus_; }
  void setReadOnlyFollowBottom(quint32 id) { follow_[id] = true; }
  // hooks
  virtual QString staticText(const CtlObject &object) const;
  virtual void command(int id) { Q_UNUSED(id); }
  virtual bool controlEnabled(quint32 id) const { Q_UNUSED(id); return true; }
  virtual QStringList listRows(quint32 id) const { Q_UNUSED(id); return {}; }
  virtual QColor listRowColor(quint32 id, int row) const { Q_UNUSED(id); Q_UNUSED(row); return Qt::black; }
  virtual void listActivated(quint32 id, int row) { Q_UNUSED(id); Q_UNUSED(row); }
  virtual bool submitEditor(quint32 id) { Q_UNUSED(id); return false; } // Enter in a control; true = consumed
  virtual void editorChanged(quint32 id) { Q_UNUSED(id); }
  virtual void toolbarCommand(quint32 ateId, int command);
  virtual bool paintCustom(QPainter &painter, const CtlObject &object) { Q_UNUSED(painter); Q_UNUSED(object); return false; } // true = drawn by the subclass
  virtual bool isComposePane(quint32 id) const { Q_UNUSED(id); return false; } // follows Preferences > IM/Chat composing defaults
  virtual qreal documentZoom(quint32 id) const { Q_UNUSED(id); return 1.0; }  // Text Magnification for history panes
  virtual void menuCommand(int id) { command(id); }
  virtual QList<MenuItem> preparedMenu(int index) const { return menu_.items.value(index).children; }
  int selectedRow(quint32 listId) const { return selection_.value(listId, -1); }
  void setSelectedRow(quint32 listId, int row) { selection_[listId] = row; requestUpdate(); }
  bool isEditable(const CtlObject &o) const;
  // WindowBase
  void paintContent(QPainter &painter) override;
  void contentMousePress(const QPoint &point, Qt::MouseButton button) override;
  void contentMouseRelease(const QPoint &point, Qt::MouseButton button) override;
  void contentMouseDoubleClick(const QPoint &point, Qt::MouseButton button) override;
  void contentMouseMove(const QPoint &point) override;
  void contentLeave() override;
  void contentKeyPress(QKeyEvent *event) override;
  bool event(QEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  QSet<quint32> editableAtes_;
private:
  struct Pane { std::unique_ptr<TextEditor> editor; std::unique_ptr<QTextDocument> document; std::unique_ptr<AteToolbar> toolbar; qreal scroll = 0; };
  Pane &pane(quint32 id);
  QRect toolbarRect(const CtlObject &o) const;
  QRect textRect(const CtlObject &o) const;
  void paintObject(QPainter &painter, CtlObject &object);
  void paintEditor(QPainter &painter, const CtlObject &object, const QRect &area, Pane &pane, bool singleLine);
  void paintDocument(QPainter &painter, const QRect &area, Pane &pane, quint32 id);
  CtlObject *hit(const QPoint &point) const;
  QList<quint32> focusOrder() const;
  void openMenu(int index);
  std::map<quint32, Pane> panes_; // Pane is move-only
  QHash<quint32, int> selection_;
  QHash<quint32, bool> follow_;
  QVector<QRect> menuRects_;
  quint32 focus_ = 0, hovered_ = 0, pressed_ = 0;
  int hoveredMenu_ = -1, openMenu_ = -1, hoveredTool_ = -1, pressedTool_ = -1;
  quint32 toolPane_ = 0;
};
