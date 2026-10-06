#include "ui/sign_on_window.h"
#include "oscar/client.h"
#include "ui/system_tray.h"
#include "ui/messaging_window.h"
#include "ui/away_dialog.h"
#include "ui/sounds.h"
#include <QElapsedTimer>
#include "ui/buddy_list_window.h"
#include <QGuiApplication>
#include <QSettings>
#include <memory>

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  QGuiApplication::setQuitOnLastWindowClosed(false);
  QGuiApplication::setOrganizationName(QStringLiteral("MorenoLand"));
  QGuiApplication::setApplicationName(QStringLiteral("OpenAIM"));
  OscarClient client;
  installSoundDefaults(); setSoundClient(&client);
  // Buddy arrival/departure sounds; the presence burst right after sign-on is not announced (inferred: the original stays quiet at sign-on).
  QElapsedTimer signedOn; QObject::connect(&client,&OscarClient::rosterReady,&client,[&]{signedOn.start();});
  QObject::connect(&client,&OscarClient::loginStageChanged,&client,[&](int stage){if(stage==0)signedOn.invalidate();});
  QObject::connect(&client,&OscarClient::buddyPresenceChanged,&client,[&](const QString &,bool online){if(signedOn.isValid()&&signedOn.elapsed()>3000)playAimSound(online?AimSound::BuddyArrival:AimSound::BuddyDeparture);});
  SignOnWindow window(&client);
  MessagingWindows messaging(&client,&window);
  AwayMessages away(&client,&window);
  QObject::connect(&window,&SignOnWindow::actionRequested,&messaging,[&](int id,const QString &name){if(id==139)messaging.openMessage(name);else if(id==24000)away.newMessage();else if(id>=BuddyListWindow::AwaySavedBase&&id<BuddyListWindow::AwaySavedBase+100)away.useSaved(id-BuddyListWindow::AwaySavedBase);});
  window.show();
  if (app.arguments().contains(QStringLiteral("--ui-preview=im"))) messaging.openMessage(); // developer preview of the IM window without signing on
  std::unique_ptr<BuddyListWindow> previewBuddyList; if (app.arguments().contains(QStringLiteral("--ui-preview=buddy"))) { previewBuddyList = std::make_unique<BuddyListWindow>(&client); previewBuddyList->show(); }
#ifdef Q_OS_WIN
  SystemTray tray(&window);
  auto updateTray = [&] { bool online=client.connected();QList<SystemTray::Action> actions;if(online){actions.append({10002,QStringLiteral("&Sign Off"),true});actions.append({0,QString(),true});}actions.append({20000,QStringLiteral("Send &Instant Message..."),online});actions.append({20001,QStringLiteral("Get Member Inf&o..."),online});actions.append({20003,QStringLiteral("Show &Buddy List..."),online});actions.append({20002,QStringLiteral("&Preferences..."),true});SystemTray::Action away{0,QStringLiteral("&Away Message"),online};if(online)away.children.append({24000,QStringLiteral("New Message..."),true});actions.append(away);actions.append({0,QStringLiteral("Read &Mail"),true,{{1003,QStringLiteral("Add New POP3 Mailbox"),true}}});actions.append({0,QStringLiteral("&Help"),true,{{27002,QStringLiteral("How to &Use Help..."),true},{27003,QStringLiteral("&Help Topics..."),true},{27004,QStringLiteral("&Buddy list Help..."),true},{27005,QStringLiteral("&Report a Bug"),true},{27006,QStringLiteral("&Frequently Asked Questions..."),true},{27007,QStringLiteral("&About AOL Instant Messenger(SM)..."),true}}});actions.append({25000,QStringLiteral("Save Buddy List..."),true});actions.append({25001,QStringLiteral("Load Buddy List..."),online});actions.append({35000,QStringLiteral("E&xit"),true});if(!online){actions.append({0,QString(),true});actions.append({10001,QStringLiteral("Sign On..."),true});}tray.setActions(actions);tray.setOnline(online);tray.setToolTip(online?QStringLiteral("AOL Instant Messenger (SM)"):QStringLiteral("AOL Instant Messenger (SM) - Signed Off")); };
  updateTray(); tray.show();
  QObject::connect(&tray,&SystemTray::activated,&window,&SignOnWindow::showClient);
  QObject::connect(&tray,&SystemTray::triggered,&window,[&](int id){if(id==20000)messaging.openMessage(QString());else if(id==20002)window.showPreferences();else if(id==10002){window.signOffFromTray();updateTray();}else if(id==35000)window.exitFromTray();else if(id==10001||id==20003)window.showClient();else if(id==1003)window.showPreferences(282,928);else if(id==24000)away.newMessage();else if(id>=BuddyListWindow::AwaySavedBase&&id<BuddyListWindow::AwaySavedBase+100)away.useSaved(id-BuddyListWindow::AwaySavedBase);});
  QObject::connect(&client,&OscarClient::statusChanged,&window,[&](const QString&){updateTray();});
  QObject::connect(&client,&OscarClient::failed,&window,[&](const QString&){updateTray();});
#endif
  return app.exec();
}
