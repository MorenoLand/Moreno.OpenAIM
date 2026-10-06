#include "away_dialog.h"
#include <algorithm>
#include "ctl_group.h"
#include "native_dialog.h"
#include "../oscar/client.h"
#include <QSettings>
#include <QVariantMap>
#include <QWindow>
#ifdef Q_OS_WIN
#include <commctrl.h>
#include <richedit.h>
#endif

namespace {
const QString ItemsKey = QStringLiteral("nativePreferences/274/417/items");
#ifdef Q_OS_WIN
enum : int { LabelCombo = 417, MessageText = 418, SaveForLater = 425, ImAway = 421, SaveButton = 427, CurrentLabel = 424, CurrentText = 425, SetNotice = 423, SetNoticeIm = 823, SetNoticeAccept = 824 };
struct EditState { QVariantList items; QString label, text; QByteArray rtf; bool save = false; HFONT font = nullptr; };
DWORD CALLBACK streamOut(DWORD_PTR cookie, LPBYTE buffer, LONG size, LONG *written) { reinterpret_cast<QByteArray *>(cookie)->append(reinterpret_cast<const char *>(buffer), size); *written = size; return 0; }
struct StreamIn { QByteArray bytes; qsizetype position = 0; };
DWORD CALLBACK streamIn(DWORD_PTR cookie, LPBYTE buffer, LONG size, LONG *written) { auto *in = reinterpret_cast<StreamIn *>(cookie); *written = LONG(qMin(qsizetype(size), in->bytes.size() - in->position)); memcpy(buffer, in->bytes.constData() + in->position, size_t(*written)); in->position += *written; return 0; }
void initFont(HWND window, HFONT &font) { LOGFONTW logical{}; HFONT dialogFont = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)); if (dialogFont && GetObjectW(dialogFont, sizeof(logical), &logical)) font = CreateFontIndirectW(&logical); }
void updateAwayButton(HWND window) { EnableWindow(GetDlgItem(window, ImAway), GetWindowTextLengthW(GetDlgItem(window, MessageText)) > 0); }
INT_PTR CALLBACK editProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  auto *state = reinterpret_cast<EditState *>(GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<EditState *>(lParam); SetWindowLongPtrW(window, DWLP_USER, lParam); initFont(window, state->font);
    createNativeControls(window, originalDialog(148), state->font);
    ShowWindow(GetDlgItem(window, SaveButton), SW_HIDE); // the new-message variant offers I'm Away/Cancel only (reference screenshot)
    for (const QVariant &value : state->items) { const QString label = value.toMap().value("label").toString(); SendDlgItemMessageW(window, LabelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.utf16())); }
    updateAwayButton(window); SetFocus(GetDlgItem(window, LabelCombo)); return FALSE;
  }
  if (message == WM_COMMAND && state) {
    const int id = LOWORD(wParam), notice = HIWORD(wParam);
    if (id == MessageText && notice == EN_CHANGE) { updateAwayButton(window); return TRUE; }
    if (id == LabelCombo && notice == CBN_SELCHANGE) {
      const int index = int(SendDlgItemMessageW(window, LabelCombo, CB_GETCURSEL, 0, 0));
      if (index >= 0 && index < state->items.size()) { const QVariantMap item = state->items[index].toMap(); StreamIn in{item.value("418/rtf").toByteArray()}; if (!in.bytes.isEmpty()) { EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&in), 0, streamIn}; SendDlgItemMessageW(window, MessageText, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&stream)); } else SetDlgItemTextW(window, MessageText, reinterpret_cast<LPCWSTR>(item.value("418").toString().utf16())); updateAwayButton(window); }
      return TRUE;
    }
    if (id == ImAway && notice == BN_CLICKED) {
      state->label = nativeWindowText(GetDlgItem(window, LabelCombo)).trimmed(); state->text = nativeWindowText(GetDlgItem(window, MessageText)); state->save = IsDlgButtonChecked(window, SaveForLater) == BST_CHECKED;
      EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&state->rtf), 0, streamOut}; SendDlgItemMessageW(window, MessageText, EM_STREAMOUT, SF_RTF, reinterpret_cast<LPARAM>(&stream));
      if (state->text.trimmed().isEmpty()) return TRUE;
      EndDialog(window, ImAway); return TRUE;
    }
    if (id == IDCANCEL) { EndDialog(window, IDCANCEL); return TRUE; }
  }
  if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
  return FALSE;
}
INT_PTR CALLBACK currentProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  auto *self = reinterpret_cast<AwayMessages *>(GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) { SetWindowLongPtrW(window, DWLP_USER, lParam); return TRUE; }
  if (self && ((message == WM_COMMAND && LOWORD(wParam) == IDCANCEL) || message == WM_CLOSE)) { PostMessageW(window, WM_APP, 0, 0); return TRUE; }
  return FALSE;
}
#endif
}

AwayMessages::AwayMessages(OscarClient *client, QWindow *owner, QObject *parent) : QObject(parent), client_(client), owner_(owner) {
  if (client_) connect(client_, &OscarClient::awayChanged, this, [this](bool away) { if (!away) closeCurrent(); });
}
AwayMessages::~AwayMessages() { closeCurrent(); }

void AwayMessages::newMessage() {
#ifdef Q_OS_WIN
  if (!client_ || !client_->connected()) return;
  const QJsonObject dialog = originalDialog(148); if (dialog.isEmpty()) return;
  QSettings settings; EditState state; state.items = settings.value(ItemsKey).toList();
  const QByteArray bytes = nativeDialogTemplate(dialog, true);
  HWND parent = owner_ && owner_->handle() ? reinterpret_cast<HWND>(owner_->winId()) : nullptr;
  const INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATE>(bytes.constData()), parent, editProc, reinterpret_cast<LPARAM>(&state));
  if (state.font) DeleteObject(state.font);
  if (result != ImAway) return;
  if (state.save && !state.label.isEmpty()) {
    QVariantMap item{{"label", state.label}, {"417", state.label}, {"418", state.text}, {"418/rtf", state.rtf}};
    bool replaced = false; for (QVariant &value : state.items) if (value.toMap().value("label").toString().compare(state.label, Qt::CaseInsensitive) == 0) { value = item; replaced = true; }
    if (!replaced) state.items.append(item);
    settings.setValue(ItemsKey, state.items);
  }
  if (client_->setAway(state.text)) showCurrent(state.label, state.text);
#endif
}

QList<QPair<QString, QString>> AwayMessages::menuMessages() {
  auto split = [](const QString &stored) { const int open = stored.indexOf(QStringLiteral("<title>"), 0, Qt::CaseInsensitive), close = stored.indexOf(QStringLiteral("</title>"), 0, Qt::CaseInsensitive); if (open < 0 || close < open) return qMakePair(stored, stored); return qMakePair(stored.mid(open + 7, close - open - 7), stored.mid(close + 8)); }; // SplitOutAwayLabel
  QList<QPair<QString, QString>> result;
  for (const QVariant &value : QSettings().value(ItemsKey).toList()) { const QVariantMap item = value.toMap(); result.append({item.value(QStringLiteral("label")).toString(), item.value(QStringLiteral("418")).toString()}); }
  if (result.isEmpty()) result.append(split(aimEnvironment().string(281)));
  const auto game = split(aimEnvironment().string(910));
  if (std::none_of(result.begin(), result.end(), [&](const auto &m) { return m.first.compare(game.first, Qt::CaseInsensitive) == 0; })) result.append(game);
  return result;
}
void AwayMessages::useSaved(int index) {
  if (!client_ || !client_->connected()) return;
  const auto messages = menuMessages(); if (index < 0 || index >= messages.size()) return;
  const auto &[label, text] = messages[index];
  if (!text.trimmed().isEmpty() && client_->setAway(text)) showCurrent(label, text); // set immediately, no dialog (AwayProc n >= 1)
}

void AwayMessages::showCurrent(const QString &label, const QString &text) {
#ifdef Q_OS_WIN
  closeCurrent();
  const QJsonObject dialog = originalDialog(147); if (dialog.isEmpty()) return;
  const QByteArray bytes = nativeDialogTemplate(dialog, true);
  HWND window = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATE>(bytes.constData()), nullptr, currentProc, reinterpret_cast<LPARAM>(this));
  if (!window) return;
  HFONT font = nullptr; initFont(window, font); currentFont_ = font; current_ = window;
  createNativeControls(window, dialog, font);
  ShowWindow(GetDlgItem(window, SetNotice), SW_HIDE); ShowWindow(GetDlgItem(window, SetNoticeAccept), SW_HIDE);
  SetDlgItemTextW(window, CurrentLabel, reinterpret_cast<LPCWSTR>(label.utf16()));
  SetDlgItemTextW(window, CurrentText, reinterpret_cast<LPCWSTR>(text.utf16())); SendDlgItemMessageW(window, CurrentText, EM_SETREADONLY, TRUE, 0);
  ShowWindow(window, SW_SHOW);
  // I'm Back posts WM_APP so the dialog is torn down outside its own window procedure.
  struct Pump { static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) { if (message == WM_APP) { auto *self = reinterpret_cast<AwayMessages *>(data); QMetaObject::invokeMethod(self, [self] { if (self->client_) self->client_->clearAway(); self->closeCurrent(); }, Qt::QueuedConnection); return 0; } return DefSubclassProc(hwnd, message, wParam, lParam); } };
  SetWindowSubclass(window, Pump::proc, 1, reinterpret_cast<DWORD_PTR>(this));
#else
  Q_UNUSED(label); Q_UNUSED(text);
#endif
}

void AwayMessages::closeCurrent() {
#ifdef Q_OS_WIN
  if (current_ && IsWindow(static_cast<HWND>(current_))) DestroyWindow(static_cast<HWND>(current_));
  if (currentFont_) DeleteObject(static_cast<HFONT>(currentFont_));
  current_ = nullptr; currentFont_ = nullptr;
#endif
}
