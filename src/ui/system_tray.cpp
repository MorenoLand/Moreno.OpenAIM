#include "system_tray.h"
#include <QCoreApplication>
#include <QDebug>
#include <functional>
#ifdef Q_OS_WIN
#include <windowsx.h>
#include <algorithm>
#endif
SystemTray::SystemTray(QWindow *callbackWindow, QObject *parent) : QObject(parent), callbackWindow_(callbackWindow) {
#ifdef Q_OS_WIN
  taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
  icon_.cbSize = sizeof(icon_); icon_.uID = 1; icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP; icon_.uCallbackMessage = callbackMessage_;
  icon_.hIcon = reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(122), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
  if (!icon_.hIcon) qWarning() << "Original tray icon resource 122 could not be loaded" << GetLastError();
  QCoreApplication::instance()->installNativeEventFilter(this);
#endif
}
SystemTray::~SystemTray() {
  hide();
#ifdef Q_OS_WIN
  QCoreApplication::instance()->removeNativeEventFilter(this);
  if (icon_.hIcon) DestroyIcon(icon_.hIcon);
#endif
}
void SystemTray::setActions(const QList<Action> &actions) { actions_ = actions; }
void SystemTray::setOnline(bool online) { if(online_==online)return;online_=online;
#ifdef Q_OS_WIN
  HICON replacement=reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(online?120:122),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_DEFAULTCOLOR));if(!replacement)return;HICON previous=icon_.hIcon;icon_.hIcon=replacement;if(registered_){UINT flags=icon_.uFlags;icon_.uFlags=NIF_ICON;Shell_NotifyIconW(NIM_MODIFY,&icon_);icon_.uFlags=flags;}if(previous)DestroyIcon(previous);
#endif
}
void SystemTray::setToolTip(const QString &text) { tooltip_=text;
#ifdef Q_OS_WIN
  qsizetype length=std::min(text.size(),qsizetype(std::size(icon_.szTip)-1));for(qsizetype i=0;i<length;++i)icon_.szTip[i]=wchar_t(text[i].unicode());icon_.szTip[length]=0;if(registered_){UINT flags=icon_.uFlags;icon_.uFlags=NIF_TIP|NIF_SHOWTIP;Shell_NotifyIconW(NIM_MODIFY,&icon_);icon_.uFlags=flags;}
#endif
}
void SystemTray::show() {
  visible_ = true;
#ifdef Q_OS_WIN
  if (!registered_) registerIcon();
#endif
}
void SystemTray::hide() {
  visible_ = false;
#ifdef Q_OS_WIN
  if (registered_) Shell_NotifyIconW(NIM_DELETE, &icon_);
  registered_ = false; version4_ = false;
#endif
}
bool SystemTray::nativeEventFilter(const QByteArray &, void *message, qintptr *result) {
#ifdef Q_OS_WIN
  MSG *msg = static_cast<MSG *>(message);
  if (taskbarCreated_ && msg->message == taskbarCreated_) { registered_ = false; version4_ = false; if (visible_) registerIcon(); return false; }
  if (!registered_ || msg->hwnd != icon_.hWnd || msg->message != callbackMessage_) return false;
  UINT event = version4_ ? LOWORD(msg->lParam) : UINT(msg->lParam);
  if (version4_ && HIWORD(msg->lParam) != icon_.uID) return false;
  if (event == WM_CONTEXTMENU || (!version4_ && event == WM_RBUTTONUP)) { POINT position{}; if (version4_) { position.x = GET_X_LPARAM(msg->wParam); position.y = GET_Y_LPARAM(msg->wParam); } else GetCursorPos(&position); popup(position.x, position.y); }
  else if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK || (!version4_ && event == WM_LBUTTONUP)) emit activated();
  if(result)*result = 0; return true;
#else
  Q_UNUSED(message); Q_UNUSED(result); return false;
#endif
}
#ifdef Q_OS_WIN
void SystemTray::registerIcon() {
  if (!callbackWindow_ || !icon_.hIcon) return;
  icon_.hWnd = reinterpret_cast<HWND>(callbackWindow_->winId());
  QString tooltip = tooltip_.isEmpty()?QCoreApplication::applicationName():tooltip_; qsizetype length = std::min(tooltip.size(), qsizetype(sizeof(icon_.szTip) / sizeof(wchar_t) - 1));
  for (qsizetype i = 0; i < length; ++i) icon_.szTip[i] = wchar_t(tooltip[i].unicode()); icon_.szTip[length] = 0;
  registered_ = Shell_NotifyIconW(NIM_ADD, &icon_) != FALSE;
  if (!registered_) { qWarning() << "System tray registration failed" << GetLastError(); return; }
  icon_.uVersion = NOTIFYICON_VERSION_4; version4_ = Shell_NotifyIconW(NIM_SETVERSION, &icon_) != FALSE;
}
void SystemTray::popup(int x, int y) {
  if (actions_.isEmpty()) return;
  HMENU menu = CreatePopupMenu(); if (!menu) return;
  const QList<Action> actions = actions_;
  std::function<void(HMENU,const QList<Action>&)> append=[&](HMENU target,const QList<Action>&items){for(const Action &action:items){if(action.text.isEmpty())AppendMenuW(target,MF_SEPARATOR,0,nullptr);else if(!action.children.isEmpty()){HMENU submenu=CreatePopupMenu();append(submenu,action.children);AppendMenuW(target,MF_POPUP|MF_STRING|(action.enabled?MF_ENABLED:MF_GRAYED),reinterpret_cast<UINT_PTR>(submenu),reinterpret_cast<LPCWSTR>(action.text.utf16()));}else AppendMenuW(target,MF_STRING|(action.enabled?MF_ENABLED:MF_GRAYED),UINT_PTR(action.id),reinterpret_cast<LPCWSTR>(action.text.utf16()));}};
  append(menu,actions);
  if (x == -1 && y == -1) { POINT position{}; GetCursorPos(&position); x = position.x; y = position.y; }
  SetForegroundWindow(icon_.hWnd);
  UINT selected = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, x, y, icon_.hWnd, nullptr);
  PostMessageW(icon_.hWnd, WM_NULL, 0, 0); DestroyMenu(menu);
  if (selected) emit triggered(int(selected));
}
#endif
