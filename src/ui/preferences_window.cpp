#include "preferences_window.h"
#include "connection_window.h"
#include "native_preferences.h"
#include <QKeyEvent>
#include <QFontMetrics>

namespace {
void drawCheck(QPainter &p, const QRect &r, bool checked) {
  p.save(); p.setBrush(Qt::NoBrush);
  p.fillRect(r, Qt::white); p.setPen(QColor(110, 110, 110)); p.drawRect(r.adjusted(0, 0, -1, -1));
  if (checked) { p.setPen(QPen(QColor(0, 86, 180), 2)); p.drawLine(r.left() + 2, r.center().y(), r.left() + 5, r.bottom() - 3); p.drawLine(r.left() + 5, r.bottom() - 3, r.right() - 2, r.top() + 2); }
  p.restore();
}
void drawRadio(QPainter &p, const QRect &r, bool checked) {
  p.save();
  p.setPen(QColor(90, 90, 90)); p.setBrush(Qt::white); p.drawEllipse(r); if (checked) { p.setBrush(QColor(0, 86, 180)); p.setPen(Qt::NoPen); p.drawEllipse(r.adjusted(3, 3, -3, -3)); }
  p.restore();
}
}
PreferencesWindow::PreferencesWindow(QWindow *parent) : WindowBase(QStringLiteral("AOL Instant Messenger (SM) Preferences"), QSize(561, 428), parent) {
#ifdef Q_OS_WIN
  native_ = new NativePreferences(parent, this);
  connect(native_, &NativePreferences::applied, this, &PreferencesWindow::settingsChanged);
  connect(native_, &NativePreferences::closed, this, &PreferencesWindow::dismissed);
#endif
  setCaptionButtons(false, false, true);
  savePassword_ = settings_.value(QStringLiteral("account/savePassword"), false).toBool();
  autoLogin_ = settings_.value(QStringLiteral("account/autoLogin"), false).toBool();
  startWithWindows_ = settings_.value(QStringLiteral("account/startWithWindows"), false).toBool();
  lan_ = settings_.value(QStringLiteral("connection/lan"), true).toBool();
  autoUpgrade_ = settings_.value(QStringLiteral("preferences/autoUpgrade"), true).toBool();
  showToday_ = settings_.value(QStringLiteral("preferences/showToday"), true).toBool();
  reconnect_ = settings_.value(QStringLiteral("preferences/reconnect"), true).toBool();
  showReconnectDialog_ = settings_.value(QStringLiteral("preferences/showReconnectDialog"), true).toBool();
}
void PreferencesWindow::show() {
#ifdef Q_OS_WIN
  native_->show();
#else
  WindowBase::show();
#endif
}
void PreferencesWindow::requestActivate() {
#ifdef Q_OS_WIN
  native_->requestActivate();
#else
  WindowBase::requestActivate();
#endif
}
void PreferencesWindow::showCategory(int id,int commandId) {
#ifdef Q_OS_WIN
  native_->activatePage(id,commandId);
#else
  Q_UNUSED(id);Q_UNUSED(commandId);show();
#endif
}
bool PreferencesWindow::close() {
#ifdef Q_OS_WIN
  native_->close(); return true;
#else
  emit dismissed(); return WindowBase::close();
#endif
}
void PreferencesWindow::paintContent(QPainter &p) {
  const QFont font(QStringLiteral("MS Sans Serif"), 8);
  drawText(p, QPoint(25, 41), QStringLiteral("Category"), QColor(45, 45, 45), font);
  p.fillRect(QRect(5, 46, 92, 352), Qt::white);
  p.setPen(QColor(135, 135, 135)); p.drawRect(QRect(5, 46, 92, 352));
  const QStringList categories{QStringLiteral("Buddy List"), QStringLiteral("Privacy"), QStringLiteral("Sign On/Off"), QStringLiteral("Idle Message"), QStringLiteral("Away Message"), QStringLiteral("Buddy Icons"), QStringLiteral("Mail"), QStringLiteral("Stock Ticker"), QStringLiteral("News Ticker"), QStringLiteral("IM/Chat"), QStringLiteral("IM Image"), QStringLiteral("Talk"), QStringLiteral("File Sharing"), QStringLiteral("File Transfer"), QStringLiteral("Games")};
  for (int i = 0; i < categories.size(); ++i) {
    const int y = 57 + i * 21;
    if (i == 2) p.fillRect(QRect(7, y - 2, 88, 17), QColor(0, 86, 180));
    drawText(p, QPoint(9, y + 10), categories[i], i == 2 ? Qt::white : QColor(70, 70, 70), font);
  }
  drawText(p, QPoint(101, 43), QStringLiteral("Sign On/Off"), QColor(20, 20, 20), QFont(QStringLiteral("MS Sans Serif"), 8, QFont::Bold));
  p.setPen(QColor(190, 190, 190)); p.drawRoundedRect(QRect(101, 57, 323, 67), 4, 4); p.drawRoundedRect(QRect(101, 128, 323, 56), 4, 4); p.drawRoundedRect(QRect(101, 188, 323, 75), 4, 4);
  drawText(p, QPoint(111, 64), QStringLiteral("Sign On"), QColor(60, 60, 60), font);
  drawCheck(p, QRect(116, 72, 13, 13), savePassword_); drawText(p, QPoint(136, 83), QStringLiteral("Save Password"), QColor(25, 25, 25), font);
  drawCheck(p, QRect(116, 92, 13, 13), autoLogin_); drawText(p, QPoint(136, 103), QStringLiteral("Automatically sign on when AIM starts"), QColor(25, 25, 25), font);
  drawCheck(p, QRect(116, 112, 13, 13), startWithWindows_); drawText(p, QPoint(136, 123), QStringLiteral("Start AIM when Windows starts"), QColor(25, 25, 25), font);
  drawText(p, QPoint(111, 135), QStringLiteral("Internet Connection"), QColor(60, 60, 60), font);
  drawRadio(p, QRect(116, 151, 13, 13), !lan_); drawText(p, QPoint(136, 162), QStringLiteral("Modem"), QColor(25, 25, 25), font);
  drawRadio(p, QRect(116, 167, 13, 13), lan_); drawText(p, QPoint(136, 178), QStringLiteral("Local area network (LAN)"), QColor(25, 25, 25), font);
  drawText(p, QPoint(111, 195), QStringLiteral("Auto Upgrade"), QColor(60, 60, 60), font);
  drawCheck(p, QRect(116, 205, 13, 13), autoUpgrade_); drawText(p, QPoint(136, 216), QStringLiteral("Notify me when a new version is available:"), QColor(25, 25, 25), font);
  p.fillRect(QRect(122, 227, 196, 21), Qt::white); p.setPen(QColor(130, 130, 130)); p.drawRect(QRect(122, 227, 196, 21)); drawText(p, QPoint(128, 242), QStringLiteral("Final Release"), QColor(25, 25, 25), font); p.drawLine(298, 235, 303, 241); p.drawLine(303, 241, 308, 235);
  drawCheck(p, QRect(116, 270, 13, 13), showToday_); drawText(p, QPoint(136, 281), QStringLiteral("Show Today window at signon"), QColor(25, 25, 25), font);
  drawCheck(p, QRect(116, 292, 13, 13), reconnect_); drawText(p, QPoint(136, 303), QStringLiteral("Reconnect automatically"), QColor(25, 25, 25), font);
  drawCheck(p, QRect(116, 314, 13, 13), showReconnectDialog_); drawText(p, QPoint(136, 325), QStringLiteral("Display reconnect dialog"), QColor(25, 25, 25), font);
  p.setPen(QColor(45, 45, 45)); p.setFont(font); p.drawText(QRect(366, 264, 185, 52), Qt::AlignLeft | Qt::TextWordWrap, QStringLiteral("Note: If you connect to the Internet through a proxy, click Connection to configure AIM for your proxy server."));
  p.setPen(QColor(130, 130, 130)); p.setBrush(QColor(245, 245, 245)); p.drawRect(QRect(392, 315, 92, 26)); drawText(p, QPoint(411, 332), QStringLiteral("Connection"), QColor(25, 25, 25), font);
  p.setPen(QColor(0, 120, 215)); p.setBrush(QColor(245, 245, 245)); p.drawRect(QRect(319, 400, 72, 23));
  p.setPen(QColor(130, 130, 130)); p.drawRect(QRect(400, 400, 72, 23)); p.setPen(dirty_ ? QColor(130, 130, 130) : QColor(190, 190, 190)); p.drawRect(QRect(481, 400, 72, 23));
  drawText(p, QPoint(344, 416), QStringLiteral("OK"), QColor(25, 25, 25), font); drawText(p, QPoint(418, 416), QStringLiteral("Cancel"), QColor(25, 25, 25), font); drawText(p, QPoint(500, 416), QStringLiteral("Apply"), dirty_ ? QColor(25, 25, 25) : QColor(150, 150, 150), font);
}
void PreferencesWindow::contentMousePress(const QPoint &point, Qt::MouseButton button) {
  if (button != Qt::LeftButton) return;
  if (QRect(116, 72, 13, 13).contains(point)) savePassword_ = !savePassword_;
  else if (QRect(116, 92, 13, 13).contains(point)) autoLogin_ = !autoLogin_;
  else if (QRect(116, 112, 13, 13).contains(point)) startWithWindows_ = !startWithWindows_;
  else if (QRect(116, 151, 13, 13).contains(point)) lan_ = false;
  else if (QRect(116, 167, 13, 13).contains(point)) lan_ = true;
  else if (QRect(116, 205, 13, 13).contains(point)) autoUpgrade_ = !autoUpgrade_;
  else if (QRect(116, 270, 13, 13).contains(point)) showToday_ = !showToday_;
  else if (QRect(116, 292, 13, 13).contains(point)) reconnect_ = !reconnect_;
  else if (QRect(116, 314, 13, 13).contains(point)) showReconnectDialog_ = !showReconnectDialog_;
  else if (QRect(392, 315, 92, 26).contains(point)) { openConnection(); return; }
  else if (QRect(319, 400, 72, 23).contains(point)) { saveAndClose(); return; }
  else if (QRect(400, 400, 72, 23).contains(point)) { close(); return; }
  else if (QRect(481, 400, 72, 23).contains(point)) { if (dirty_) save(); return; }
  else return;
  dirty_ = true;
  renderNow();
}
void PreferencesWindow::contentKeyPress(QKeyEvent *event) { if (event->key() == Qt::Key_Escape) close(); else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) saveAndClose(); }
void PreferencesWindow::save() {
  settings_.setValue(QStringLiteral("account/savePassword"), savePassword_);
  settings_.setValue(QStringLiteral("account/autoLogin"), autoLogin_);
  settings_.setValue(QStringLiteral("account/startWithWindows"), startWithWindows_);
  settings_.setValue(QStringLiteral("connection/lan"), lan_);
  settings_.setValue(QStringLiteral("preferences/autoUpgrade"), autoUpgrade_);
  settings_.setValue(QStringLiteral("preferences/showToday"), showToday_);
  settings_.setValue(QStringLiteral("preferences/reconnect"), reconnect_);
  settings_.setValue(QStringLiteral("preferences/showReconnectDialog"), showReconnectDialog_);
  dirty_ = false;
  emit settingsChanged();
  renderNow();
}
void PreferencesWindow::saveAndClose() {
  save();
  close();
}
void PreferencesWindow::openConnection() {
  if (connectionWindow_) { connectionWindow_->show(); connectionWindow_->requestActivate(); return; }
  connectionWindow_ = new ConnectionWindow(settings_.value(QStringLiteral("connection/host")).toString(), quint16(settings_.value(QStringLiteral("connection/port"), 5190).toUInt()), this);
  connectionWindow_->setSaveHandler([this](const QString &host, quint16 port) { settings_.setValue(QStringLiteral("connection/host"), host); settings_.setValue(QStringLiteral("connection/port"), port); });
  connect(connectionWindow_, &QObject::destroyed, this, [this] { connectionWindow_.clear(); });
  connectionWindow_->show();
}
