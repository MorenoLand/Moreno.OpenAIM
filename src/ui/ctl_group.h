#pragma once
#include <QByteArray>
#include <QList>
#include <QMargins>
#include <QRect>
#include <QSize>
#include <memory>

// Object tree of an original AimRes.dll CTLGROUP resource (parsed as oscarui.dll's CtlGroupLoadResource does).
// Record layout and object flags: Research/im_window.md section 2 and Research/ctlgroup_layout.md.
struct CtlObject {
  enum class Kind { Group, Control, Separator, RateMeter, Static, Combo, Button, Ate };
  enum Flag : quint32 { StretchH = 0x1, StretchV = 0x2, IsGroup = 0x4, Padding = 0x20, FixedSize = 0x40, Hidden = 0x400, DialogUnits = 0x1000, FixedCell = 0x10000 };
  Kind kind = Kind::Control;
  quint32 id = 0;
  quint32 flags = 0;
  quint32 style = 0;
  // group
  bool vertical = false;
  quint16 packMode = 0;
  quint32 titleId = 0;
  // shared control data
  quint32 controlClass = 0;
  QSize minimum;
  QSize sizeHint;
  QMargins margins;
  quint16 align = 0;
  quint32 fontId = 0, textId = 0, tooltipId = 0;
  // 0x404 static: explicit rect hint
  QPoint staticOrigin;
  // 0x407 button
  quint16 buttonType = 0, placement = 0, gap = 0;
  quint32 art[3] = {0, 0, 0};
  quint32 valueId = 0;
  // 0x406 combo
  quint32 profileKeyId = 0;
  QByteArray raw;
  QList<std::shared_ptr<CtlObject>> children;
  QRect rect; // result of layout, client coordinates
  bool hidden() const { return flags & Hidden; }
  CtlObject *find(quint32 objectId);
};

std::shared_ptr<CtlObject> parseCtlGroup(const QByteArray &data);
std::shared_ptr<CtlObject> loadCtlGroup(int resourceId);
