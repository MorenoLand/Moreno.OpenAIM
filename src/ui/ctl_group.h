#pragma once
#include <QByteArray>
#include <QFont>
#include <QList>
#include <QMargins>
#include <QRect>
#include <QSize>
#include <QString>
#include <memory>

// Port of the CTLGROUP layout engine in oscarui.dll (AIM 4.7.2480). Every rule below follows
// Research/ctlgroup_layout.md, which cites the original code addresses; the object fields mirror the original
// 0x98-byte layout object.
struct CtlObject {
  enum class Kind { Group, TabGroup, Button, ListBox, ComboBox, ListView, TreeView, Edit, Tree, Trackbar, ArtButton, PersistentCombo, RateMeter, Separator, Static, Ate, TabBody, Unknown };
  enum Flag : quint32 { StretchH = 0x1, StretchV = 0x2, IsGroup = 0x4, Uniform = 0x10, Padding = 0x20, FixedSize = 0x40, NotShown = 0x200, Hidden = 0x400, NoMaxSize = 0x800, DialogUnits = 0x1000, Sized = 0x10000 };
  Kind kind = Kind::Unknown;
  quint32 id = 0, flags = 0, style = 0;
  bool vertical = false;          // groups: children flow top-to-bottom
  quint16 pack = 0;               // groups: pack mode 0..4
  quint16 align = 2;              // cross-axis alignment in the parent: 3 start, 4 end, other centre
  QSize base{0, 0};               // record w,h (+0x18)
  QSize hint{0, 0};               // record-4 size hint (+0x20)
  QMargins margins;               // +0x40..+0x4c
  quint32 fontId = 0, textId = 0, tooltipId = 0, titleId = 0;
  quint16 buttonType = 0, placement = 0, gap = 0;    // 0x407 art button
  quint32 art[3] = {0, 0, 0};
  quint32 profileKeyId = 0;                           // 0x406 persistent combo
  // layout results
  QSize ideal{0, 0}, minimum{0, 0}, size{0, 0}, content{0, 0}; // QSize() would be (-1,-1)
  QPoint pos;
  int visibleChildren = 0;
  CtlObject *parent = nullptr;
  QList<std::shared_ptr<CtlObject>> children;

  bool hidden() const { return flags & Hidden; }
  bool shown() const; // not hidden/not-shown here or in any ancestor
  QRect cell() const { return QRect(pos, size); }
  // Native window rectangle of a leaf: the cell inset by the object's own margins (the +4 pad stays inside).
  QRect windowRect() const { return QRect(pos.x() + margins.left(), pos.y() + margins.top(), size.width() - margins.left() - margins.right(), size.height() - margins.top() - margins.bottom()); }
  CtlObject *find(quint32 objectId);
};

// Supplies what the original obtained from GDI and AimRes.dll.
class CtlEnvironment {
public:
  virtual ~CtlEnvironment() = default;
  virtual QSize averageCharSize(quint32 fontId) const;                         // FontGetAvgSize: tmAveCharWidth, tmHeight+tmExternalLeading
  enum TextMode { ButtonText, StaticLine, StaticWrapped };                    // button: "&" is a mnemonic; statics: DT_NOPREFIX
  virtual QSize textExtent(quint32 fontId, const QString &text, int wrapWidth, TextMode mode) const; // DrawText DT_CALCRECT
  virtual QSize artSize(quint32 artId) const;                                  // RT_BITMAP biWidth/biHeight
  virtual int verticalScrollWidth() const;                                     // SM_CXVSCROLL
  virtual QString string(quint32 id) const;                                    // RT_STRING
  static QFont font(quint32 fontId);                                           // FONTDESC -> font
};

std::shared_ptr<CtlObject> parseCtlGroup(const QByteArray &data);
std::shared_ptr<CtlObject> loadCtlGroup(int resourceId);
void ctlShowControl(CtlObject &root, quint32 id, bool show);                   // CtlGroupShowControl
void ctlSetPage(CtlObject &tabGroup, quint32 pageId);                          // CtlGroupSetPage
QSize ctlIdealSize(CtlObject &root, const CtlEnvironment &environment);        // CtlGroupGetIdealSize
void ctlMove(CtlObject &root, const QRect &rect, const CtlEnvironment &environment); // CtlGroupMove
const CtlEnvironment &aimEnvironment();
