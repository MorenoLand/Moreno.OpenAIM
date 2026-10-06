#include "sign_on_window.h"
#include "ate_link.h"
#include "buddy_list_window.h"
#include "preferences_window.h"
#include "art.h"
#include "ctl_group.h"
#include "gdi_text.h"
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QTimer>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <vector>
#include <wincrypt.h>
#endif

namespace {
// Saved passwords are kept per screen name. OSCAR's weak MD5 login (AIM 3.5-4.7) needs the plain password, so it is
// stored encrypted for the current Windows user with DPAPI; other platforms do not persist it yet.
QString passwordKey(const QString &screenName) { QString name = screenName; name.remove(QLatin1Char(' ')); return QStringLiteral("accounts/%1/password").arg(name.toCaseFolded()); }
QString loadSavedPassword(QSettings &settings, const QString &screenName) {
#ifdef Q_OS_WIN
  QByteArray bytes = settings.value(passwordKey(screenName)).toByteArray(); if (screenName.trimmed().isEmpty() || bytes.isEmpty()) return {};
  DATA_BLOB source{DWORD(bytes.size()), reinterpret_cast<BYTE *>(bytes.data())}, plain{};
  if (!CryptUnprotectData(&source, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) return {};
  QString text = QString::fromUtf8(reinterpret_cast<const char *>(plain.pbData), int(plain.cbData)); SecureZeroMemory(plain.pbData, plain.cbData); LocalFree(plain.pbData); return text;
#else
  Q_UNUSED(settings); Q_UNUSED(screenName); return {};
#endif
}
void storeSavedPassword(QSettings &settings, const QString &screenName, const QString &password) {
#ifdef Q_OS_WIN
  if (screenName.trimmed().isEmpty()) return;
  QByteArray bytes = password.toUtf8(); DATA_BLOB source{DWORD(bytes.size()), reinterpret_cast<BYTE *>(bytes.data())}, encrypted{};
  if (CryptProtectData(&source, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) { settings.setValue(passwordKey(screenName), QByteArray(reinterpret_cast<const char *>(encrypted.pbData), int(encrypted.cbData))); LocalFree(encrypted.pbData); }
  SecureZeroMemory(bytes.data(), size_t(bytes.size()));
#else
  Q_UNUSED(settings); Q_UNUSED(screenName); Q_UNUSED(password);
#endif
}
#ifdef Q_OS_WIN
LRESULT CALLBACK signOnSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
  if (message == WM_COMMAND && lParam) reinterpret_cast<SignOnWindow *>(data)->nativeCommand(LOWORD(wParam), HIWORD(wParam));
  return DefSubclassProc(window, message, wParam, lParam);
}
#endif
void forgetSavedPassword(QSettings &settings, const QString &screenName) { if (!screenName.trimmed().isEmpty()) settings.remove(passwordKey(screenName)); }
}
SignOnWindow::SignOnWindow(OscarClient *client) : WindowBase(QStringLiteral("Sign On"), QSize(212, 378)), client_(client), logo_(QStringLiteral(":/aim/signon.gif")), helpIcon_(transparentBitmap(QStringLiteral(":/aim/signon-help.bmp"),0x00ccccccu)), setupIcon_(transparentBitmap(QStringLiteral(":/aim/setup-wrench.bmp"))), signOnIcon_(transparentBitmap(QStringLiteral(":/aim/signon-button.bmp"))) {
  screenName_ = settings_.value(QStringLiteral("account/screenName")).toString();
  screenNameLabel_ = transparentBitmap(QStringLiteral(":/aim/screen-name.bmp"));
  form_ = loadCtlGroup(111); connecting_ = loadCtlGroup(119); // osclogin.ocm: full form / connecting state
  setCanvasSize(formCanvas(false));
  const int buttonIds[3][3]={{128,131,134},{127,130,133},{126,129,132}};
  for(int button=0;button<3;++button)for(int state=0;state<3;++state)buttonStates_[button][state]=transparentBitmap(QStringLiteral(":/aim/button-%1.bmp").arg(buttonIds[button][state]));
  host_ = settings_.value(QStringLiteral("connection/host"),QStringLiteral("login.oscar.aol.com")).toString();
  port_ = quint16(settings_.value(QStringLiteral("connection/port"), 5190).toUInt());
  savePassword_ = settings_.value(QStringLiteral("account/savePassword"), false).toBool();
  autoLogin_ = settings_.value(QStringLiteral("account/autoLogin"), false).toBool();
  if (savePassword_) password_ = loadSavedPassword(settings_, screenName_);
  QTimer::singleShot(0, this, [this] { if (autoLogin_ && savePassword_ && !screenName_.isEmpty() && !password_.isEmpty()) signOn(); });
#ifdef Q_OS_WIN
  QGuiApplication::instance()->installNativeEventFilter(this);
#endif
  connect(&caretTimer_, &QTimer::timeout, this, [this] { caretVisible_ = !caretVisible_; renderNow(); });
  caretTimer_.start(500);
  connect(client_, &OscarClient::statusChanged, this, [this](const QString &status) { status_ = status; renderNow(); });
  connect(client_, &OscarClient::failed, this, [this](const QString &reason) { setLoginStage(0);status_ = reason; renderNow(); });
  connect(client_, &OscarClient::loginStageChanged, this, &SignOnWindow::setLoginStage);
  connect(client_, &OscarClient::rosterReady, this, [this] { if (savePassword_) storeSavedPassword(settings_, screenName_, password_); else { forgetSavedPassword(settings_, screenName_); password_.fill(QChar(0)); password_.clear(); } if(preferencesWindow_)preferencesWindow_->close(); if(buddyWindow_)buddyWindow_->deleteLater();buddyWindow_ = new BuddyListWindow(client_);buddyWindow_->QObject::setParent(this); /* top-level and unowned like the original, so it appears in the taskbar */connect(buddyWindow_,&BuddyListWindow::actionRequested,this,[this](int id,const QString &name){if(id==20002||id==174)showPreferences();else if(id==745||id==190)signOffFromTray(); /* Sign Off / Switch Screen Name: back to the Sign On window */else emit actionRequested(id,name);}); connect(buddyWindow_,&BuddyListWindow::exitAccepted,qApp,&QCoreApplication::quit); buddyWindow_->show(); hide(); });
  if (QScreen *screen = QGuiApplication::primaryScreen()) setPosition(screen->availableGeometry().center() - QPoint(width() / 2, height() / 2));
}
void SignOnWindow::showClient() { QWindow *target=buddyWindow_&&client_->connected()?static_cast<QWindow*>(buddyWindow_.data()):this;target->showNormal();target->raise();target->requestActivate();target->requestUpdate(); }
void SignOnWindow::signOffFromTray() { if(buddyWindow_)buddyWindow_->hide();client_->signOff();show();requestActivate(); }
void SignOnWindow::exitFromTray() { if(buddyWindow_&&client_->connected()){buddyWindow_->show();buddyWindow_->requestExit();}else{
#ifdef Q_OS_WIN
  bool suppress=false;if(settings_.value(QStringLiteral("preferences/confirmExit"),true).toBool()&&!confirmExit(suppress))return;if(suppress)settings_.setValue(QStringLiteral("preferences/confirmExit"),false);
#endif
  client_->signOff();QCoreApplication::quit();} }
SignOnWindow::~SignOnWindow() {
#ifdef Q_OS_WIN
  QGuiApplication::instance()->removeNativeEventFilter(this);
  if (nativeName_ && handle()) RemoveWindowSubclass(reinterpret_cast<HWND>(winId()), signOnSubclass, 1);
  for(void *control : {nativeName_,nativePassword_,nativeSave_,nativeAuto_,nativeCancel_}) if(control&&IsWindow(static_cast<HWND>(control))) DestroyWindow(static_cast<HWND>(control));
  for (void *font : {nativeFont_, nativeFontEdit_, nativeFontCombo_}) if (font) DeleteObject(static_cast<HFONT>(font));
#endif
}
bool SignOnWindow::event(QEvent *event) {
  if (event->type() == QEvent::Expose && isExposed()) initializeNativeControls();
  return WindowBase::event(event);
}
QList<CtlObject *> SignOnWindow::formObjects(bool connecting) {
  QList<CtlObject *> objects; CtlObject *root = (connecting ? connecting_ : form_).get(); if (!root) return objects;
#ifdef Q_OS_WIN
  const QRect client(1, TitleBarHeight, width(), height());
#else
  const QRect client(0, TitleBarHeight, width(), height() - TitleBarHeight);
#endif
  ctlMove(*root, client, aimEnvironment());
  std::function<void(CtlObject &)> collect = [&](CtlObject &o) { objects.append(&o); for (const auto &child : o.children) collect(*child); };
  collect(*root); return objects;
}
QRect SignOnWindow::formRect(quint32 id, bool connecting, int occurrence) {
  for (CtlObject *o : formObjects(connecting)) if (o->id == id && o->kind != CtlObject::Kind::Group && occurrence-- == 0) return o->windowRect();
  return {};
}
QSize SignOnWindow::formCanvas(bool connecting) const {
  // osclogin sizes the window from CtlGroupGetIdealSize of CTLGROUP 111 (119 while connecting). On the reference
  // screenshot the client area is 10x19 px larger than that ideal size (inferred offset; its source is not traced).
  const auto &group = connecting ? connecting_ : form_;
  const QSize ideal = group ? ctlIdealSize(*group, aimEnvironment()) : QSize(200, 324);
  return ideal + QSize(10, 19) + QSize(2, TitleBarHeight + 3);
}
int SignOnWindow::actionAt(const QPoint &point) {
  if (loginStage_) return 0;
  if (formRect(962).contains(point)) return 1;   // Help
  if (formRect(963).contains(point)) return 2;   // Setup
  if (formRect(964).contains(point)) return 3;   // Sign On
  if (formRect(1052).contains(point)) return 4;  // Forgot Password?
  return 0;
}
void SignOnWindow::initializeNativeControls() {
#ifdef Q_OS_WIN
  if (nativeName_) return;
  HWND owner = reinterpret_cast<HWND>(winId());
  SetWindowLongPtrW(owner, GWL_STYLE, GetWindowLongPtrW(owner, GWL_STYLE) | WS_CLIPCHILDREN);
  // Fonts of the CTLGROUP 111 records: FONTDESC 108 (password, check boxes) and 112 (screen-name combo), Arial -11.
  auto fontFor = [](quint32 id) { const GdiFont f = GdiFont::fromFontDesc(id); LOGFONTW logical{}; logical.lfHeight = f.height; logical.lfWeight = f.weight; logical.lfCharSet = DEFAULT_CHARSET; wcsncpy_s(logical.lfFaceName, f.face.toStdWString().c_str(), _TRUNCATE); return CreateFontIndirectW(&logical); };
  nativeFontEdit_ = fontFor(108); nativeFontCombo_ = fontFor(112);
  HDC dc = GetDC(owner); nativeFont_ = CreateFontW(-MulDiv(8, GetDeviceCaps(dc, LOGPIXELSY), 72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,0,0,L"MS Sans Serif"); ReleaseDC(owner,dc);
  auto windowRect = [this](const QRect &canvas) { return canvas.translated(-1, -TitleBarHeight); };
  const QRect name = windowRect(formRect(958)), password = windowRect(formRect(959)), save = windowRect(formRect(960)), autoLogin = windowRect(formRect(961)), cancel = windowRect(formRect(193, true));
  nativeName_ = CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWN|CBS_AUTOHSCROLL,name.x(),name.y(),name.width(),120,owner,reinterpret_cast<HMENU>(195),GetModuleHandleW(nullptr),nullptr);
  nativePassword_ = CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_PASSWORD|ES_AUTOHSCROLL,password.x(),password.y(),password.width(),password.height(),owner,reinterpret_cast<HMENU>(197),GetModuleHandleW(nullptr),nullptr);
  // The original edit is not visual-styled (classic sunken client edge, reference screenshot).
  using SetTheme = HRESULT(WINAPI *)(HWND, LPCWSTR, LPCWSTR); static HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll"); if (auto setTheme = uxtheme ? reinterpret_cast<SetTheme>(GetProcAddress(uxtheme, "SetWindowTheme")) : nullptr) setTheme(static_cast<HWND>(nativePassword_), L"", L"");
  nativeSave_ = CreateWindowExW(0,L"BUTTON",reinterpret_cast<LPCWSTR>(aimEnvironment().string(874).utf16()),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,save.x(),save.y(),save.width(),save.height(),owner,reinterpret_cast<HMENU>(198),GetModuleHandleW(nullptr),nullptr);
  nativeAuto_ = CreateWindowExW(0,L"BUTTON",reinterpret_cast<LPCWSTR>(aimEnvironment().string(875).utf16()),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,autoLogin.x(),autoLogin.y(),autoLogin.width(),autoLogin.height(),owner,reinterpret_cast<HMENU>(199),GetModuleHandleW(nullptr),nullptr);
  nativeCancel_=CreateWindowExW(0,L"BUTTON",reinterpret_cast<LPCWSTR>(aimEnvironment().string(279).utf16()),WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON,cancel.x(),cancel.y(),cancel.width(),cancel.height(),owner,reinterpret_cast<HMENU>(193),GetModuleHandleW(nullptr),nullptr);if(nativeCancel_)SendMessageW(static_cast<HWND>(nativeCancel_),WM_SETFONT,reinterpret_cast<WPARAM>(nativeFontCombo_),TRUE);
  SendMessageW(static_cast<HWND>(nativeName_),WM_SETFONT,reinterpret_cast<WPARAM>(nativeFontCombo_),TRUE);
  for (void *control : {nativePassword_,nativeSave_,nativeAuto_}) if(control) SendMessageW(static_cast<HWND>(control),WM_SETFONT,reinterpret_cast<WPARAM>(nativeFontEdit_),TRUE);
  SetWindowTextW(static_cast<HWND>(nativeName_),reinterpret_cast<LPCWSTR>(screenName_.utf16()));
  SetWindowTextW(static_cast<HWND>(nativePassword_),reinterpret_cast<LPCWSTR>(password_.utf16()));
  SendMessageW(static_cast<HWND>(nativeSave_),BM_SETCHECK,savePassword_?BST_CHECKED:BST_UNCHECKED,0);
  SendMessageW(static_cast<HWND>(nativeAuto_),BM_SETCHECK,autoLogin_?BST_CHECKED:BST_UNCHECKED,0);
  SendMessageW(static_cast<HWND>(nativePassword_),EM_SETLIMITTEXT,32,0);
  SendMessageW(static_cast<HWND>(nativeName_),CB_LIMITTEXT,32,0);
  // Control notifications (WM_COMMAND) are sent, not posted, so the application event filter never sees them.
  SetWindowSubclass(owner, signOnSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
  SetFocus(static_cast<HWND>(nativeName_));
#endif
}
bool SignOnWindow::nativeEventFilter(const QByteArray &, void *message, qintptr *result) {
#ifdef Q_OS_WIN
  if (!nativeName_||!handle()) return false; // winId() would recreate a destroyed native window and re-enter this filter
  MSG *msg = static_cast<MSG *>(message); HWND owner=reinterpret_cast<HWND>(winId());
  if (msg->message==WM_COMMAND && msg->hwnd==owner) {
    int id=LOWORD(msg->wParam), notice=HIWORD(msg->wParam);
    if(id==193&&notice==BN_CLICKED&&loginStage_){cancelSignOn();if(result)*result=0;return true;}
  }
  bool owns=msg->hwnd==owner||IsChild(owner,msg->hwnd);
  if(owns&&msg->message==WM_KEYDOWN) {
    if(msg->wParam==VK_RETURN) { if(!loginStage_)signOn(); if(result)*result=0; return true; }
    if(msg->wParam==VK_ESCAPE) { if(loginStage_)cancelSignOn();else closeRequested(); if(result)*result=0; return true; }
    if(msg->wParam==VK_TAB) { HWND controls[]={static_cast<HWND>(nativeName_),static_cast<HWND>(nativePassword_),static_cast<HWND>(nativeSave_),static_cast<HWND>(nativeAuto_)}; int current=0; for(int i=0;i<4;i++) if(GetFocus()==controls[i]||IsChild(controls[i],GetFocus())) current=i; SetFocus(controls[(current+((GetKeyState(VK_SHIFT)&0x8000)?3:1))%4]); if(result)*result=0; return true; }
  }
#else
  Q_UNUSED(message); Q_UNUSED(result);
#endif
  return false;
}
void SignOnWindow::nativeCommand(int id, int notice) {
#ifdef Q_OS_WIN
  auto text=[](HWND control) { int length=GetWindowTextLengthW(control); std::vector<wchar_t> buffer(size_t(length)+1); GetWindowTextW(control,buffer.data(),length+1); return QString::fromWCharArray(buffer.data()); };
  auto setCheck=[](void *control,bool on){SendMessageW(static_cast<HWND>(control),BM_SETCHECK,on?BST_CHECKED:BST_UNCHECKED,0);};
  if(id==193&&notice==BN_CLICKED&&loginStage_){cancelSignOn();return;}
  if(id==195&&(notice==CBN_EDITCHANGE||notice==CBN_SELCHANGE)) {
    if(notice==CBN_SELCHANGE){const int index=int(SendMessageW(static_cast<HWND>(nativeName_),CB_GETCURSEL,0,0));std::vector<wchar_t> buffer(size_t(std::max<LRESULT>(0,SendMessageW(static_cast<HWND>(nativeName_),CB_GETLBTEXTLEN,index,0)))+1);SendMessageW(static_cast<HWND>(nativeName_),CB_GETLBTEXT,index,reinterpret_cast<LPARAM>(buffer.data()));screenName_=QString::fromWCharArray(buffer.data());}
    else screenName_=text(static_cast<HWND>(nativeName_));
    // A screen name with a stored password brings it back; any other name starts with an empty password.
    const QString saved=loadSavedPassword(settings_,screenName_.trimmed());
    if(!saved.isEmpty()||!password_.isEmpty()){password_=saved;suppressPasswordTick_=true;SetWindowTextW(static_cast<HWND>(nativePassword_),reinterpret_cast<LPCWSTR>(password_.utf16()));suppressPasswordTick_=false;}
  }
  if(id==197&&notice==EN_CHANGE) {
    const bool wasEmpty=password_.isEmpty(); password_=text(static_cast<HWND>(nativePassword_));
    // Typing a password ticks Save Password and Auto-login.
    if(!suppressPasswordTick_&&wasEmpty&&!password_.isEmpty()){savePassword_=autoLogin_=true;setCheck(nativeSave_,true);setCheck(nativeAuto_,true);settings_.setValue("account/savePassword",true);settings_.setValue("account/autoLogin",true);}
  }
  if(id==198&&notice==BN_CLICKED) { savePassword_=SendMessageW(static_cast<HWND>(nativeSave_),BM_GETCHECK,0,0)==BST_CHECKED; settings_.setValue("account/savePassword",savePassword_); if(!savePassword_){forgetSavedPassword(settings_,screenName_);if(autoLogin_){autoLogin_=false;setCheck(nativeAuto_,false);settings_.setValue("account/autoLogin",false);}} }
  if(id==199&&notice==BN_CLICKED) { autoLogin_=SendMessageW(static_cast<HWND>(nativeAuto_),BM_GETCHECK,0,0)==BST_CHECKED; settings_.setValue("account/autoLogin",autoLogin_); if(autoLogin_&&!savePassword_){savePassword_=true;setCheck(nativeSave_,true);settings_.setValue("account/savePassword",true);} }
#else
  Q_UNUSED(id); Q_UNUSED(notice);
#endif
}
void SignOnWindow::paintContent(QPainter &p) {
  p.fillRect(QRect(1, TitleBarHeight, canvasWidth() - 2, canvasHeight() - TitleBarHeight - 1), art::Face);
  const bool connecting = loginStage_ != 0;
  int staticIndex = 0;
  for (CtlObject *o : formObjects(connecting)) {
    if (!o->shown()) continue;
    const QRect r = o->windowRect();
    switch (o->kind) {
    case CtlObject::Kind::Ate: if (o->id == 957 && !logo_.isNull()) p.drawImage(QPoint(r.center().x() - logo_.width() / 2 + 1, r.top() + (r.height() - logo_.height()) / 2), logo_); break; // GIFDATA 103
    case CtlObject::Kind::Separator: art::drawEtchedLine(p, r); break;
    case CtlObject::Kind::ArtButton: {
      const int action = o->id == 962 ? 1 : o->id == 963 ? 2 : o->id == 964 ? 3 : 0;
      const int state = pressedAction_ == action && hoveredAction_ == action ? 2 : hoveredAction_ == action ? 1 : 0;
      p.drawImage(r.topLeft() + QPoint(1, 1), art::image(o->art[state] ? o->art[state] : o->art[0]));
      break;
    }
    case CtlObject::Kind::Static: {
      const GdiFont font = GdiFont::fromFontDesc(o->fontId);
      if (connecting) {
        // Progress statics: 1019 shows the screen name, 192 the sign-on phase (STRING 226-228).
        const QString text = o->id == 1019 ? screenName_ : aimEnvironment().string(quint32(225 + (loginStage_ == 1 ? 1 : loginStage_ == 2 ? 2 : 3)));
        drawGdiText(p, r, text, font, Qt::black, art::Face, GdiSingleLine | GdiCenter | GdiVCenter | GdiNoPrefix | GdiEndEllipsis);
      } else if (o->id == 0 && staticIndex++ == 0) {
        p.drawImage(QPoint(r.left(), r.top() + (r.height() - screenNameLabel_.height()) / 2), screenNameLabel_); // art 252 "ScreenName" with key
      } else if (o->id == 0) drawGdiText(p, r, aimEnvironment().string(o->textId), font, Qt::black, art::Face, GdiSingleLine | GdiVCenter | GdiNoPrefix);
      else if (o->id == 965) drawGdiText(p, r, QStringLiteral("%1 %2").arg(aimEnvironment().string(o->textId), QStringLiteral("4.7.2480")), font, Qt::black, art::Face, GdiSingleLine | GdiCenter | GdiVCenter | GdiNoPrefix);
      else if (o->id == 1052) { GdiFont link = font; link.underline = true; drawGdiText(p, r, aimEnvironment().string(o->textId), link, QColor(0, 0, 255), art::Face, GdiSingleLine | GdiCenter | GdiVCenter | GdiNoPrefix); }
      break;
    }
    default: break;
    }
  }
#ifndef Q_OS_WIN
  if (!connecting) { drawField(p, formRect(958), screenName_, false, nameActive_); drawField(p, formRect(959), password_, true, !nameActive_); drawCheckbox(p, QRect(formRect(960).topLeft() + QPoint(0, 4), QSize(13, 13)), savePassword_); drawCheckbox(p, QRect(formRect(961).topLeft() + QPoint(0, 4), QSize(13, 13)), autoLogin_); }
  else { const QRect cancel = formRect(193, true); p.setPen(QColor(130,130,130)); p.setBrush(QColor(250,250,250)); p.drawRect(cancel); p.drawText(cancel, Qt::AlignCenter, aimEnvironment().string(279)); }
#endif
  if (!status_.isEmpty() && !connecting) {
    const QRect separator = [&] { for (CtlObject *o : formObjects(false)) if (o->kind == CtlObject::Kind::Separator) return o->windowRect(); return QRect(); }();
    drawGdiText(p, QRect(5, separator.top() - 8, canvasWidth() - 10, 13), status_, GdiFont::fromFontDesc(112), QColor(145, 0, 0), art::Face, GdiSingleLine | GdiCenter | GdiVCenter | GdiNoPrefix | GdiEndEllipsis);
  }
}
void SignOnWindow::contentMouseMove(const QPoint &point) { const int action = actionAt(point); if (action != hoveredAction_) { hoveredAction_ = action; setCursor(action == 4 ? Qt::PointingHandCursor : Qt::ArrowCursor); renderNow(); } }
void SignOnWindow::contentLeave() { if(hoveredAction_){hoveredAction_=0;unsetCursor();renderNow();} }
void SignOnWindow::drawField(QPainter &p, const QRect &r, const QString &value, bool password, bool active) {
  p.fillRect(r, Qt::white);
  p.setPen(active ? QColor(0, 85, 190) : QColor(95, 95, 95)); p.drawLine(r.left(), r.top(), r.right(), r.top()); p.drawLine(r.left(), r.top(), r.left(), r.bottom());
  p.setPen(QColor(255, 255, 255)); p.drawLine(r.left(), r.bottom(), r.right(), r.bottom()); p.drawLine(r.right(), r.top(), r.right(), r.bottom());
  const QString shown = password ? QString(value.size(), QChar(0x25cf)) : value;
  drawGdiText(p, r.adjusted(3, 1, -18, -1), shown, GdiFont::fromFontDesc(108), QColor(15, 15, 15), Qt::white, GdiSingleLine | GdiVCenter | GdiNoPrefix);
  if (active && caretVisible_) { const int x = r.left() + 3 + gdiTextSize(GdiFont::fromFontDesc(108), shown, GdiSingleLine | GdiNoPrefix).width(); p.setPen(QColor(15, 15, 15)); p.drawLine(x, r.top() + 4, x, r.bottom() - 4); }
}
void SignOnWindow::contentMousePress(const QPoint &point, Qt::MouseButton button) {
  if (button != Qt::LeftButton) return;
  if (loginStage_) { if (formRect(193, true).contains(point)) cancelSignOn(); return; }
  if (const int action = actionAt(point); action >= 1 && action <= 3) { pressedAction_ = hoveredAction_ = action; renderNow(); return; }
  if (actionAt(point) == 4) { ate::openUrl(aimEnvironment().string(1315)); return; } // Forgot Password? (osclogin 0x11102a1e, on mouse down)
#ifndef Q_OS_WIN
  if (formRect(958).contains(point)) nameActive_ = true;
  else if (formRect(959).contains(point)) nameActive_ = false;
  else if (formRect(960).contains(point)) savePassword_ = !savePassword_;
  else if (formRect(961).contains(point)) autoLogin_ = !autoLogin_;
  else return;
  settings_.setValue(QStringLiteral("account/screenName"), screenName_);
  settings_.setValue(QStringLiteral("account/savePassword"), savePassword_); settings_.setValue(QStringLiteral("account/autoLogin"), autoLogin_);
  if (!savePassword_) forgetSavedPassword(settings_, screenName_);
  settings_.setValue(QStringLiteral("account/savePassword"), savePassword_);
  settings_.setValue(QStringLiteral("account/autoLogin"), autoLogin_);
  renderNow();
#endif
}
void SignOnWindow::contentMouseRelease(const QPoint &point, Qt::MouseButton button) {
  if (button != Qt::LeftButton || !pressedAction_) return;
  const int action = pressedAction_; pressedAction_ = 0; renderNow();
  if (actionAt(point) != action) return;
  if (action == 1) {
#ifdef Q_OS_WIN
    // o_ShowHelp(hwnd, 1, 0x212): WinHelp HELP_CONTEXT topic 530 of aim95.hlp (osclogin 0x1110225f).
    const std::wstring file = (QCoreApplication::applicationDirPath() + QLatin1Char('/') + aimEnvironment().string(182)).toStdWString();
    WinHelpW(reinterpret_cast<HWND>(winId()), file.c_str(), HELP_CONTEXT, 530);
#endif
  }
  else if (action == 2) showPreferences();
  else if (action == 3) signOn();
}void SignOnWindow::contentKeyPress(QKeyEvent *event) {
  if(loginStage_){if(event->key()==Qt::Key_Escape)cancelSignOn();return;}
  if (event->key() == Qt::Key_Escape) { closeRequested(); return; }
  if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) { signOn(); return; }
  if (event->key() == Qt::Key_Tab) { nameActive_ = !nameActive_; renderNow(); return; }
  QString &value = nameActive_ ? screenName_ : password_;
  if (event->key() == Qt::Key_Backspace) value.chop(1);
  else if (!event->text().isEmpty() && event->text().at(0).isPrint() && value.size() < 32) value.append(event->text());
  renderNow();
}
void SignOnWindow::closeRequested() { if(loginStage_)cancelSignOn();else exitFromTray(); }
void SignOnWindow::signOn() {
  if (!client_||loginStage_) return;
#ifdef Q_OS_WIN
  HWND owner=reinterpret_cast<HWND>(winId());HWND name=GetDlgItem(owner,195),password=GetDlgItem(owner,197);
  if(!name||!password){status_=QStringLiteral("Unable to read the sign-on fields.");renderNow();return;}
  auto read=[](HWND control){int length=GetWindowTextLengthW(control);std::vector<wchar_t> buffer(size_t(length)+1);GetWindowTextW(control,buffer.data(),length+1);return QString::fromWCharArray(buffer.data());};
  screenName_=read(name).trimmed();password_=read(password);
  savePassword_=SendMessageW(GetDlgItem(owner,198),BM_GETCHECK,0,0)==BST_CHECKED;autoLogin_=SendMessageW(GetDlgItem(owner,199),BM_GETCHECK,0,0)==BST_CHECKED;
#endif
  if (host_.isEmpty()) { status_ = QStringLiteral("Use Setup to enter your OSCAR server."); renderNow(); return; }
  if (screenName_.isEmpty() || password_.isEmpty()) { status_ = QStringLiteral("Enter your screen name and password."); renderNow(); return; }
  settings_.setValue(QStringLiteral("account/screenName"), screenName_);
  status_ = QStringLiteral("Connecting");
  renderNow();
  client_->signOn(host_, port_, screenName_, password_);
}
void SignOnWindow::setLoginStage(int stage) { if(stage<0||stage>3||loginStage_==stage)return;loginStage_=stage;hoveredAction_=0;pressedAction_=0;setCanvasSize(formCanvas(stage!=0));
#ifdef Q_OS_WIN
  for(void *control:{nativeName_,nativePassword_,nativeSave_,nativeAuto_})if(control)ShowWindow(static_cast<HWND>(control),stage?SW_HIDE:SW_SHOW);if(nativeCancel_){const QRect cancel=formRect(193,true).translated(-1,-TitleBarHeight);SetWindowPos(static_cast<HWND>(nativeCancel_),nullptr,cancel.x(),cancel.y(),cancel.width(),cancel.height(),SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(static_cast<HWND>(nativeCancel_),stage?SW_SHOW:SW_HIDE);if(stage)SetFocus(static_cast<HWND>(nativeCancel_));}if(!stage&&nativeName_)SetFocus(static_cast<HWND>(nativeName_));
#endif
  renderNow(); }
void SignOnWindow::cancelSignOn() { client_->signOff();setLoginStage(0);status_.clear();renderNow(); }
void SignOnWindow::showPreferences(int category,int commandId) {
  if (preferencesWindow_) { preferencesWindow_->showCategory(category,commandId); preferencesWindow_->requestActivate(); return; }
  preferencesWindow_ = new PreferencesWindow(this);
  connect(preferencesWindow_, &QObject::destroyed, this, [this] { preferencesWindow_.clear(); reloadPreferences(); });
  connect(preferencesWindow_, &PreferencesWindow::settingsChanged, this, &SignOnWindow::reloadPreferences);
  connect(preferencesWindow_, &PreferencesWindow::dismissed, this, &SignOnWindow::reloadPreferences);
  preferencesWindow_->showCategory(category,commandId);
}
void SignOnWindow::reloadPreferences() { settings_.sync();host_ = settings_.value(QStringLiteral("connection/host"),QStringLiteral("login.oscar.aol.com")).toString(); port_ = quint16(settings_.value(QStringLiteral("connection/port"), 5190).toUInt()); savePassword_ = settings_.value(QStringLiteral("account/savePassword"), false).toBool(); autoLogin_ = settings_.value(QStringLiteral("account/autoLogin"), false).toBool();
#ifdef Q_OS_WIN
  if(nativeSave_) SendMessageW(static_cast<HWND>(nativeSave_),BM_SETCHECK,savePassword_?BST_CHECKED:BST_UNCHECKED,0);
  if(nativeAuto_) SendMessageW(static_cast<HWND>(nativeAuto_),BM_SETCHECK,autoLogin_?BST_CHECKED:BST_UNCHECKED,0);
#endif
  renderNow(); }
void SignOnWindow::drawCheckbox(QPainter &p, const QRect &r, bool checked) {
  p.save(); p.setBrush(Qt::NoBrush);
  p.fillRect(r, QColor(255, 255, 255)); p.setPen(QColor(90, 90, 90)); p.drawRect(r.adjusted(0, 0, -1, -1));
  if (checked) { p.setPen(QPen(QColor(0, 86, 180), 2)); p.drawLine(r.left() + 2, r.center().y(), r.left() + 5, r.bottom() - 3); p.drawLine(r.left() + 5, r.bottom() - 3, r.right() - 2, r.top() + 2); }
  p.restore();
}
