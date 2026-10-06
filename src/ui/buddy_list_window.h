#pragma once
#include "window_base.h"
#include "menu_template.h"
#include "../oscar/client.h"
#include "text_editor.h"
#include <functional>
#include <optional>
#include <QImage>
#include <QPointer>
#include <QSettings>
#include <QSet>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>

struct CtlObject;

// Buddy List window of buddyui.ocm: CTLGROUP 101 laid out by the ported engine, RT_MENU 103 bar, Online / List Setup
// tab pages, Today/Away/Preferences box, Today banner and stock ticker.
class BuddyListWindow final : public WindowBase {
  Q_OBJECT
public:
  explicit BuddyListWindow(OscarClient *client);
  ~BuddyListWindow() override;
  static constexpr int AwaySavedBase = 24100; // + index of a saved away message (Away Message submenu)
  void requestExit();
  void preferencesChanged(); // Preferences applied: stock ticker visibility, tree font
  static void showAbout(QWindow *owner);              // Help > About (RT_DIALOG 111)
  static void showHelp(QWindow *owner, int command);  // 158 / 156 / 705 WinHelp entries
  static void runHelpCommand(QWindow *owner, const QString &screenName, int command); // 158/156/705/159/902/160
signals:
  void exitAccepted();
  void actionRequested(int id, const QString &screenName);
protected:
  void paintContent(QPainter &painter) override;
  void contentMousePress(const QPoint &point, Qt::MouseButton button) override;
  void contentMouseRelease(const QPoint &point, Qt::MouseButton button) override;
  void contentMouseMove(const QPoint &point) override;
  void contentLeave() override;
  void contentMouseDoubleClick(const QPoint &point, Qt::MouseButton button) override;
  void contentKeyPress(QKeyEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void closeRequested() override;
  bool event(QEvent *event) override;
  bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
private:
  static constexpr unsigned DockCallback = 0x8000 + 0x4D; // WM_APP-based appbar notification
  void dock(int edge);
  void undock();
  int dockEdge_ = -1, undockedHeight_ = 0;
  QElapsedTimer created_;
  struct Row { QRect rect; quint16 groupId; quint16 itemId; QString name; bool group; bool pending = false; };
  // List Setup in-place label editing (oscarui _Oscar_Tree editor, Research/remaining_buttons.md 1.4-1.5).
  enum class EditResult { Accept, Reject, Delete };
  struct LabelEdit { bool group = false, pending = false; quint16 groupId = 0, itemId = 0; QString original; int maxLength = 32; std::unique_ptr<TextEditor> editor; };
  struct PendingBuddy { quint16 groupId = 0, afterItemId = 0; }; // *New Buddy* row: nothing is sent until it is named
  void addBuddy();
  void addGroup();
  void deleteSelection();
  void editName();
  void showContextMenu(const QPoint &point);
  void showNetFindMenu();
  bool requireOnline();
  bool validateList();
  void scheduleEdit(int delay);
  void beginEdit(const QString &initial = {}, bool typed = false);
  void endEdit(bool commit, bool interactive);
  EditResult checkGroup(QString &text, bool interactive);
  EditResult checkBuddy(QString &text, bool interactive);
  void rosterEdit(std::function<void()> change);
  bool hasRealGroup() const;
  void selectRow(quint16 groupId, quint16 itemId, bool group, const QString &name, bool pending = false);
  void showPage(bool listSetup);
  void paintEditor(QPainter &painter, const QRect &rect);
  QRect client() const;
  void layout();
  QVector<QRect> menuRects() const;
  void openMenu(int index);
  QList<MenuItem> preparedMenu(const MenuItem &top) const;
  void paintObject(QPainter &painter, CtlObject &object);
  void paintTabs(QPainter &painter, CtlObject &tabs);
  void paintTree(QPainter &painter, const QRect &area);
  QVector<QRect> tabRects(const CtlObject &tabs) const;
  QRect tabWindow(const CtlObject &tabs) const;
  CtlObject *buttonAt(const QPoint &point) const;
  void command(int id);
  void showAwayMenu(const QRect &anchor);
  void showClosedNotice();
  OscarClient *client_ = nullptr;
  QSettings settings_;
  std::shared_ptr<CtlObject> group_;
  QImage logo_, banner_;
  QPointer<QWindow> confirmation_;
  bool listSetup_ = false;
  quint32 hovered_ = 0, pressed_ = 0;
  QString selectedName_;
  QVector<Row> rows_;
  QSet<quint16> collapsedGroups_;
  quint16 selectedGroupId_ = 0, selectedItemId_ = 0;
  bool selectedGroup_ = false, selectedPending_ = false;
  std::optional<PendingBuddy> pendingBuddy_;
  std::unique_ptr<LabelEdit> edit_;
  QRect editRect_;
  QTimer editTimer_;
  bool committing_ = false, pressedOnSelection_ = false;
  QList<std::function<void()>> rosterQueue_;
  quint16 editGroupAfterRoster_ = 0;
  int treeScroll_ = 0;
  QRect treeArea_;
  QList<MenuItem> menuBar_;
  QVector<QRect> menuRects_;
  int hoveredMenu_ = -1, openMenu_ = -1;
  QTimer tickerTimer_;
  int tickerOffset_ = 0;
};
