#include "auto_responder.h"
#include "preferences.h"
#include "../oscar/client.h"
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
// Minutes without keyboard/mouse input before the user counts as idle (inferred; the original's threshold was not traced).
constexpr int IdleMinutes = 10;
QString key(QString name) { name.remove(QLatin1Char(' ')); return name.toCaseFolded(); }
QString html(const QString &text) { return QStringLiteral("<HTML><BODY>%1</BODY></HTML>").arg(text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<BR>"))); }
}

AutoResponder::AutoResponder(OscarClient *client, QObject *parent) : QObject(parent), client_(client) {
  connect(client_, &OscarClient::messageReceived, this, [this](const QString &sender, const QString &, bool autoResponse) { incoming(sender, autoResponse); });
  connect(client_, &OscarClient::awayChanged, this, [this](bool) { answered_.clear(); });
  connect(client_, &OscarClient::loginStageChanged, this, [this](int stage) { if (stage == 0) { idle_ = false; answered_.clear(); } });
  connect(&timer_, &QTimer::timeout, this, &AutoResponder::poll);
  timer_.start(10000);
}

void AutoResponder::incoming(const QString &sender, bool autoResponse) {
  if (autoResponse || !client_->connected()) return; // never answer an auto response
  QString text;
  if (client_->away()) { if (prefs::awayAutoRespond()) text = client_->awayText(); } // "Auto respond and insert in personal profile"
  else if (idle_ && prefs::idleAutoRespond()) text = prefs::idleMessage();
  if (text.trimmed().isEmpty()) return;
  if (text != answeredText_) { answered_.clear(); answeredText_ = text; }
  if (answered_.contains(key(sender))) return; // one answer per sender for each away/idle period (inferred)
  answered_.insert(key(sender));
  client_->sendAutoResponse(sender, html(text));
}

void AutoResponder::poll() {
#ifdef Q_OS_WIN
  if (!client_->connected()) return;
  LASTINPUTINFO input{sizeof(input), 0}; if (!GetLastInputInfo(&input)) return;
  const quint32 seconds = (GetTickCount() - input.dwTime) / 1000;
  const bool idle = seconds >= IdleMinutes * 60;
  if (idle == idle_) return;
  idle_ = idle; answered_.clear();
  // Privacy "Allow users to see how long I've been idle" (control 252, checked by default) reports it with SNAC(01,11).
  if (prefs::checked(292, 252, true)) client_->setIdle(idle ? seconds : 0);
#endif
}
