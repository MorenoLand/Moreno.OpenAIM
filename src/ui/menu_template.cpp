#include "menu_template.h"
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QFile>
#include <QWindow>
#include <QtEndian>
#include <functional>
#include <memory>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
constexpr quint16 MenuGrayed = 0x0001, MenuChecked = 0x0008, MenuPopup = 0x0010, MenuEnd = 0x0080;
QList<MenuItem> parseLevel(const QByteArray &data, qsizetype &offset) {
  QList<MenuItem> items;
  while (offset + 2 <= data.size()) {
    const quint16 flags = qFromLittleEndian<quint16>(data.constData() + offset); offset += 2;
    MenuItem item; item.grayed = flags & MenuGrayed; item.checked = flags & MenuChecked;
    if (!(flags & MenuPopup)) { if (offset + 2 > data.size()) break; item.id = qFromLittleEndian<quint16>(data.constData() + offset); offset += 2; }
    while (offset + 2 <= data.size()) { const char16_t c = qFromLittleEndian<quint16>(data.constData() + offset); offset += 2; if (!c) break; item.text.append(QChar(c)); }
    if (flags & MenuPopup) item.children = parseLevel(data, offset);
    items.append(item);
    if (flags & MenuEnd) break;
  }
  return items;
}
}

QString MenuItem::label() const { QString value = text.section(QLatin1Char('\t'), 0, 0); value.replace(QStringLiteral("&&"), QStringLiteral("\x01")); value.remove(QLatin1Char('&')); value.replace(QChar(1), QLatin1Char('&')); return value; }
QChar MenuItem::mnemonic() const { for (qsizetype i = 0; i + 1 < text.size(); ++i) if (text[i] == QLatin1Char('&')) { if (text[i + 1] == QLatin1Char('&')) { ++i; continue; } return text[i + 1].toLower(); } return {}; }

QList<MenuItem> parseMenuTemplate(const QByteArray &data) {
  if (data.size() < 4) return {};
  const quint16 version = qFromLittleEndian<quint16>(data.constData()), headerSize = qFromLittleEndian<quint16>(data.constData() + 2);
  if (version != 0) return {};
  qsizetype offset = 4 + headerSize;
  return parseLevel(data, offset);
}

QList<MenuItem> loadMenuResource(int id) { QFile file(QStringLiteral(":/aim/menus/%1").arg(id)); return file.open(QIODevice::ReadOnly) ? parseMenuTemplate(file.readAll()) : QList<MenuItem>{}; }

#ifdef Q_OS_WIN
namespace {
// Popup items are owner-drawn so the hovered item gets a clearly visible highlight; the themed Win11 menu highlight
// (rgb 240 on 249) is barely visible. Layout, font and colours otherwise follow the native menu.
struct DrawnItem { QString text; bool grayed = false; bool checked = false; bool submenu = false; };
constexpr COLORREF MenuBackground = RGB(249, 249, 249), MenuHot = RGB(222, 228, 236), MenuText = RGB(0, 0, 0), MenuGrayText = RGB(160, 160, 160);
constexpr int TextLeft = 36, TextRight = 28, ItemPadding = 7, ArrowWidth = 20, HotInsetX = 4, HotInsetY = 1;
HFONT menuFont() { NONCLIENTMETRICSW metrics{}; metrics.cbSize = sizeof(metrics); SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0); return CreateFontIndirectW(&metrics.lfMenuFont); }
class OwnerDraw final : public QAbstractNativeEventFilter {
public:
  explicit OwnerDraw(HWND owner) : owner_(owner), font_(menuFont()) { QCoreApplication::instance()->installNativeEventFilter(this); }
  ~OwnerDraw() override { QCoreApplication::instance()->removeNativeEventFilter(this); DeleteObject(font_); }
  bool nativeEventFilter(const QByteArray &, void *message, qintptr *result) override {
    MSG *msg = static_cast<MSG *>(message);
    if (msg->hwnd != owner_) return false;
    if (msg->message == WM_MEASUREITEM) {
      auto *measure = reinterpret_cast<MEASUREITEMSTRUCT *>(msg->lParam); if (measure->CtlType != ODT_MENU) return false;
      const auto *item = reinterpret_cast<const DrawnItem *>(measure->itemData);
      HDC dc = GetDC(owner_); HGDIOBJ old = SelectObject(dc, font_); RECT text{}; const QString label = item->text.section(QLatin1Char('\t'), 0, 0), accel = item->text.section(QLatin1Char('\t'), 1);
      DrawTextW(dc, reinterpret_cast<LPCWSTR>(label.utf16()), -1, &text, DT_CALCRECT | DT_SINGLELINE); int width = text.right;
      if (!accel.isEmpty()) { RECT accelRect{}; DrawTextW(dc, reinterpret_cast<LPCWSTR>(accel.utf16()), -1, &accelRect, DT_CALCRECT | DT_SINGLELINE); width += accelRect.right + 24; }
      TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics); SelectObject(dc, old); ReleaseDC(owner_, dc);
      measure->itemWidth = UINT(TextLeft + width + TextRight - GetSystemMetrics(SM_CXMENUCHECK)); measure->itemHeight = UINT(metrics.tmHeight + ItemPadding);
      if (result) *result = TRUE; return true;
    }
    if (msg->message == WM_DRAWITEM) {
      auto *draw = reinterpret_cast<DRAWITEMSTRUCT *>(msg->lParam); if (draw->CtlType != ODT_MENU) return false;
      const auto *item = reinterpret_cast<const DrawnItem *>(draw->itemData); HDC dc = draw->hDC; const RECT r = draw->rcItem;
      HBRUSH background = CreateSolidBrush(MenuBackground); FillRect(dc, &r, background); DeleteObject(background);
      const bool hot = (draw->itemState & ODS_SELECTED) && !item->grayed;
      if (hot) { HBRUSH brush = CreateSolidBrush(MenuHot); HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, GetStockObject(NULL_PEN)); RoundRect(dc, r.left + HotInsetX, r.top + HotInsetY, r.right - HotInsetX + 1, r.bottom - HotInsetY + 1, 8, 8); SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); }
      HGDIOBJ oldFont = SelectObject(dc, font_); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, item->grayed ? MenuGrayText : MenuText);
      BOOL cues = FALSE; SystemParametersInfoW(SPI_GETKEYBOARDCUES, 0, &cues, 0);
      const UINT prefix = (!cues || (draw->itemState & ODS_NOACCEL)) ? DT_HIDEPREFIX : 0; // underline mnemonics only when keyboard cues are on, as native menus do
      if (item->checked) { RECT check{r.left + 8, r.top, r.left + TextLeft - 4, r.bottom}; DrawTextW(dc, L"\u2713", -1, &check, DT_SINGLELINE | DT_VCENTER | DT_CENTER); }
      const QString label = item->text.section(QLatin1Char('\t'), 0, 0), accel = item->text.section(QLatin1Char('\t'), 1);
      RECT text{r.left + TextLeft, r.top, r.right - TextRight, r.bottom}; DrawTextW(dc, reinterpret_cast<LPCWSTR>(label.utf16()), -1, &text, DT_SINGLELINE | DT_VCENTER | prefix);
      if (!accel.isEmpty()) DrawTextW(dc, reinterpret_cast<LPCWSTR>(accel.utf16()), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
      SelectObject(dc, oldFont);
      if (item->submenu) {
        // Draw a native-style chevron and clip the area so the system does not paint its classic arrow over it.
        const int cx = r.right - ArrowWidth / 2 - 4, cy = (r.top + r.bottom) / 2; HPEN pen = CreatePen(PS_SOLID, 1, item->grayed ? MenuGrayText : MenuText); HGDIOBJ oldPen = SelectObject(dc, pen);
        const POINT chevron[] = {{cx - 2, cy - 4}, {cx + 2, cy}, {cx - 2, cy + 4}}; Polyline(dc, chevron, 3); const POINT inner[] = {{cx - 1, cy - 4}, {cx + 3, cy}, {cx - 1, cy + 4}}; Polyline(dc, inner, 3);
        SelectObject(dc, oldPen); DeleteObject(pen);
        ExcludeClipRect(dc, r.right - ArrowWidth - 8, r.top, r.right, r.bottom);
      }
      if (result) *result = TRUE; return true;
    }
    return false;
  }
private:
  HWND owner_;
  HFONT font_;
};
}
#endif

int popupMenu(QWindow *owner, const QList<MenuItem> &items, const QPoint &globalPosition, const QList<int> &disabledIds) {
#ifdef Q_OS_WIN
  if (!owner || items.isEmpty()) return 0;
  HWND window = reinterpret_cast<HWND>(owner->winId());
  std::vector<std::unique_ptr<DrawnItem>> drawn;
  std::function<HMENU(const QList<MenuItem> &)> build = [&](const QList<MenuItem> &level) {
    HMENU menu = CreatePopupMenu();
    for (const MenuItem &item : level) {
      if (item.isSeparator()) { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); continue; }
      auto data = std::make_unique<DrawnItem>(); data->text = item.text; data->grayed = item.grayed || disabledIds.contains(item.id); data->checked = item.checked; data->submenu = !item.children.isEmpty();
      const UINT flags = MF_OWNERDRAW | (data->grayed ? MF_GRAYED : MF_ENABLED) | (item.checked ? MF_CHECKED : 0);
      const auto *payload = reinterpret_cast<LPCWSTR>(data.get()); drawn.push_back(std::move(data));
      if (!item.children.isEmpty()) AppendMenuW(menu, flags | MF_POPUP, reinterpret_cast<UINT_PTR>(build(item.children)), payload);
      else AppendMenuW(menu, flags, UINT_PTR(item.id), payload);
    }
    return menu;
  };
  HMENU menu = build(items);
  OwnerDraw ownerDraw(window);
  SetForegroundWindow(window); // a menu owned by a background window does not track or dismiss correctly
  const int selected = int(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, globalPosition.x(), globalPosition.y(), window, nullptr));
  PostMessageW(window, WM_NULL, 0, 0);
  DestroyMenu(menu);
  return selected;
#else
  Q_UNUSED(owner); Q_UNUSED(items); Q_UNUSED(globalPosition); Q_UNUSED(disabledIds);
  return 0; // TODO: painted popup for non-Windows platforms
#endif
}