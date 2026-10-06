#include "native_dialog.h"
#ifdef Q_OS_WIN
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <commctrl.h>
#include <richedit.h>
#include <vector>

QJsonObject originalDialog(int id) {
  static const QJsonObject inventory = [] { QFile file(QStringLiteral(":/aim/dialogs.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject(); }();
  return inventory.value(QString::number(id)).toObject();
}

QByteArray nativeDialogTemplate(const QJsonObject &dialog, bool popup) {
  QByteArray bytes;
  auto word = [&bytes](WORD value) { bytes.append(reinterpret_cast<const char *>(&value), sizeof(value)); };
  auto dword = [&bytes](DWORD value) { bytes.append(reinterpret_cast<const char *>(&value), sizeof(value)); };
  auto string = [&word](const QString &value) { for (QChar c : value) word(c.unicode()); word(0); };
  DWORD style = popup ? WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT : WS_CHILD | DS_CONTROL | DS_SETFONT;
  dword(style); dword(popup ? 0 : WS_EX_CONTROLPARENT); word(0); word(0); word(0); word(WORD(dialog.value("width").toInt())); word(WORD(dialog.value("height").toInt())); word(0); word(0); string(dialog.value("title").toString());
  QJsonObject font = dialog.value("font").toObject(); word(WORD(font.value("points").toInt(8))); string(font.value("face").toString("MS Sans Serif"));
  return bytes;
}

QString nativeControlClass(const QJsonValue &value) {
  if (value.isDouble()) { switch (value.toInt()) { case 128: return "BUTTON"; case 129: return "EDIT"; case 130: return "STATIC"; case 131: return "LISTBOX"; case 132: return "SCROLLBAR"; case 133: return "COMBOBOX"; } }
  QString name = value.toString(); if (name == "WndAte32Class") return "RICHEDIT50W"; if (name == "_Oscar_Tree") return "SysTreeView32"; if (name == "_Oscar_UserListWnd") return "SysListView32"; return name;
}

void createNativeControls(HWND window, const QJsonObject &dialog, HFONT font) {
  static const bool richEdit = LoadLibraryW(L"Msftedit.dll") != nullptr; Q_UNUSED(richEdit);
  for (const QJsonValue &value : dialog.value("controls").toArray()) {
    const QJsonObject control = value.toObject(); const QString original = control.value("class").toString(); const QString name = nativeControlClass(control.value("class")); QString text = control.value("text").toString();
    RECT r{control.value("x").toInt(), control.value("y").toInt(), control.value("x").toInt() + control.value("width").toInt(), control.value("y").toInt() + control.value("height").toInt()}; MapDialogRect(window, &r);
    DWORD style = DWORD(control.value("style").toDouble());
    if (original == "WndAte32Class") { style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL; text.clear(); }
    if (original == "_Oscar_UserListWnd") style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_LIST | LVS_SINGLESEL;
    if (original == "_Oscar_Tree") style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | TVS_HASLINES | TVS_HASBUTTONS;
    HWND child = CreateWindowExW(DWORD(control.value("exstyle").toDouble()), reinterpret_cast<LPCWSTR>(name.utf16()), reinterpret_cast<LPCWSTR>(text.utf16()), style, r.left, r.top, r.right - r.left, r.bottom - r.top, window, reinterpret_cast<HMENU>(INT_PTR(control.value("id").toInt())), GetModuleHandleW(nullptr), nullptr);
    if (!child) { qWarning() << "Original dialog control failed" << control.value("id") << name << GetLastError(); continue; }
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (name == "RICHEDIT50W") SendMessageW(child, EM_SETEVENTMASK, 0, ENM_CHANGE);
  }
}

QString nativeWindowText(HWND window) { int length = GetWindowTextLengthW(window); std::vector<wchar_t> text(size_t(length) + 1); GetWindowTextW(window, text.data(), length + 1); return QString::fromWCharArray(text.data()); }
#endif
