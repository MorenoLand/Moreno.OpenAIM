#include "native_preferences.h"
#include "native_dialog.h"
#include "sounds.h"
#include "preferences.h"
#include <QHash>
#include <QColor>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSettings>
#include <QDebug>
#include <QNetworkProxy>
#include <QFontDatabase>
#ifdef Q_OS_WIN
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <shlobj.h>
#include <wincrypt.h>
#include <vector>
namespace {
constexpr int categories[] = {277,292,293,273,274,281,282,284,299,288,276,279,285,286,295};
QString key(int page, int control) { return QStringLiteral("nativePreferences/%1/%2").arg(page).arg(control); }
QString alias(int page, int control) {
  if (page == 294) { switch(control){case 431:return QStringLiteral("connection/host");case 433:return QStringLiteral("connection/port");case 435:return QStringLiteral("connection/proxyEnabled");case 438:return QStringLiteral("connection/proxyHost");case 440:return QStringLiteral("connection/proxyPort");case 447:return QStringLiteral("connection/proxyUser");case 449:return QStringLiteral("connection/proxyPassword");default:break;} }
  if (page != 293) return {};
  switch (control) { case 198: return QStringLiteral("account/savePassword"); case 199: return QStringLiteral("account/autoLogin"); case 208: return QStringLiteral("account/startWithWindows"); case 206: return QStringLiteral("connection/lan"); case 985: return QStringLiteral("preferences/autoUpgrade"); case 1117: return QStringLiteral("preferences/showToday"); case 47: return QStringLiteral("preferences/reconnect"); case 48: return QStringLiteral("preferences/showReconnectDialog"); default: return {}; }
}
QString windowText(HWND window) { int length = GetWindowTextLengthW(window); std::vector<wchar_t> text(size_t(length) + 1); GetWindowTextW(window, text.data(), length + 1); return QString::fromWCharArray(text.data()); }
struct RichStream { QByteArray bytes; qsizetype position = 0; };
DWORD CALLBACK writeRich(DWORD_PTR cookie, LPBYTE buffer, LONG size, LONG *written) { auto stream = reinterpret_cast<RichStream *>(cookie); stream->bytes.append(reinterpret_cast<const char *>(buffer),size); *written=size; return 0; }
DWORD CALLBACK readRich(DWORD_PTR cookie, LPBYTE buffer, LONG size, LONG *written) { auto stream = reinterpret_cast<RichStream *>(cookie); *written=LONG(qMin(qsizetype(size),stream->bytes.size()-stream->position)); memcpy(buffer,stream->bytes.constData()+stream->position,size_t(*written)); stream->position+=*written; return 0; }
void setText(HWND window, const QString &text) { wchar_t name[32]{};GetClassNameW(window,name,32);if(_wcsicmp(name,L"COMBOBOX")==0&&(GetWindowLongPtrW(window,GWL_STYLE)&3)==CBS_DROPDOWNLIST){LRESULT index=SendMessageW(window,CB_FINDSTRINGEXACT,-1,reinterpret_cast<LPARAM>(text.utf16()));SendMessageW(window,CB_SETCURSEL,index,0);}else SetWindowTextW(window, reinterpret_cast<LPCWSTR>(text.utf16())); }
QVariant readControl(HWND window){QString text=windowText(window);wchar_t name[32]{};GetClassNameW(window,name,32);if(_wcsicmp(name,L"EDIT")!=0||!(GetWindowLongPtrW(window,GWL_STYLE)&ES_PASSWORD))return text;QByteArray bytes=text.toUtf8();DATA_BLOB source{DWORD(bytes.size()),reinterpret_cast<BYTE *>(bytes.data())},encrypted{};if(!CryptProtectData(&source,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted)){qWarning()<<"Cannot protect preference password"<<GetLastError();return {};}QByteArray result(reinterpret_cast<const char *>(encrypted.pbData),int(encrypted.cbData));SecureZeroMemory(bytes.data(),size_t(bytes.size()));LocalFree(encrypted.pbData);return result;}
QString controlValue(HWND window,const QVariant &value){wchar_t name[32]{};GetClassNameW(window,name,32);if(_wcsicmp(name,L"EDIT")!=0||!(GetWindowLongPtrW(window,GWL_STYLE)&ES_PASSWORD))return value.toString();QByteArray bytes=value.toByteArray();if(bytes.isEmpty())return {};DATA_BLOB source{DWORD(bytes.size()),reinterpret_cast<BYTE *>(bytes.data())},plain{};if(!CryptUnprotectData(&source,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain)){qWarning()<<"Cannot decrypt preference password"<<GetLastError();return {};}QString text=QString::fromUtf8(reinterpret_cast<const char *>(plain.pbData),int(plain.cbData));SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);return text;}
QByteArray emptyTemplate(const QJsonObject &dialog, bool secondary) { return nativeDialogTemplate(dialog, secondary); }
QString controlClass(const QJsonValue &value) { return nativeControlClass(value); }
}
#endif
NativePreferences::NativePreferences(QWindow *owner, QObject *parent) : QObject(parent), owner_(owner) {
  QFile file(QStringLiteral(":/aim/dialogs.json")); if (file.open(QIODevice::ReadOnly)) inventory_ = QJsonDocument::fromJson(file.readAll()).object();
  QCoreApplication::instance()->installNativeEventFilter(this);
}
NativePreferences::~NativePreferences() { close(); QCoreApplication::instance()->removeNativeEventFilter(this); }
void NativePreferences::show() {
#ifdef Q_OS_WIN
  if (host_) { ShowWindow(host_, SW_SHOW); requestActivate(); return; }
  INITCOMMONCONTROLSEX init{sizeof(init), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_DATE_CLASSES}; InitCommonControlsEx(&init); LoadLibraryW(L"Msftedit.dll");
  QFile file(QStringLiteral(":/aim/dialogs/291")); if (!file.open(QIODevice::ReadOnly)) { qWarning() << "Missing original preferences dialog 291"; return; } hostBytes_ = file.readAll();
  host_ = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATE>(hostBytes_.constData()), owner_ ? reinterpret_cast<HWND>(owner_->winId()) : nullptr, hostProc, reinterpret_cast<LPARAM>(this));
  if (!host_) { qWarning() << "Preferences dialog creation failed" << GetLastError(); return; } ShowWindow(host_, SW_SHOW); requestActivate();
#endif
}
void NativePreferences::requestActivate() {
#ifdef Q_OS_WIN
  if (host_) SetForegroundWindow(host_);
#endif
}
void NativePreferences::activatePage(int id,int commandId) {
#ifdef Q_OS_WIN
  show();if(!host_)return;for(size_t i=0;i<std::size(categories);++i)if(categories[i]==id){SendDlgItemMessageW(host_,1131,LB_SETCURSEL,i,0);selectPage(id);if(commandId&&pages_.contains(id))command(pages_.value(id),commandId,BN_CLICKED);return;}
#else
  Q_UNUSED(id);Q_UNUSED(commandId);
#endif
}
void NativePreferences::close() {
#ifdef Q_OS_WIN
  bool hadWindow=host_!=nullptr;
  for (Page *page : secondary_) { if(IsWindow(page->window)) DestroyWindow(page->window); DeleteObject(page->font); delete page; } secondary_.clear();
  for (Page *page : pages_) { if(IsWindow(page->window)) DestroyWindow(page->window); DeleteObject(page->font); delete page; } pages_.clear();
  HWND host = host_; host_ = nullptr; if(IsWindow(host)) DestroyWindow(host); draft_.clear(); if(hadWindow)emit closed();
#endif
}
bool NativePreferences::nativeEventFilter(const QByteArray &, void *message, qintptr *result) {
#ifdef Q_OS_WIN
  MSG *msg = static_cast<MSG *>(message);
  if (msg->message >= WM_KEYFIRST && msg->message <= WM_KEYLAST) {
    for (Page *page : secondary_) if (IsDialogMessageW(page->window, msg)) { if(result)*result = 0; return true; }
    if (host_ && IsDialogMessageW(host_, msg)) { if(result)*result = 0; return true; }
  }
#else
  Q_UNUSED(message); Q_UNUSED(result);
#endif
  return false;
}
#ifdef Q_OS_WIN
INT_PTR CALLBACK NativePreferences::hostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  auto self = reinterpret_cast<NativePreferences *>(GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) { self = reinterpret_cast<NativePreferences *>(lParam); self->host_ = window; SetWindowLongPtrW(window, DWLP_USER, lParam);SetWindowTextW(window,L"AOL Instant Messenger (SM) Preferences");HICON icon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1));if(icon){SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(icon));SendMessageW(window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(icon));} for (int id : categories) { QString title = self->inventory_.value(QString::number(id)).toObject().value("title").toString(); SendDlgItemMessageW(window, 1131, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(title.utf16())); } SendDlgItemMessageW(window, 1131, LB_SETCURSEL, 2, 0); self->selectPage(293); return TRUE; }
  if (!self) return FALSE;
  if(message==WM_NCDESTROY&&self->host_==window){self->host_=nullptr;emit self->closed();return FALSE;}
  if (message == WM_DRAWITEM && wParam == 1131) { auto item = reinterpret_cast<DRAWITEMSTRUCT *>(lParam); if (item->itemID == UINT(-1)) return TRUE; wchar_t text[256]{}; SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, reinterpret_cast<LPARAM>(text)); bool selected = (item->itemState & ODS_SELECTED) != 0; FillRect(item->hDC, &item->rcItem, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW)); SetTextColor(item->hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT)); SetBkMode(item->hDC, TRANSPARENT); RECT r = item->rcItem; r.left += 2; DrawTextW(item->hDC, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX); if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC, &item->rcItem); return TRUE; }
  if (message == WM_CLOSE) { self->close(); return TRUE; }
  if (message == WM_COMMAND) { int id = LOWORD(wParam); if (id == 1131 && HIWORD(wParam) == LBN_SELCHANGE) { int index = int(SendDlgItemMessageW(window, 1131, LB_GETCURSEL, 0, 0)); if (index >= 0 && index < int(std::size(categories))) self->selectPage(categories[index]); return TRUE; } if (id == IDOK || id == 264) { self->apply(); if (id == IDOK) self->close(); return TRUE; } if (id == IDCANCEL) { self->close(); return TRUE; } }
  return FALSE;
}
NativePreferences::Page *NativePreferences::createPage(int id, bool secondary) {
  QJsonObject dialog = inventory_.value(QString::number(id)).toObject(); if (dialog.isEmpty()) return nullptr;
  auto page = new Page{this, id, nullptr, nullptr, secondary}; page->draftSnapshot=draft_;page->wasDirty=IsWindowEnabled(GetDlgItem(host_,264)); QByteArray bytes = emptyTemplate(dialog, secondary);
  page->window = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATE>(bytes.constData()), host_, pageProc, reinterpret_cast<LPARAM>(page));
  if (!page->window) { delete page; return nullptr; }
  if (secondary) { secondary_.append(page); EnableWindow(host_, FALSE); } else pages_.insert(id, page);
  ShowWindow(page->window, SW_SHOW); return page;
}
INT_PTR CALLBACK NativePreferences::pageProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  auto page = reinterpret_cast<Page *>(GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    page = reinterpret_cast<Page *>(lParam); page->window = window; SetWindowLongPtrW(window, DWLP_USER, lParam); auto self = page->self; QJsonObject dialog = self->inventory_.value(QString::number(page->id)).toObject();
    LOGFONTW actualFont{};HFONT dialogFont=reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0));if(dialogFont&&GetObjectW(dialogFont,sizeof(actualFont),&actualFont))page->font=CreateFontIndirectW(&actualFont);
    self->loading_ = true;
    for (const QJsonValue &value : dialog.value("controls").toArray()) { QJsonObject control = value.toObject(); QString name = controlClass(control.value("class")), text = control.value("text").toString(); RECT r{control.value("x").toInt(),control.value("y").toInt(),control.value("x").toInt()+control.value("width").toInt(),control.value("y").toInt()+control.value("height").toInt()}; MapDialogRect(window,&r); DWORD style = DWORD(control.value("style").toDouble()); if (control.value("class").toString() == "WndAte32Class") { style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL; text.clear(); } if (control.value("class").toString() == "_Oscar_UserListWnd") style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_LIST | LVS_SINGLESEL; if (control.value("class").toString() == "_Oscar_Tree") style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | TVS_HASLINES | TVS_HASBUTTONS;
      if(page->id==211&&control.value("id").toInt()==382){name="EDIT";style=WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL;}
      HWND child = CreateWindowExW(DWORD(control.value("exstyle").toDouble()),reinterpret_cast<LPCWSTR>(name.utf16()),reinterpret_cast<LPCWSTR>(text.utf16()),style,r.left,r.top,r.right-r.left,r.bottom-r.top,window,reinterpret_cast<HMENU>(INT_PTR(control.value("id").toInt())),GetModuleHandleW(nullptr),nullptr); if (!child) qWarning() << "Original preferences control failed" << page->id << control.value("id") << name << GetLastError(); else { SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(page->font),TRUE); if(name=="RICHEDIT50W")SendMessageW(child,EM_SETEVENTMASK,0,ENM_CHANGE); }
    }
    {static const QHash<int,QList<int>> soundCombos{{278,{127,131,1209}},{280,{972,976,980}},{283,{943}},{289,{112,116,120}},{290,{676,679}}};for(int combo:soundCombos.value(page->id))for(const QString &name:bundledSounds())SendDlgItemMessageW(window,combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.utf16()));} // the original lists the Sounds folderSendDlgItemMessageW(window,984,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(value.utf16()));SendDlgItemMessageW(window,984,CB_SETCURSEL,0,0);}
    if(page->id==277){for(const QString &family:QFontDatabase::families())SendDlgItemMessageW(window,232,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(family.utf16()));setText(GetDlgItem(window,232),QStringLiteral("Arial"));self->updateFontSizes(page);}
    self->restore(page); if(page->id==288)self->showComposeDefaults(page); if(page->id==294)self->updateConnection(page); if(page->id==292){self->refreshItems(page,245);self->refreshItems(page,247);}if(page->id==274)self->refreshItems(page,417);if(page->id==282)self->refreshItems(page,931);if(page->id==281)self->refreshItems(page,836);if(page->id==295)self->refreshItems(page,859); self->loading_ = false; return TRUE;
  }
  if (!page) return FALSE;
  if(message==WM_NCDESTROY){page->window=nullptr;return FALSE;}
  if(message==WM_DRAWITEM){auto item=reinterpret_cast<DRAWITEMSTRUCT *>(lParam);if((item->CtlType!=ODT_LISTBOX&&item->CtlType!=ODT_COMBOBOX)||item->itemID==UINT(-1))return FALSE;wchar_t text[2048]{};SendMessageW(item->hwndItem,item->CtlType==ODT_COMBOBOX?CB_GETLBTEXT:LB_GETTEXT,item->itemID,reinterpret_cast<LPARAM>(text));bool selected=(item->itemState&ODS_SELECTED)!=0;FillRect(item->hDC,&item->rcItem,GetSysColorBrush(selected?COLOR_HIGHLIGHT:COLOR_WINDOW));SetTextColor(item->hDC,GetSysColor(selected?COLOR_HIGHLIGHTTEXT:COLOR_WINDOWTEXT));SetBkMode(item->hDC,TRANSPARENT);RECT r=item->rcItem;r.left+=2;DrawTextW(item->hDC,text,-1,&r,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);return TRUE;}
  if (message == WM_CLOSE && page->secondary) { page->self->command(page,IDCANCEL,BN_CLICKED); return TRUE; }
  if (message == WM_COMMAND) { page->self->command(page,LOWORD(wParam),HIWORD(wParam)); return TRUE; }
  return FALSE;
}
void NativePreferences::selectPage(int id) {
  if (pages_.contains(selected_)) { capture(pages_.value(selected_)); ShowWindow(pages_.value(selected_)->window,SW_HIDE); }
  selected_ = id; Page *page = pages_.value(id,nullptr); if (!page) page = createPage(id,false); if (!page) return;
  RECT r{66,13,366,228}; MapDialogRect(host_,&r); SetWindowPos(page->window,nullptr,r.left,r.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_SHOWWINDOW); setText(GetDlgItem(host_,1132),inventory_.value(QString::number(id)).toObject().value("title").toString());
}
void NativePreferences::capture(Page *page) {
  if(page->id==274){int index=int(SendDlgItemMessageW(page->window,417,LB_GETCURSEL,0,0));QString k=key(274,417)+"/items";QSettings settings;QVariantList items=draft_.value(k,settings.value(k)).toList();if(index>=0&&index<items.size()){QVariantMap item=items[index].toMap();item["418"]=windowText(GetDlgItem(page->window,418));RichStream rich;EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&rich),0,writeRich};SendDlgItemMessageW(page->window,418,EM_STREAMOUT,SF_RTF,reinterpret_cast<LPARAM>(&stream));if(!stream.dwError)item["418/rtf"]=rich.bytes;items[index]=item;draft_[k]=items;}}
  if(page->id==294){bool enabled=SendDlgItemMessageW(page->window,435,BM_GETCHECK,0,0)==BST_CHECKED;if(!enabled)draft_[QStringLiteral("connection/proxyType")]=int(QNetworkProxy::NoProxy);else if(SendDlgItemMessageW(page->window,443,BM_GETCHECK,0,0)==BST_CHECKED){draft_[QStringLiteral("connection/proxyType")]=int(QNetworkProxy::Socks5Proxy);draft_[QStringLiteral("connection/proxyProtocol")]=QStringLiteral("socks5");}else if(SendDlgItemMessageW(page->window,444,BM_GETCHECK,0,0)==BST_CHECKED||SendDlgItemMessageW(page->window,1187,BM_GETCHECK,0,0)==BST_CHECKED){draft_[QStringLiteral("connection/proxyType")]=int(QNetworkProxy::HttpProxy);draft_[QStringLiteral("connection/proxyProtocol")]=SendDlgItemMessageW(page->window,444,BM_GETCHECK,0,0)==BST_CHECKED?QStringLiteral("https"):QStringLiteral("http");}else if(SendDlgItemMessageW(page->window,442,BM_GETCHECK,0,0)==BST_CHECKED){draft_[QStringLiteral("connection/proxyProtocol")]=QStringLiteral("socks4");qWarning()<<"Qt network transport does not provide SOCKS4 proxy support";}}
  for (const QJsonValue &value : inventory_.value(QString::number(page->id)).toObject().value("controls").toArray()) { QJsonObject c=value.toObject(); int id=c.value("id").toInt(); HWND child=GetDlgItem(page->window,id); if (!child || id==65535) continue; QString name=controlClass(c.value("class")); int type=int(c.value("style").toDouble())&15; if (name=="BUTTON" && (type==2||type==3||type==4||type==5||type==6||type==9)) draft_[key(page->id,id)]=int(SendMessageW(child,BM_GETCHECK,0,0)); else if (name=="EDIT"||name=="RICHEDIT50W"||name=="COMBOBOX") draft_[key(page->id,id)]=readControl(child); }
  for(const QJsonValue &value:inventory_.value(QString::number(page->id)).toObject().value("controls").toArray()){QJsonObject c=value.toObject();if(c.value("class").toString()!="WndAte32Class")continue;int id=c.value("id").toInt();RichStream rich;EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&rich),0,writeRich};SendDlgItemMessageW(page->window,id,EM_STREAMOUT,SF_RTF,reinterpret_cast<LPARAM>(&stream));if(!stream.dwError)draft_[key(page->id,id)+"/rtf"]=rich.bytes;}
}
void NativePreferences::restore(Page *page) {
  QSettings settings;
  for (const QJsonValue &value : inventory_.value(QString::number(page->id)).toObject().value("controls").toArray()) { QJsonObject c=value.toObject(); int id=c.value("id").toInt(); HWND child=GetDlgItem(page->window,id); QString k=key(page->id,id),a=alias(page->id,id); QVariant v=draft_.value(k,settings.value(k)); if (!v.isValid()&&!a.isEmpty()) v=settings.value(a); if (!v.isValid()&&page->id==293&&id==205) v=!settings.value("connection/lan",true).toBool(); if (!v.isValid()&&page->id==293&&(id==206||id==985||id==47||id==48)) v=true; if(!v.isValid()&&page->id==294){if(id==431)v=QStringLiteral("login.oscar.aol.com");if(id==433)v=QStringLiteral("5190");if(id==440)v=QStringLiteral("1080");if(id==435)v=false;if(id==442){bool selected=false;for(int protocol:{443,444,1187})selected=selected||draft_.value(key(294,protocol),settings.value(key(294,protocol))).toBool();v=!selected;}}if (!v.isValid()) continue; QString name=controlClass(c.value("class")); if (name=="BUTTON") SendMessageW(child,BM_SETCHECK,v.toInt(),0); else if (name=="EDIT"||name=="RICHEDIT50W"||name=="COMBOBOX") {setText(child,controlValue(child,v));if(page->id==277&&id==232)updateFontSizes(page);} }
  for(const QJsonValue &value:inventory_.value(QString::number(page->id)).toObject().value("controls").toArray()){QJsonObject c=value.toObject();if(c.value("class").toString()!="WndAte32Class")continue;int id=c.value("id").toInt();QString k=key(page->id,id)+"/rtf";RichStream rich{draft_.value(k,settings.value(k)).toByteArray()};if(rich.bytes.isEmpty())continue;EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&rich),0,readRich};SendDlgItemMessageW(page->window,id,EM_STREAMIN,SF_RTF,reinterpret_cast<LPARAM>(&stream));}
}
void NativePreferences::updateFontSizes(Page *page){QString family=windowText(GetDlgItem(page->window,232)),previous=windowText(GetDlgItem(page->window,233));QList<int> sizes=QFontDatabase::pointSizes(family);if(sizes.isEmpty())sizes=QFontDatabase::standardSizes();SendDlgItemMessageW(page->window,233,CB_RESETCONTENT,0,0);for(int size:sizes){QString text=QString::number(size);SendDlgItemMessageW(page->window,233,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.utf16()));}setText(GetDlgItem(page->window,233),previous.isEmpty()?QStringLiteral("9"):previous);}
void NativePreferences::updateConnection(Page *page){bool enabled=SendDlgItemMessageW(page->window,435,BM_GETCHECK,0,0)==BST_CHECKED;for(int id:{436,437,438,439,440,441,442,443,444,1187,445,446,447,448,449})EnableWindow(GetDlgItem(page->window,id),enabled);}
void NativePreferences::showComposeDefaults(Page *page){
  // Preview of "Defaults for Composing Windows" in ATE 352: sample text in the stored font, colour and window colour.
  QSettings settings; auto stored=[&](const char *name,const QVariant &fallback){const QString k=key(288,352)+"/"+name;return draft_.value(k,settings.value(k,fallback));};
  HWND editor=GetDlgItem(page->window,352); if(!editor)return;
  const QColor background(stored("background","#ffffff").toString()), color(stored("color","#000000").toString());
  SendMessageW(editor,EM_SETBKGNDCOLOR,0,RGB(background.red(),background.green(),background.blue()));
  if(GetWindowTextLengthW(editor)==0)SetWindowTextW(editor,L"AaBbYyZz");
  CHARFORMAT2W format{};format.cbSize=sizeof(format);format.dwMask=CFM_FACE|CFM_SIZE|CFM_BOLD|CFM_ITALIC|CFM_UNDERLINE|CFM_COLOR;
  format.yHeight=LONG(stored("points",12).toReal()*20);format.crTextColor=RGB(color.red(),color.green(),color.blue());
  format.dwEffects=(stored("bold",false).toBool()?CFE_BOLD:0)|(stored("italic",false).toBool()?CFE_ITALIC:0)|(stored("underline",false).toBool()?CFE_UNDERLINE:0);
  wcsncpy_s(format.szFaceName,reinterpret_cast<const wchar_t *>(stored("face","Times New Roman").toString().utf16()),_TRUNCATE);
  SendMessageW(editor,EM_SETCHARFORMAT,SCF_ALL,reinterpret_cast<LPARAM>(&format));
}
void NativePreferences::dirty() { if (!loading_) EnableWindow(GetDlgItem(host_,264),TRUE); }
void NativePreferences::refreshItems(Page *page,int list){QSettings settings;QString k=key(page->id,list)+"/items";QVariantList items=draft_.value(k,settings.value(k)).toList();HWND control=GetDlgItem(page->window,list);bool listview=page->id==292;SendMessageW(control,listview?LVM_DELETEALLITEMS:LB_RESETCONTENT,0,0);for(int i=0;i<items.size();++i){QVariantMap item=items[i].toMap();QString label=item.value("label").toString();if(listview){LVITEMW value{};value.mask=LVIF_TEXT;value.iItem=i;value.pszText=const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(label.utf16()));SendMessageW(control,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&value));}else SendMessageW(control,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.utf16()));}}
void NativePreferences::removeItem(Page *page,int list){HWND control=GetDlgItem(page->window,list);int index=page->id==292?int(SendMessageW(control,LVM_GETNEXTITEM,-1,LVNI_SELECTED)):int(SendMessageW(control,LB_GETCURSEL,0,0));QSettings settings;QString k=key(page->id,list)+"/items";QVariantList items=draft_.value(k,settings.value(k)).toList();if(index<0||index>=items.size())return;items.removeAt(index);draft_[k]=items;refreshItems(page,list);dirty();}
void NativePreferences::editItem(Page *page,int list,int dialog,bool edit){HWND control=GetDlgItem(page->window,list);int index=edit?int(SendMessageW(control,LB_GETCURSEL,0,0)):-1;QSettings settings;QString k=key(page->id,list)+"/items";QVariantList items=draft_.value(k,settings.value(k)).toList();if(edit&&(index<0||index>=items.size()))return;Page *editor=createPage(dialog,true);if(!editor)return;editor->collectionPage=page->id;editor->collectionControl=list;editor->collectionIndex=index;QVariantMap item=index>=0?items[index].toMap():QVariantMap();loading_=true;for(const QJsonValue &value:inventory_.value(QString::number(dialog)).toObject().value("controls").toArray()){QJsonObject c=value.toObject();int id=c.value("id").toInt();QString name=controlClass(c.value("class"));if(dialog==211&&id==382)name="EDIT";HWND child=GetDlgItem(editor->window,id);if(name=="EDIT"||name=="RICHEDIT50W"||name=="COMBOBOX")setText(child,controlValue(child,item.value(QString::number(id))));else if(name=="BUTTON"&&((int(c.value("style").toDouble())&15)==3))SendMessageW(child,BM_SETCHECK,item.value(QString::number(id)).toInt(),0);}for(const QJsonValue &value:inventory_.value(QString::number(dialog)).toObject().value("controls").toArray()){QJsonObject c=value.toObject();if(c.value("class").toString()!="WndAte32Class")continue;int id=c.value("id").toInt();RichStream rich{item.value(QString::number(id)+"/rtf").toByteArray()};if(!rich.bytes.isEmpty()){EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&rich),0,readRich};SendDlgItemMessageW(editor->window,id,EM_STREAMIN,SF_RTF,reinterpret_cast<LPARAM>(&stream));}}loading_=false;}
void NativePreferences::apply() { for(Page *page:pages_) capture(page); QSettings settings; for(auto it=draft_.cbegin();it!=draft_.cend();++it) { settings.setValue(it.key(),it.value()); QStringList parts=it.key().split('/'); QString a=parts.size()==3?alias(parts.value(1).toInt(),parts.value(2).toInt()):QString(); if(!a.isEmpty()) settings.setValue(a,it.value()); } settings.sync(); prefs::invalidate(); EnableWindow(GetDlgItem(host_,264),FALSE); emit applied(); }
void NativePreferences::command(Page *page,int id,int notification) {
  if (loading_) return;
  bool accept=id==IDOK||(page->id==275&&id==427)||((page->id==241||page->id==247)&&id==883);
  if (page->secondary&&(accept||id==IDCANCEL)) { if(accept){if(page->collectionPage){QVariantMap item;for(const QJsonValue &value:inventory_.value(QString::number(page->id)).toObject().value("controls").toArray()){QJsonObject c=value.toObject();int control=c.value("id").toInt();QString name=controlClass(c.value("class"));if(page->id==211&&control==382)name="EDIT";HWND child=GetDlgItem(page->window,control);if(name=="EDIT"||name=="RICHEDIT50W"||name=="COMBOBOX")item[QString::number(control)]=readControl(child);else if(name=="BUTTON"&&((int(c.value("style").toDouble())&15)==3))item[QString::number(control)]=int(SendMessageW(child,BM_GETCHECK,0,0));}for(const QJsonValue &value:inventory_.value(QString::number(page->id)).toObject().value("controls").toArray()){QJsonObject c=value.toObject();if(c.value("class").toString()!="WndAte32Class")continue;int control=c.value("id").toInt();RichStream rich;EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&rich),0,writeRich};SendDlgItemMessageW(page->window,control,EM_STREAMOUT,SF_RTF,reinterpret_cast<LPARAM>(&stream));if(!stream.dwError)item[QString::number(control)+"/rtf"]=rich.bytes;}int label=page->id==211?382:page->id==275?417:page->id==247?938:873;QString text=item.value(QString::number(label)).toString().trimmed();if(text.isEmpty()){SetFocus(GetDlgItem(page->window,label));return;}item["label"]=text;QSettings settings;QString k=key(page->collectionPage,page->collectionControl)+"/items";QVariantList items=draft_.value(k,settings.value(k)).toList();if(page->collectionIndex>=0&&page->collectionIndex<items.size())items[page->collectionIndex]=item;else items.append(item);draft_[k]=items;refreshItems(pages_.value(page->collectionPage),page->collectionControl);}else capture(page);dirty();} if(!accept){draft_=page->draftSnapshot;EnableWindow(GetDlgItem(host_,264),page->wasDirty);} secondary_.removeOne(page); DestroyWindow(page->window); DeleteObject(page->font); delete page; EnableWindow(host_,secondary_.isEmpty()); SetForegroundWindow(secondary_.isEmpty()?host_:secondary_.last()->window); return; }
  if(page->id==277&&id==232&&notification==CBN_SELCHANGE){loading_=true;updateFontSizes(page);loading_=false;dirty();return;}
  if(notification==LBN_SELCHANGE&&((page->id==274&&id==417)||(page->id==295&&id==859))){int index=int(SendDlgItemMessageW(page->window,id,LB_GETCURSEL,0,0));QSettings settings;QString k=key(page->id,id)+"/items";QVariantList items=draft_.value(k,settings.value(k)).toList();if(index>=0&&index<items.size()){loading_=true;QVariantMap item=items[index].toMap();setText(GetDlgItem(page->window,page->id==274?418:884),item.value(page->id==274?"418":"860").toString());if(page->id==274){RichStream rich{item.value("418/rtf").toByteArray()};if(!rich.bytes.isEmpty()){EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&rich),0,readRich};SendDlgItemMessageW(page->window,418,EM_STREAMIN,SF_RTF,reinterpret_cast<LPARAM>(&stream));}}loading_=false;}return;}
  if (notification==EN_CHANGE||notification==CBN_SELCHANGE||notification==CBN_EDITCHANGE||notification==LBN_SELCHANGE) { dirty(); return; }
  if (notification!=BN_CLICKED) return;
  if(page->id==294&&id==435){updateConnection(page);dirty();return;}
  if(page->id==294&&id==452){loading_=true;setText(GetDlgItem(page->window,431),QStringLiteral("login.oscar.aol.com"));setText(GetDlgItem(page->window,433),QStringLiteral("5190"));setText(GetDlgItem(page->window,440),QStringLiteral("1080"));SendDlgItemMessageW(page->window,435,BM_SETCHECK,BST_UNCHECKED,0);CheckRadioButton(page->window,442,444,442);SendDlgItemMessageW(page->window,1187,BM_SETCHECK,BST_UNCHECKED,0);updateConnection(page);loading_=false;dirty();return;}
  int target=0;
  if(page->id==293&&id==1123) target=294; if(page->id==277&&id==1125) target=278; if(page->id==279&&id==1130) target=280; if(page->id==282&&id==1126) target=283; if(page->id==288&&id==1128) target=289; if(page->id==288&&id==1129) target=290; if(page->id==286&&id==1127) target=287;
  if(target){createPage(target,true);return;}
  if(page->id==292){if(id==248||id==250){editItem(page,id==248?245:247,211,false);return;}if(id==249||id==251){removeItem(page,id==249?245:247);return;}}
  if(page->id==274){if(id==419||id==497){editItem(page,417,275,id==497);return;}if(id==420){removeItem(page,417);return;}}
  if(page->id==282){if(id==928||id==929){editItem(page,931,247,id==929);return;}if(id==932){removeItem(page,931);return;}}
  if(page->id==295){if(id==861||id==886){editItem(page,859,241,id==886);return;}if(id==887){removeItem(page,859);return;}if(id==885){QString text=windowText(GetDlgItem(page->window,884));if(OpenClipboard(page->window)){EmptyClipboard();SIZE_T size=SIZE_T(text.size()+1)*sizeof(wchar_t);HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,size);if(memory){void *buffer=GlobalLock(memory);if(buffer){memcpy(buffer,text.utf16(),size);GlobalUnlock(memory);if(!SetClipboardData(CF_UNICODETEXT,memory))GlobalFree(memory);}else GlobalFree(memory);}CloseClipboard();}return;}}
  if((page->id==285&&id==774)||(page->id==286&&id==776)){HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);BROWSEINFOW browser{};browser.hwndOwner=page->window;browser.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;PIDLIST_ABSOLUTE folder=SHBrowseForFolderW(&browser);if(folder){wchar_t path[MAX_PATH]{};if(SHGetPathFromIDListW(folder,path)){setText(GetDlgItem(page->window,page->id==285?773:775),QString::fromWCharArray(path));dirty();}CoTaskMemFree(folder);}if(SUCCEEDED(initialized))CoUninitialize();return;}
  if((page->id==281&&id==839)||(page->id==241&&id==788)||(page->id==247&&id==949)){wchar_t path[32768]{};OPENFILENAMEW chooser{sizeof(chooser)};chooser.hwndOwner=page->window;chooser.lpstrFile=path;chooser.nMaxFile=DWORD(std::size(path));chooser.lpstrFilter=page->id==281?L"Images\0*.bmp;*.gif;*.jpg;*.jpeg;*.png;*.ico\0All files\0*.*\0":L"Programs (*.exe)\0*.exe\0All files\0*.*\0";chooser.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;if(GetOpenFileNameW(&chooser)){QString selected=QString::fromWCharArray(path);if(page->id==281){QString k=key(281,836)+"/items";QSettings settings;QVariantList items=draft_.value(k,settings.value(k)).toList();QVariantMap item;item["label"]=selected;item["path"]=selected;int index=int(SendDlgItemMessageW(page->window,836,LB_GETCURSEL,0,0));if(index>=0&&index<items.size())items[index]=item;else items.append(item);draft_[k]=items;refreshItems(page,836);}else setText(GetDlgItem(page->window,page->id==241?874:948),selected);dirty();}return;}
  if(page->id==281&&(id==840||id==841)){if(id==840)removeItem(page,836);else{draft_[key(281,836)+"/items"]=QVariantList();refreshItems(page,836);dirty();}return;}
  if(page->id==284&&id==85){Page *editor=createPage(3,true);if(editor){editor->collectionPage=284;editor->collectionControl=85;QSettings settings;QString k=key(284,85)+"/items";QVariantList items=draft_.value(k,settings.value(k)).toList();for(int i=0;i<items.size();++i){QString label=items[i].toMap().value("label").toString();LVITEMW value{};value.mask=LVIF_TEXT;value.iItem=i;value.pszText=const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(label.utf16()));SendDlgItemMessageW(editor->window,74,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&value));}}return;}
  if(page->id==3&&id==81){QString symbol=windowText(GetDlgItem(page->window,86)).trimmed();if(symbol.isEmpty()){SetFocus(GetDlgItem(page->window,86));return;}QSettings settings;QString k=key(284,85)+"/items";QVariantList items=draft_.value(k,settings.value(k)).toList();QVariantMap item;item["label"]=symbol;items.append(item);draft_[k]=items;LVITEMW value{};value.mask=LVIF_TEXT;value.iItem=items.size()-1;value.pszText=const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(symbol.utf16()));SendDlgItemMessageW(page->window,74,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&value));dirty();return;}
  int soundControl = 0; bool preview = false;
  if(page->id==278){if(id==128||id==129){soundControl=127;preview=id==129;}if(id==132||id==133){soundControl=131;preview=id==133;}if(id==1210||id==1211){soundControl=1209;preview=id==1211;}}
  if(page->id==280){if(id==973||id==974){soundControl=972;preview=id==974;}if(id==977||id==978){soundControl=976;preview=id==978;}if(id==981||id==982){soundControl=980;preview=id==982;}}
  if(page->id==283&&(id==942||id==944)){soundControl=943;preview=id==944;}
  if(page->id==289){if(id==113||id==114){soundControl=112;preview=id==114;}if(id==117||id==118){soundControl=116;preview=id==118;}if(id==121||id==122){soundControl=120;preview=id==122;}}
  if(page->id==290){if(id==678||id==677){soundControl=676;preview=id==677;}if(id==682||id==681){soundControl=679;preview=id==681;}}
  if(soundControl){if(preview){using Play=BOOL(WINAPI *)(LPCWSTR,HMODULE,DWORD); static HMODULE module=LoadLibraryW(L"winmm.dll"); auto play=module?reinterpret_cast<Play>(GetProcAddress(module,"PlaySoundW")):nullptr;QString path=windowText(GetDlgItem(page->window,soundControl));Q_UNUSED(play);playSoundFile(path);}else{wchar_t path[32768]{};OPENFILENAMEW chooser{sizeof(chooser)};chooser.hwndOwner=page->window;chooser.lpstrFile=path;chooser.nMaxFile=DWORD(std::size(path));chooser.lpstrFilter=L"Wave audio (*.wav)\0*.wav\0All files\0*.*\0";chooser.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;if(GetOpenFileNameW(&chooser)){setText(GetDlgItem(page->window,soundControl),QString::fromWCharArray(path));dirty();}}return;}
  if(page->id==287&&id==758){wchar_t path[32768]{};OPENFILENAMEW chooser{sizeof(chooser)};chooser.hwndOwner=page->window;chooser.lpstrFile=path;chooser.nMaxFile=DWORD(std::size(path));chooser.lpstrFilter=L"Programs (*.exe)\0*.exe\0All files\0*.*\0";chooser.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;if(GetOpenFileNameW(&chooser)){setText(GetDlgItem(page->window,759),QString::fromWCharArray(path));dirty();}return;}
  if(page->id==288&&(id==350||id==1137||id==351)) { HWND editor=GetDlgItem(page->window,352); CHARFORMAT2W format{};format.cbSize=sizeof(format);SendMessageW(editor,EM_GETCHARFORMAT,SCF_SELECTION,reinterpret_cast<LPARAM>(&format)); if(id==350){LOGFONTW font{};font.lfHeight=-MulDiv(format.yHeight,96,1440);wcscpy_s(font.lfFaceName,format.szFaceName);CHOOSEFONTW chooser{sizeof(chooser)};chooser.hwndOwner=page->window;chooser.lpLogFont=&font;chooser.Flags=CF_SCREENFONTS|CF_INITTOLOGFONTSTRUCT|CF_EFFECTS;chooser.rgbColors=format.crTextColor;if(ChooseFontW(&chooser)){format.dwMask=CFM_FACE|CFM_SIZE|CFM_BOLD|CFM_ITALIC|CFM_UNDERLINE|CFM_COLOR;format.yHeight=chooser.iPointSize*2;format.crTextColor=chooser.rgbColors;format.dwEffects=(font.lfWeight>=FW_BOLD?CFE_BOLD:0)|(font.lfItalic?CFE_ITALIC:0)|(font.lfUnderline?CFE_UNDERLINE:0);wcscpy_s(format.szFaceName,font.lfFaceName);SendMessageW(editor,EM_SETCHARFORMAT,SCF_ALL,reinterpret_cast<LPARAM>(&format));const QString base=key(288,352);draft_[base+"/face"]=QString::fromWCharArray(font.lfFaceName);draft_[base+"/points"]=chooser.iPointSize/10.0;draft_[base+"/bold"]=font.lfWeight>=FW_BOLD;draft_[base+"/italic"]=bool(font.lfItalic);draft_[base+"/underline"]=bool(font.lfUnderline);draft_[base+"/color"]=QColor(GetRValue(chooser.rgbColors),GetGValue(chooser.rgbColors),GetBValue(chooser.rgbColors)).name();dirty();}}else{static COLORREF custom[16]{};CHOOSECOLORW chooser{sizeof(chooser)};chooser.hwndOwner=page->window;chooser.lpCustColors=custom;chooser.rgbResult=format.crTextColor;if(id==351){QColor c(draft_.value(key(288,352)+"/background",QSettings().value(key(288,352)+"/background","#ffffff")).toString());chooser.rgbResult=RGB(c.red(),c.green(),c.blue());}chooser.Flags=CC_FULLOPEN|CC_RGBINIT;if(ChooseColorW(&chooser)){const QString name=QColor(GetRValue(chooser.rgbResult),GetGValue(chooser.rgbResult),GetBValue(chooser.rgbResult)).name();if(id==351){SendMessageW(editor,EM_SETBKGNDCOLOR,0,chooser.rgbResult);draft_[key(288,352)+"/background"]=name;}else{format.dwMask=CFM_COLOR;format.crTextColor=chooser.rgbResult;SendMessageW(editor,EM_SETCHARFORMAT,SCF_ALL,reinterpret_cast<LPARAM>(&format));draft_[key(288,352)+"/color"]=name;}dirty();}}return; }
  HWND child=GetDlgItem(page->window,id); int type=int(GetWindowLongPtrW(child,GWL_STYLE))&15; if(type==2||type==3||type==4||type==5||type==6||type==9){dirty();return;}
  qWarning()<<"Original preferences handler not yet mapped"<<page->id<<id;
}
#endif
