#pragma once
// Builds native Win32 dialogs from the original AimRes.dll dialog templates dumped to resources/dialogs.json.
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

QJsonObject originalDialog(int id);
// An empty DLGTEMPLATE carrying the original size, title and font; controls are created separately.
QByteArray nativeDialogTemplate(const QJsonObject &dialog, bool popup);
// Maps original control classes (atoms and AOL-private classes) to stock Win32 classes.
QString nativeControlClass(const QJsonValue &value);
// Creates every control of the original template inside an initialised dialog window.
void createNativeControls(HWND window, const QJsonObject &dialog, HFONT font);
QString nativeWindowText(HWND window);
#endif
