// Prints the CTLGROUP layout computed by the C++ port, for comparison with Research/ctl_layout_sim.py.
// usage: openaim_layouttest <ctlgroup id> <client w> <client h> [inset l,t,r,b] [--im-mode 0x11|2] [--hide id,..] [--show id,..] [--page container:page]
#include "../src/ui/ctl_group.h"
#include <QCoreApplication>
#include <QStringList>
#include <cstdio>
#include <functional>

namespace {
const char *kindName(CtlObject::Kind kind) {
  switch (kind) {
  case CtlObject::Kind::Group: return "GROUP"; case CtlObject::Kind::TabGroup: return "TABGROUP"; case CtlObject::Kind::Button: return "BUTTON"; case CtlObject::Kind::ListBox: return "LISTBOX";
  case CtlObject::Kind::ComboBox: return "COMBOBOX"; case CtlObject::Kind::ListView: return "LISTVIEW"; case CtlObject::Kind::TreeView: return "TREEVIEW"; case CtlObject::Kind::Edit: return "EDIT";
  case CtlObject::Kind::Tree: return "TREE"; case CtlObject::Kind::Trackbar: return "TRACKBAR"; case CtlObject::Kind::ArtButton: return "ARTBTN"; case CtlObject::Kind::PersistentCombo: return "COMBO";
  case CtlObject::Kind::RateMeter: return "RATEMETER"; case CtlObject::Kind::Separator: return "SEPARATOR"; case CtlObject::Kind::Static: return "STATIC"; case CtlObject::Kind::Ate: return "ATE";
  case CtlObject::Kind::TabBody: return "TABBODY"; default: return "UNKNOWN";
  }
}
QList<quint32> ids(const QString &value) { QList<quint32> result; for (const QString &part : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) result.append(part.toUInt(nullptr, 0)); return result; }
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const QStringList args = app.arguments();
  if (args.size() < 4) { std::fprintf(stderr, "usage: %s <ctlgroup> <client w> <client h> [l,t,r,b] [--im-mode m] [--hide ids] [--show ids] [--page c:p]\n", argv[0]); return 2; }
  auto root = loadCtlGroup(args[1].toInt());
  if (!root) { std::fprintf(stderr, "CTLGROUP %s not found\n", qPrintable(args[1])); return 1; }
  QRect rect(0, 0, args[2].toInt(), args[3].toInt());
  for (int i = 4; i < args.size(); ++i) {
    const QString arg = args[i];
    if (arg == QLatin1String("--im-mode") && i + 1 < args.size()) {
      const int mode = args[++i].toInt(nullptr, 0);
      // icbmui SetMode 0x113908db
      const bool newMessage = mode == 0x11, conversation = mode == 2;
      ctlShowControl(*root, 0x191, !conversation); ctlShowControl(*root, 0x193, conversation);
      for (quint32 id : {0x195u, 0x196u, 0x2b7u, 0x197u}) ctlShowControl(*root, id, conversation);
      if (newMessage) ctlShowControl(*root, 0x193, false);
      ctlShowControl(*root, 0x198, false);
    } else if (arg == QLatin1String("--hide") && i + 1 < args.size()) { for (quint32 id : ids(args[++i])) ctlShowControl(*root, id, false); }
    else if (arg == QLatin1String("--show") && i + 1 < args.size()) { for (quint32 id : ids(args[++i])) ctlShowControl(*root, id, true); }
    else if (arg == QLatin1String("--page") && i + 1 < args.size()) { const QStringList parts = args[++i].split(QLatin1Char(':')); if (CtlObject *tab = root->find(parts.value(0).toUInt(nullptr, 0))) ctlSetPage(*tab, parts.value(1).toUInt(nullptr, 0)); }
    else if (arg.count(QLatin1Char(',')) == 3) { const QList<quint32> inset = ids(arg); rect.adjust(int(inset[0]), int(inset[1]), -int(inset[2]), -int(inset[3])); }
  }
  const QSize ideal = ctlIdealSize(*root, aimEnvironment());
  std::printf("# CtlGroupGetIdealSize(root) = %dx%d   root.min = %dx%d\n", ideal.width(), ideal.height(), root->minimum.width(), root->minimum.height());
  std::printf("# CtlGroupMove rect = (%d,%d,%d,%d)\n", rect.left(), rect.top(), rect.left() + rect.width(), rect.top() + rect.height());
  ctlMove(*root, rect, aimEnvironment());
  std::function<void(const CtlObject &, int)> print = [&](const CtlObject &o, int depth) {
    std::printf("%*s%-9s id=%u (0x%X) flags=0x%X", depth * 2, "", kindName(o.kind), o.id, o.id, o.flags);
    bool hidden = false; for (const CtlObject *a = &o; a; a = a->parent) hidden = hidden || a->hidden();
    if (hidden) std::printf("  HIDDEN (0x400)\n");
    else { const QRect c = o.cell(), w = o.windowRect(); std::printf("  cell=(%d,%d,%d,%d) %dx%d  inset-by-margins=(%d,%d,%d,%d) %dx%d  ideal=(%d, %d) min=(%d, %d)\n", c.left(), c.top(), c.left() + c.width(), c.top() + c.height(), c.width(), c.height(), w.left(), w.top(), w.left() + w.width(), w.top() + w.height(), w.width(), w.height(), o.ideal.width(), o.ideal.height(), o.minimum.width(), o.minimum.height()); }
    for (const auto &child : o.children) print(*child, depth + 1);
  };
  print(*root, 0);
  return 0;
}
