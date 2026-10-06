#pragma once
#include <QByteArray>
#include <QList>
#include <QPoint>
#include <QString>

class QWindow;

// One entry of an original RT_MENU template. A separator has an empty text and id 0.
struct MenuItem {
  QString text;
  int id = 0;
  bool grayed = false;
  bool checked = false;
  QList<MenuItem> children;
  bool isSeparator() const { return text.isEmpty() && children.isEmpty(); }
  QString label() const; // text before the accelerator tab, '&' markers removed
  QChar mnemonic() const;
};

// Parses a standard (version 0) MENUITEMTEMPLATE as stored in AimRes.dll.
QList<MenuItem> parseMenuTemplate(const QByteArray &data);
QList<MenuItem> loadMenuResource(int id);

// Shows a popup at a global position and returns the selected command id, or 0.
int popupMenu(QWindow *owner, const QList<MenuItem> &items, const QPoint &globalPosition, const QList<int> &disabledIds = {}, bool bottomAlign = false);
