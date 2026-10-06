#include "sign_on_window.h"
#include "buddy_list_window.h"
#include "preferences_window.h"
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
void forgetSavedPassword(QSettings &settings, const QString &screenName) { if (!screenName.trimmed().isEmpty()) settings.remove(passwordKey(screenName)); }
}
SignOnWindow::SignOnWindow(OscarClient *client) : WindowBase(QStringLiteral("Sign On"), QSize(212, 378)), client_(client), logo_(QStringLiteral(":/aim/signon.gif")), helpIcon_(transparentBitmap(QStringLiteral(":/aim/signon-help.bmp"),0x00ccccccu)), setupIcon_(transparentBitmap(QStringLiteral(":/aim/setup-wrench.bmp"))), signOnIcon_(transparentBitmap(QStringLiteral(":/aim/signon-button.bmp"))) {
  screenName_ = settings_.value(QStringLiteral("account/screenName")).toString();
  screenNameLabel_ = transparentBitmap(QStringLiteral(":/aim/screen-name.bmp"));
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
  connect(client_, &OscarClient::rosterReady, this, [this] { if (savePassword_) storeSavedPassword(settings_, screenName_, password_); else { forgetSavedPassword(settings_, screenName_); password_.fill(QChar(0)); password_.clear(); } if(preferencesWindow_)preferencesWindow_->close(); if(buddyWindow_)buddyWindow_->deleteLater();buddyWindow_ = new BuddyListWindow(client_);buddyWindow_->QObject::setParent(this); /* top-level and unowned like the original, so it appears in the taskbar */connect(buddyWindow_,&BuddyListWindow::actionRequested,this,[this](int id,const QString &name){if(id==20002||id==174)showPreferences();else if(id==745)signOffFromTray();else emit actionRequested(id,name);}); connect(buddyWindow_,&BuddyListWindow::exitAccepted,qApp,&QCoreApplication::quit); buddyWindow_->show(); hide(); });
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
  for(void *control : {nativeName_,nativePassword_,nativeSave_,nativeAuto_,nativeCancel_}) if(control&&IsWindow(static_cast<HWND>(control))) DestroyWindow(static_cast<HWND>(control));
  if (nativeFont_) DeleteObject(static_cast<HFONT>(nativeFont_));
#endif
}
bool SignOnWindow::event(QEvent *event) {
  if (event->type() == QEvent::Expose && isExposed()) initializeNativeControls();
  return WindowBase::event(event);
}
void SignOnWindow::initializeNativeControls() {
#ifdef Q_OS_WIN
  if (nativeName_) return;
  HWND owner = reinterpret_cast<HWND>(winId());
  SetWindowLongPtrW(owner, GWL_STYLE, GetWindowLongPtrW(owner, GWL_STYLE) | WS_CLIPCHILDREN);
  HDC dc = GetDC(owner); nativeFont_ = CreateFontW(-MulDiv(8, GetDeviceCaps(dc, LOGPIXELSY), 72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,0,0,L"MS Sans Serif"); ReleaseDC(owner,dc);
  nativeName_ = CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWN|CBS_AUTOHSCROLL,14,211-TitleBarHeight,177,120,owner,reinterpret_cast<HMENU>(195),GetModuleHandleW(nullptr),nullptr);
  nativePassword_ = CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_PASSWORD|ES_AUTOHSCROLL,14,254-TitleBarHeight,177,22,owner,reinterpret_cast<HMENU>(197),GetModuleHandleW(nullptr),nullptr);
  nativeSave_ = CreateWindowExW(0,L"BUTTON",L"Save password",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,14,283-TitleBarHeight,116,14,owner,reinterpret_cast<HMENU>(198),GetModuleHandleW(nullptr),nullptr);
  nativeAuto_ = CreateWindowExW(0,L"BUTTON",L"Auto-login",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,133,283-TitleBarHeight,76,14,owner,reinterpret_cast<HMENU>(199),GetModuleHandleW(nullptr),nullptr);
  nativeCancel_=CreateWindowExW(0,L"BUTTON",L"Cancel",WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON,77,241-TitleBarHeight,55,21,owner,reinterpret_cast<HMENU>(193),GetModuleHandleW(nullptr),nullptr);if(nativeCancel_)SendMessageW(static_cast<HWND>(nativeCancel_),WM_SETFONT,reinterpret_cast<WPARAM>(nativeFont_),TRUE);
  for (void *control : {nativeName_,nativePassword_,nativeSave_,nativeAuto_}) if(control) SendMessageW(static_cast<HWND>(control),WM_SETFONT,reinterpret_cast<WPARAM>(nativeFont_),TRUE);
  SetWindowTextW(static_cast<HWND>(nativeName_),reinterpret_cast<LPCWSTR>(screenName_.utf16()));
  SetWindowTextW(static_cast<HWND>(nativePassword_),reinterpret_cast<LPCWSTR>(password_.utf16()));
  SendMessageW(static_cast<HWND>(nativeSave_),BM_SETCHECK,savePassword_?BST_CHECKED:BST_UNCHECKED,0);
  SendMessageW(static_cast<HWND>(nativeAuto_),BM_SETCHECK,autoLogin_?BST_CHECKED:BST_UNCHECKED,0);
  SendMessageW(static_cast<HWND>(nativePassword_),EM_SETLIMITTEXT,32,0);
  SendMessageW(static_cast<HWND>(nativeName_),CB_LIMITTEXT,32,0);
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
    auto text=[](HWND control) { int length=GetWindowTextLengthW(control); std::vector<wchar_t> buffer(size_t(length)+1); GetWindowTextW(control,buffer.data(),length+1); return QString::fromWCharArray(buffer.data()); };
    if(id==195&&(notice==CBN_EDITCHANGE||notice==CBN_SELCHANGE)) screenName_=text(static_cast<HWND>(nativeName_));
    if(id==197&&notice==EN_CHANGE) password_=text(static_cast<HWND>(nativePassword_));
    if(id==198&&notice==BN_CLICKED) { savePassword_=SendMessageW(static_cast<HWND>(nativeSave_),BM_GETCHECK,0,0)==BST_CHECKED; settings_.setValue("account/savePassword",savePassword_); if(!savePassword_)forgetSavedPassword(settings_,screenName_); }
    if(id==199&&notice==BN_CLICKED) { autoLogin_=SendMessageW(static_cast<HWND>(nativeAuto_),BM_GETCHECK,0,0)==BST_CHECKED; settings_.setValue("account/autoLogin",autoLogin_); }
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
void SignOnWindow::paintContent(QPainter &p) {
  p.fillRect(QRect(1, TitleBarHeight, canvasWidth() - 2, canvasHeight() - TitleBarHeight - 1), QColor(240, 240, 240));
  if (!logo_.isNull()) p.drawImage(QPoint(10, 28), logo_);
  p.setPen(QColor(160,160,160));p.drawLine(5,188,207,188);p.setPen(Qt::white);p.drawLine(5,189,207,189);
  if(loginStage_){QFont font(QStringLiteral("Arial"));font.setPixelSize(11);p.setFont(font);p.setPen(Qt::black);p.drawText(QRect(5,195,202,18),Qt::AlignCenter,screenName_);const QString phases[]={QString(),QStringLiteral("1. Connecting..."),QStringLiteral("2. Verifying name and password..."),QStringLiteral("3. Starting services...")};p.drawText(QRect(5,215,202,18),Qt::AlignCenter,phases[loginStage_]);
#ifndef Q_OS_WIN
    p.setPen(QColor(130,130,130));p.setBrush(QColor(250,250,250));p.drawRect(QRect(78,241,55,21));p.drawText(QRect(78,241,55,21),Qt::AlignCenter,QStringLiteral("Cancel"));
#endif
    return;}
  QFont text(QStringLiteral("MS Sans Serif"), 8);
  p.drawImage(QPoint(15, 198), screenNameLabel_);
#ifndef Q_OS_WIN
  drawField(p, QRect(15, 211, 177, 22), screenName_, false, nameActive_);
#endif
  p.setPen(QColor(20, 20, 20)); p.setFont(text); p.drawText(QRect(15, 237, 177, 15), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Password"));
#ifndef Q_OS_WIN
  drawField(p, QRect(15, 254, 177, 22), password_, true, !nameActive_);
  drawCheckbox(p, QRect(15, 283, 13, 13), savePassword_);
  drawText(p, QPoint(33, 294), QStringLiteral("Save password"), QColor(20, 20, 20), text);
  drawCheckbox(p, QRect(134, 283, 13, 13), autoLogin_);
  drawText(p, QPoint(151, 294), QStringLiteral("Auto-login"), QColor(20, 20, 20), text);
#endif
  const QPoint buttonPositions[]={QPoint(9,303),QPoint(43,303),QPoint(146,302)};
  for(int button=0;button<3;++button){int state=pressedAction_==button+1&&hoveredAction_==button+1?2:hoveredAction_==button+1?1:0;p.drawImage(buttonPositions[button],buttonStates_[button][state]);}
  drawText(p, QPoint(68, 352), QStringLiteral("Version: 4.7.2480"), QColor(20, 20, 20), QFont(QStringLiteral("MS Sans Serif"), 7));
  QFont forgotFont(QStringLiteral("MS Sans Serif"), 8); forgotFont.setUnderline(true);
  drawText(p, QPoint(60, 373), QStringLiteral("Forgot Password?"), QColor(0, 0, 238), forgotFont);
  if (!status_.isEmpty()) {
    p.fillRect(QRect(10, 182, 192, 17), QColor(242, 242, 242));
    drawText(p, QPoint(12, 194), status_.left(36), QColor(145, 0, 0), QFont(QStringLiteral("MS Sans Serif"), 7));
  }
}
void SignOnWindow::contentMouseMove(const QPoint &point) { int action=QRect(7,302,36,42).contains(point)?1:QRect(41,302,41,42).contains(point)?2:QRect(144,302,45,42).contains(point)?3:QRect(55,359,109,18).contains(point)?4:0; if(action!=hoveredAction_){hoveredAction_=action;setCursor(action==4?Qt::PointingHandCursor:Qt::ArrowCursor);renderNow();} }
void SignOnWindow::contentLeave() { if(hoveredAction_){hoveredAction_=0;unsetCursor();renderNow();} }
void SignOnWindow::drawField(QPainter &p, const QRect &r, const QString &value, bool password, bool active) {
  p.fillRect(r, Qt::white);
  p.setPen(active ? QColor(0, 85, 190) : QColor(95, 95, 95)); p.drawLine(r.left(), r.top(), r.right(), r.top()); p.drawLine(r.left(), r.top(), r.left(), r.bottom());
  p.setPen(QColor(255, 255, 255)); p.drawLine(r.left(), r.bottom(), r.right(), r.bottom()); p.drawLine(r.right(), r.top(), r.right(), r.bottom());
  const QString shown = password ? QString(value.size(), QChar(0x25cf)) : value;
  const QFont font(QStringLiteral("MS Sans Serif"), 8);
  drawText(p, QPoint(r.left() + 3, r.top() + 15), shown, QColor(15, 15, 15), font);
  if (!password) {
    p.setPen(QColor(105, 105, 105));
    const QPoint center(r.right() - 9, r.center().y());
    p.drawLine(center.x() - 4, center.y() - 2, center.x(), center.y() + 2);
    p.drawLine(center.x(), center.y() + 2, center.x() + 4, center.y() - 2);
  }
  if (active && caretVisible_) { const int x = r.left() + 3 + QFontMetrics(font).horizontalAdvance(shown); p.setPen(QColor(15, 15, 15)); p.drawLine(x, r.top() + 4, x, r.bottom() - 4); }
}
void SignOnWindow::contentMousePress(const QPoint &point, Qt::MouseButton button) {
  if (button != Qt::LeftButton) return;
  if(loginStage_){if(QRect(78,241,55,21).contains(point))cancelSignOn();return;}
  if(QRect(7,302,36,42).contains(point)||QRect(41,302,41,42).contains(point)||QRect(144,302,48,42).contains(point)){pressedAction_=QRect(7,302,36,42).contains(point)?1:QRect(41,302,41,42).contains(point)?2:3;hoveredAction_=pressedAction_;renderNow();return;}
  if (QRect(15, 211, 177, 22).contains(point)) nameActive_ = true;
  else if (QRect(15, 254, 177, 22).contains(point)) nameActive_ = false;
  else if (QRect(15, 283, 13, 13).contains(point)) savePassword_ = !savePassword_;
  else if (QRect(134, 283, 13, 13).contains(point)) autoLogin_ = !autoLogin_;
  else if (QRect(41, 302, 41, 42).contains(point)) { showPreferences(); return; }
  else if (QRect(144, 302, 45, 42).contains(point)) { signOn(); return; }
  else if (QRect(7, 302, 36, 42).contains(point)) status_ = QStringLiteral("AIM Help is unavailable");
  else if (QRect(55, 359, 109, 18).contains(point)) status_ = QStringLiteral("Password recovery is unavailable");
  else return;
  settings_.setValue(QStringLiteral("account/screenName"), screenName_);
  settings_.setValue(QStringLiteral("account/savePassword"), savePassword_);
  settings_.setValue(QStringLiteral("account/autoLogin"), autoLogin_);
  renderNow();
}
void SignOnWindow::contentMouseRelease(const QPoint &point,Qt::MouseButton button){if(button!=Qt::LeftButton||!pressedAction_)return;int action=pressedAction_;pressedAction_=0;renderNow();if(action==1&&QRect(7,302,36,42).contains(point)){status_=QStringLiteral("AIM Help is unavailable");renderNow();}else if(action==2&&QRect(41,302,41,42).contains(point))showPreferences();else if(action==3&&QRect(144,302,48,42).contains(point))signOn();}
void SignOnWindow::contentKeyPress(QKeyEvent *event) {
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
void SignOnWindow::setLoginStage(int stage) { if(stage<0||stage>3||loginStage_==stage)return;loginStage_=stage;hoveredAction_=0;pressedAction_=0;setCanvasSize(QSize(212,stage?269:378));
#ifdef Q_OS_WIN
  for(void *control:{nativeName_,nativePassword_,nativeSave_,nativeAuto_})if(control)ShowWindow(static_cast<HWND>(control),stage?SW_HIDE:SW_SHOW);if(nativeCancel_){ShowWindow(static_cast<HWND>(nativeCancel_),stage?SW_SHOW:SW_HIDE);if(stage)SetFocus(static_cast<HWND>(nativeCancel_));}if(!stage&&nativeName_)SetFocus(static_cast<HWND>(nativeName_));
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
