#include "user_actions.h"
#include "ctl_group.h"
#include "native_dialog.h"
#include "../oscar/client.h"
#include <QPointer>
#include <QRegularExpression>
#include <QWindow>
#include <memory>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
QString aimString(quint32 id) { return aimEnvironment().string(id); }
QString appTitle() { return QStringLiteral("AOL Instant Messenger (SM)"); }
#ifdef Q_OS_WIN
HWND ownerHandle(QWindow *owner) { return owner && owner->handle() ? reinterpret_cast<HWND>(owner->winId()) : nullptr; }
QString itemText(HWND dialog, int id) { return nativeWindowText(GetDlgItem(dialog, id)); }
#endif
}

namespace userActions {
void infoBox(QWindow *owner, const QString &text) {
#ifdef Q_OS_WIN
  MessageBoxW(ownerHandle(owner), reinterpret_cast<LPCWSTR>(text.utf16()), reinterpret_cast<LPCWSTR>(appTitle().utf16()), MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_TASKMODAL); // 0x42040
#else
  Q_UNUSED(owner); Q_UNUSED(text);
#endif
}

void warn(QWindow *owner, OscarClient *client, const QString &screenName) {
#ifdef Q_OS_WIN
  const QString name = screenName.trimmed(); if (!client || name.isEmpty()) return;
  bool anonymous = false;
  const INT_PTR result = runOriginalDialog(ownerHandle(owner), 142,
    [&](HWND dialog) { SetDlgItemTextW(dialog, 384, reinterpret_cast<LPCWSTR>(formatAimString(aimString(536), {name, name, name}).utf16())); },
    [&](HWND dialog, int id, int) { if (id == IDOK) { anonymous = IsDlgButtonChecked(dialog, 383) == BST_CHECKED; EndDialog(dialog, IDOK); return true; } return false; });
  if (result != IDOK) return; // Cancel (id 2 = IDCANCEL) or closing the dialog sends nothing
  // Report the outcome once (icbmui 0x11383332 / 0x113833e6).
  auto done = std::make_shared<QList<QMetaObject::Connection>>(); QPointer<QWindow> guard(owner);
  auto finish = [done] { for (const auto &connection : *done) QObject::disconnect(connection); };
  done->append(QObject::connect(client, &OscarClient::warnCompleted, client, [=](const QString &warned, quint16, quint16 level) {
    if (warned.compare(name, Qt::CaseInsensitive) != 0) return; finish();
    infoBox(guard, formatAimString(aimString(526), {name, QString::number(MulDiv(level, 100, 999))}));
  }));
  done->append(QObject::connect(client, &OscarClient::operationFailed, client, [=](const QString &operation, const QString &reason) {
    if (operation.compare(QStringLiteral("Warn %1").arg(name), Qt::CaseInsensitive) != 0) return; finish();
    const bool notAllowed = reason.contains(QStringLiteral("0x000d"), Qt::CaseInsensitive);
    infoBox(guard, notAllowed ? formatAimString(aimString(527), {name}) : formatAimString(aimString(528), {name, QString()}));
  }));
  client->warnUser(name, anonymous);
#else
  Q_UNUSED(owner); Q_UNUSED(client); Q_UNUSED(screenName);
#endif
}

void block(QWindow *owner, OscarClient *client, const QString &screenName) {
#ifdef Q_OS_WIN
  const QString name = screenName.trimmed(); if (!client || name.isEmpty()) return;
  if (client->isBlocked(name)) return;
  const QString question = formatAimString(aimString(529), {name});
  if (MessageBoxW(ownerHandle(owner), reinterpret_cast<LPCWSTR>(question.utf16()), reinterpret_cast<LPCWSTR>(appTitle().utf16()), MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_TASKMODAL) != IDYES) return; // 0x42024
  client->blockUser(name);
#else
  Q_UNUSED(owner); Q_UNUSED(client); Q_UNUSED(screenName);
#endif
}

void addBuddy(QWindow *owner, OscarClient *client, const QString &screenName) {
#ifdef Q_OS_WIN
  const QString name = screenName.trimmed(); if (!client || name.isEmpty()) return;
  const QVector<aim::oscar::FeedbagItem> roster = client->roster();
  QStringList groups; for (const auto &item : roster) if (item.classId == 1 && item.groupId != 0 && !item.name.isEmpty()) groups.append(item.name);
  QString chosen;
  const INT_PTR result = runOriginalDialog(ownerHandle(owner), 248,
    [&](HWND dialog) {
      SetDlgItemTextW(dialog, 499, reinterpret_cast<LPCWSTR>(formatAimString(itemText(dialog, 499), {name}).utf16()));
      for (const QString &group : groups) SendDlgItemMessageW(dialog, 227, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(group.utf16()));
      SendDlgItemMessageW(dialog, 227, CB_SETCURSEL, 0, 0);
    },
    [&](HWND dialog, int id, int) {
      if (id == 551) { // New Group...: RT_DIALOG 249
        QString group;
        if (runOriginalDialog(dialog, 249, [](HWND) {}, [&](HWND inner, int innerId, int) { if (innerId == 380) { group = itemText(inner, 370).trimmed(); if (group.isEmpty()) return true; EndDialog(inner, IDOK); return true; } return false; }) == IDOK) {
          LRESULT index = SendDlgItemMessageW(dialog, 227, CB_FINDSTRINGEXACT, WPARAM(-1), reinterpret_cast<LPARAM>(group.utf16()));
          if (index == CB_ERR) index = SendDlgItemMessageW(dialog, 227, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(group.utf16()));
          SendDlgItemMessageW(dialog, 227, CB_SETCURSEL, WPARAM(index), 0);
        }
        return true;
      }
      if (id == IDOK) { chosen = itemText(dialog, 227).trimmed(); if (chosen.isEmpty()) return true; EndDialog(dialog, IDOK); return true; }
      return false;
    });
  if (result != IDOK) return;
  for (const auto &item : roster) if (item.classId == 1 && item.groupId != 0 && item.name.compare(chosen, Qt::CaseInsensitive) == 0) { client->addBuddy(item.groupId, name); return; }
  // A new group: create it first, then add the buddy into it once the server confirmed the group.
  auto connection = std::make_shared<QMetaObject::Connection>();
  *connection = QObject::connect(client, &OscarClient::rosterEditFinished, client, [=](bool success) {
    QObject::disconnect(*connection); if (!success) return;
    for (const auto &item : client->roster()) if (item.classId == 1 && item.groupId != 0 && item.name.compare(chosen, Qt::CaseInsensitive) == 0) { client->addBuddy(item.groupId, name); return; }
  });
  if (!client->addGroup(chosen)) QObject::disconnect(*connection);
#else
  Q_UNUSED(owner); Q_UNUSED(client); Q_UNUSED(screenName);
#endif
}
}
