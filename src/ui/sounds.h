#pragma once
#include <QStringList>

class OscarClient;

// Event sounds of AIM 4.7. Each event uses the original Preferences sound sub-dialog controls (RT_DIALOG 278/280/283/
// 289/290: a "Play a sound when..." check box plus a sound-file combo) stored under nativePreferences/<page>/<control>;
// the bundled defaults are the files of the original install's Sounds folder.
enum class AimSound { BuddyArrival, BuddyDeparture, PhoneCall, ImFirstReceive, ImReceive, ImSend, ChatReceive, ChatSend, NewMail, Alert, TalkBegin, TalkEnd, TalkStop };

void setSoundClient(OscarClient *client);   // away state for "Disable sounds while I'm Away"
void installSoundDefaults();                // registers the original defaults once, as the modules did at start-up
void playAimSound(AimSound sound);
void playSoundFile(const QString &nameOrPath); // a bundled sound name (e.g. "imrcv.wav") or a file path
QStringList bundledSounds();
