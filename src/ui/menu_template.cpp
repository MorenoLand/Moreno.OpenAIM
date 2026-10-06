#include "menu_template.h"
#include <QFile>
#include <QWindow>
#include <QtEndian>
#include <functional>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
constexpr quint16 MenuGrayed = 0x0001, MenuChecked = 0x0008, MenuPopup = 0x0010, MenuEnd = 0x0080;
QList<MenuItem> parseLevel(const QByteArray &data, qsizetype &offset) {
  QList<MenuItem> items;
  while (offset + 2 <= data.size()) {
    const quint16 flags = qFromLittleEndian<quint16>(data.constData() + offset); offset += 2;
    MenuItem item; item.grayed = flags & MenuGrayed; item.checked = flags & MenuChecked;
    if (!(flags & MenuPopup)) { if (offset + 2 > data.size()) break; item.id = qFromLittleEndian<quint16>(data.constData() + offset); offset += 2; }
    while (offset + 2 <= data.size()) { const char16_t c = qFromLittleEndian<quint16>(data.constData() + offset); offset += 2; if (!c) break; item.text.append(QChar(c)); }
    if (flags & MenuPopup) item.children = parseLevel(data, offset);
    items.append(item);
    if (flags & MenuEnd) break;
  }
  return items;
}
}

QString MenuItem::label() const { QString value = text.section(QLatin1Char('\t'), 0, 0); value.replace(QStringLiteral("&&"), QStringLiteral("\x01")); value.remove(QLatin1Char('&')); value.replace(QChar(1), QLatin1Char('&')); return value; }
QChar MenuItem::mnemonic() const { for (qsizetype i = 0; i + 1 < text.size(); ++i) if (text[i] == QLatin1Char('&')) { if (text[i + 1] == QLatin1Char('&')) { ++i; continue; } return text[i + 1].toLower(); } return {}; }

QList<MenuItem> parseMenuTemplate(const QByteArray &data) {
  if (data.size() < 4) return {};
  const quint16 version = qFromLittleEndian<quint16>(data.constData()), headerSize = qFromLittleEndian<quint16>(data.constData() + 2);
  if (version != 0) return {};
  qsizetype offset = 4 + headerSize;
  return parseLevel(data, offset);
}

QList<MenuItem> loadMenuResource(int id) { QFile file(QStringLiteral(":/aim/menus/%1").arg(id)); return file.open(QIODevice::ReadOnly) ? parseMenuTemplate(file.readAll()) : QList<MenuItem>{}; }

int popupMenu(QWindow *owner, const QList<MenuItem> &items, const QPoint &globalPosition, const QList<int> &disabledIds) {
#ifdef Q_OS_WIN
  if (!owner || !owner->handle() || items.isEmpty()) return 0;
  std::function<HMENU(const QList<MenuItem> &)> build = [&](const QList<MenuItem> &level) {
    HMENU menu = CreatePopupMenu();
    for (const MenuItem &item : level) {
      if (item.isSeparator()) { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); continue; }
      UINT flags = MF_STRING | ((item.grayed || disabledIds.contains(item.id)) ? MF_GRAYED : MF_ENABLED) | (item.checked ? MF_CHECKED : 0);
      if (!item.children.isEmpty()) AppendMenuW(menu, flags | MF_POPUP, reinterpret_cast<UINT_PTR>(build(item.children)), reinterpret_cast<LPCWSTR>(item.text.utf16()));
      else AppendMenuW(menu, flags, UINT_PTR(item.id), reinterpret_cast<LPCWSTR>(item.text.utf16()));
    }
    return menu;
  };
  HMENU menu = build(items); HWND window = reinterpret_cast<HWND>(owner->winId());
  const int selected = int(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, globalPosition.x(), globalPosition.y(), window, nullptr));
  DestroyMenu(menu);
  return selected;
#else
  Q_UNUSED(owner); Q_UNUSED(items); Q_UNUSED(globalPosition); Q_UNUSED(disabledIds);
  return 0; // TODO: painted popup for non-Windows platforms
#endif
}
