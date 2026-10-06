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
// Runs an original dialog modally: controls are created from the template, `init` fills them, `command` handles
// WM_COMMAND (return true when handled; call EndDialog from it to finish). Returns the EndDialog result.
#include <functional>
INT_PTR runOriginalDialog(HWND owner, int id, const std::function<void(HWND)> &init, const std::function<bool(HWND, int, int)> &command);
QString formatAimString(QString text, const QStringList &arguments); // fills %s / %d / %ld / %0.200s in order
#endif
