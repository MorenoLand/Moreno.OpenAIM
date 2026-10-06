#include "native_dialog.h"
#ifdef Q_OS_WIN
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QRegularExpression>
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
    // Icon / bitmap statics name an AimRes resource id in their text.
    const DWORD type = style & SS_TYPEMASK; bool numeric = false; const int resource = control.value("text").toString().toInt(&numeric);
    if (name == "STATIC" && numeric && type == SS_ICON) { static QHash<int, HICON> icons; if (!icons.contains(resource)) icons.insert(resource, QIcon(QStringLiteral(":/aim/icons/%1").arg(resource)).pixmap(32, 32).toImage().toHICON()); SendMessageW(child, STM_SETICON, reinterpret_cast<WPARAM>(icons.value(resource)), 0); }
    if (name == "STATIC" && numeric && type == SS_BITMAP) { static QHash<int, HBITMAP> bitmaps; if (!bitmaps.contains(resource)) bitmaps.insert(resource, QImage(QStringLiteral(":/aim/art/%1").arg(resource)).toHBITMAP()); SendMessageW(child, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(bitmaps.value(resource))); }
  }
}

namespace {
struct DialogState { const std::function<void(HWND)> *init; const std::function<bool(HWND, int, int)> *command; QJsonObject dialog; HFONT font = nullptr; };
INT_PTR CALLBACK originalDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  auto *state = reinterpret_cast<DialogState *>(GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<DialogState *>(lParam); SetWindowLongPtrW(window, DWLP_USER, lParam);
    LOGFONTW logical{}; HFONT dialogFont = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)); if (dialogFont && GetObjectW(dialogFont, sizeof(logical), &logical)) state->font = CreateFontIndirectW(&logical);
    createNativeControls(window, state->dialog, state->font);
    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1))));
    if (*state->init) (*state->init)(window);
    return TRUE;
  }
  if (message == WM_COMMAND && state) { if (*state->command && (*state->command)(window, LOWORD(wParam), HIWORD(wParam))) return TRUE; if (LOWORD(wParam) == IDCANCEL) { EndDialog(window, IDCANCEL); return TRUE; } }
  if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
  return FALSE;
}
}
namespace {
struct ModelessState { std::function<void(HWND)> init; std::function<bool(HWND, int, int)> command; QJsonObject dialog; HFONT font = nullptr; };
INT_PTR CALLBACK modelessDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  auto *state = reinterpret_cast<ModelessState *>(GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<ModelessState *>(lParam); SetWindowLongPtrW(window, DWLP_USER, lParam);
    LOGFONTW logical{}; HFONT dialogFont = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)); if (dialogFont && GetObjectW(dialogFont, sizeof(logical), &logical)) state->font = CreateFontIndirectW(&logical);
    createNativeControls(window, state->dialog, state->font);
    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1))));
    if (state->init) state->init(window);
    return TRUE;
  }
  if (!state) return FALSE;
  if (message == WM_COMMAND) { if (state->command && state->command(window, LOWORD(wParam), HIWORD(wParam))) return TRUE; if (LOWORD(wParam) == IDCANCEL) { DestroyWindow(window); return TRUE; } }
  if (message == WM_CLOSE) { if (!state->command || !state->command(window, IDCANCEL, BN_CLICKED)) DestroyWindow(window); return TRUE; }
  if (message == WM_NCDESTROY) { if (state->font) DeleteObject(state->font); SetWindowLongPtrW(window, DWLP_USER, 0); delete state; }
  return FALSE;
}
}
HWND createOriginalDialog(HWND owner, int id, std::function<void(HWND)> init, std::function<bool(HWND, int, int)> command) {
  auto *state = new ModelessState{std::move(init), std::move(command), originalDialog(id)}; if (state->dialog.isEmpty()) { delete state; return nullptr; }
  const QByteArray bytes = nativeDialogTemplate(state->dialog, true);
  HWND window = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATE>(bytes.constData()), owner, modelessDialogProc, reinterpret_cast<LPARAM>(state));
  if (!window) { delete state; return nullptr; }
  ShowWindow(window, SW_SHOW); return window;
}
INT_PTR runOriginalDialog(HWND owner, int id, const std::function<void(HWND)> &init, const std::function<bool(HWND, int, int)> &command) {
  DialogState state{&init, &command, originalDialog(id)}; if (state.dialog.isEmpty()) return -1;
  const QByteArray bytes = nativeDialogTemplate(state.dialog, true);
  const INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATE>(bytes.constData()), owner, originalDialogProc, reinterpret_cast<LPARAM>(&state));
  if (state.font) DeleteObject(state.font);
  return result;
}
QString formatAimString(QString text, const QStringList &arguments) {
  static const QRegularExpression specifier(QStringLiteral("%(?:0\\.\\d+)?l?[sdu]"));
  for (const QString &argument : arguments) { const QRegularExpressionMatch match = specifier.match(text); if (!match.hasMatch()) break; text.replace(match.capturedStart(), match.capturedLength(), argument); }
  return text.replace(QStringLiteral("%%"), QStringLiteral("%"));
}
QString nativeWindowText(HWND window) { int length = GetWindowTextLengthW(window); std::vector<wchar_t> text(size_t(length) + 1); GetWindowTextW(window, text.data(), length + 1); return QString::fromWCharArray(text.data()); }
#endif
