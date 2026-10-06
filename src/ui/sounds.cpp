#include "sounds.h"
#include "../oscar/client.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QSettings>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
struct SoundEntry { AimSound sound; int page; int check; int file; const char *defaultFile; int defaultOn; }; // defaultOn: 1 on, 0 off, -1 not registered
// Defaults: buddyui.ocm 0x11286a7b registers UseArrivalSound/UseDepartureSound = 1 (Dooropen.wav/Doorslam.wav);
// icbmui.ocm 0x113898a5 registers UseIMReceiveSound/UseIMSendSound/UseIMInitRcvSound = 1 (imrcv/imsend/ring.wav).
// File defaults: ChatUI "imsend.wav", OscMail "newmail.wav", AlertUI "moo.wav", icbmui talk*.wav.
const SoundEntry entries[] = {
  {AimSound::BuddyArrival, 278, 126, 127, "dooropen.wav", 1},
  {AimSound::BuddyDeparture, 278, 130, 131, "doorslam.wav", 1},
  {AimSound::PhoneCall, 278, 1208, 1209, "phone.wav", 0},
  {AimSound::ImFirstReceive, 289, 111, 112, "ring.wav", 1},
  {AimSound::ImReceive, 289, 115, 116, "imrcv.wav", 1},
  {AimSound::ImSend, 289, 119, 120, "imsend.wav", 1},
  {AimSound::ChatReceive, 290, 675, 676, "", -1},
  {AimSound::ChatSend, 290, 680, 679, "imsend.wav", -1},
  {AimSound::NewMail, 283, 933, 943, "newmail.wav", -1},
  {AimSound::Alert, 0, 0, 0, "moo.wav", -1},
  {AimSound::TalkBegin, 280, 971, 972, "talkbeg.wav", -1},
  {AimSound::TalkEnd, 280, 975, 976, "talkend.wav", -1},
  {AimSound::TalkStop, 280, 979, 980, "talkstop.wav", -1},
};
QPointer<OscarClient> soundClient;
QString key(int page, int control) { return QStringLiteral("nativePreferences/%1/%2").arg(page).arg(control); }
const SoundEntry *entry(AimSound sound) { for (const SoundEntry &e : entries) if (e.sound == sound) return &e; return nullptr; }
}

void setSoundClient(OscarClient *client) { soundClient = client; }

QStringList bundledSounds() { QStringList names; for (const QFileInfo &info : QDir(QStringLiteral(":/aim/sounds")).entryInfoList(QDir::Files, QDir::Name)) names.append(info.fileName()); return names; }

void installSoundDefaults() {
  QSettings settings;
  for (const SoundEntry &e : entries) {
    if (!e.page) continue;
    if (*e.defaultFile && !settings.contains(key(e.page, e.file))) settings.setValue(key(e.page, e.file), QString::fromLatin1(e.defaultFile));
    if (e.defaultOn >= 0 && !settings.contains(key(e.page, e.check))) settings.setValue(key(e.page, e.check), e.defaultOn);
  }
}

void playSoundFile(const QString &nameOrPath) {
#ifdef Q_OS_WIN
  if (nameOrPath.trimmed().isEmpty()) return;
  static QHash<QString, QByteArray> bundled; // PlaySound(SND_MEMORY|SND_ASYNC) needs the buffer to outlive playback
  const QString bundledPath = QStringLiteral(":/aim/sounds/") + QFileInfo(nameOrPath).fileName().toLower();
  if (!QFileInfo(nameOrPath).isAbsolute() && QFile::exists(bundledPath)) {
    auto it = bundled.find(bundledPath);
    if (it == bundled.end()) { QFile file(bundledPath); if (!file.open(QIODevice::ReadOnly)) return; it = bundled.insert(bundledPath, file.readAll()); }
    PlaySoundW(reinterpret_cast<LPCWSTR>(it->constData()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    return;
  }
  if (QFileInfo::exists(nameOrPath)) PlaySoundW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(nameOrPath).utf16()), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
#else
  Q_UNUSED(nameOrPath); // TODO: audio output on Linux/macOS (Qt Multimedia is not part of the static kit)
#endif
}

void playAimSound(AimSound sound) {
  const SoundEntry *e = entry(sound); if (!e) return;
  QSettings settings;
  // "Disable sounds while I'm Away" (Away Message preferences, control 748).
  if (soundClient && soundClient->away() && settings.value(key(274, 748), 0).toInt()) return;
  QString file = QString::fromLatin1(e->defaultFile);
  if (e->page) {
    if (!settings.value(key(e->page, e->check), e->defaultOn > 0 ? 1 : 0).toInt()) return;
    file = settings.value(key(e->page, e->file), file).toString();
  }
  playSoundFile(file);
}
