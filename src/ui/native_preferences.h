#pragma once
#include <QObject>
#include <QAbstractNativeEventFilter>
#include <QJsonObject>
#include <QMap>
#include <QVariant>
#include <QWindow>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
class NativePreferences final : public QObject, public QAbstractNativeEventFilter {
  Q_OBJECT
public:
  explicit NativePreferences(QWindow *owner, QObject *parent = nullptr);
  ~NativePreferences() override;
  void show();
  void activatePage(int id,int commandId=0);
  void requestActivate();
  void close();
  bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
signals:
  void applied();
  void closed();
private:
  QWindow *owner_;
  QJsonObject inventory_;
  QMap<QString, QVariant> draft_;
#ifdef Q_OS_WIN
  struct Page { NativePreferences *self; int id; HWND window = nullptr; HFONT font = nullptr; bool secondary = false; int collectionPage = 0; int collectionControl = 0; int collectionIndex = -1; QMap<QString,QVariant> draftSnapshot; bool wasDirty = false; };
  HWND host_ = nullptr;
  QMap<int, Page *> pages_;
  QList<Page *> secondary_;
  int selected_ = 293;
  bool loading_ = false;
  QByteArray hostBytes_;
  static INT_PTR CALLBACK hostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  static INT_PTR CALLBACK pageProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  Page *createPage(int id, bool secondary);
  void selectPage(int id);
  void capture(Page *page);
  void restore(Page *page);
  void command(Page *page, int id, int notification);
  void apply();
  void dirty();
  void editItem(Page *page, int list, int dialog, bool edit);
  void refreshItems(Page *page, int list);
  void removeItem(Page *page, int list);
  void updateConnection(Page *page);
  void updateFontSizes(Page *page);
#endif
};
