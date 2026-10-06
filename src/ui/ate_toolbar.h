#pragma once
#include <QImage>
#include <QList>
#include <QRect>
#include <QString>

class QPainter;

// The rich-text pane toolbar (wndutils.dll CButtonBar, Research/ate_toolbar.md): a 21 px bar at the top of an ATE pane
// with 23x17 buttons cut from an AimRes toolbar strip, centred, 1 px apart, 8 px etched separators.
class AteToolbar {
public:
  enum class Set { Away, Chat, InstantMessage };      // strips 1001, 1015, 1014
  enum Command : int { TextColor = 0x50AE, BackgroundColor = 0x50E7, Smaller = 0x26F, NormalSize = 0x50E5, Larger = 0x270, Bold = 0x268, Italic = 0x26A, Underline = 0x26D, Link = 0x26B, Smiley = 0x31, ConnectImage = 0x332, InsertPicture = 0x265, Greeting = 0x4AF };
  static constexpr int Height = 21;
  explicit AteToolbar(Set set);
  struct Item { int command = 0; quint32 tooltip = 0; int cell = -1; QRect rect; bool separator() const { return cell < 0; } };
  void layout(const QRect &bar);
  // IM Image connected: the "Connect to Send IM Image" cell becomes "Insert Picture" (cmd 0x265, tip STRING 665).
  void setImageConnected(bool connected) { for (Item &item : items_) if (item.command == ConnectImage || item.command == InsertPicture) { item.command = connected ? InsertPicture : ConnectImage; item.tooltip = connected ? 665 : 1184; } }
  void paint(QPainter &painter, int hovered, int pressed, const QList<int> &checkedCommands) const;
  int hit(const QPoint &point) const; // item index or -1
  const QList<Item> &items() const { return items_; }
  QRect bar() const { return bar_; }
  // 4x4 smiley picker (strip 1008, mask 1009); returns the glyph index or -1.
  static int pickSmiley(class QWindow *owner, const QPoint &globalPosition);
  static QString smileyCode(int glyph);
  static QImage smileyImage(int glyph);
private:
  QList<Item> items_;
  QImage strip_;
  QRect bar_;
};
