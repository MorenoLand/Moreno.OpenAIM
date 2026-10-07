#include "ctl_window.h"
#include "ate_link.h"
#include "preferences.h"
#include "art.h"
#include "gdi_text.h"
#include <QAbstractTextDocumentLayout>
#include <QCursor>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QPainter>
#include <QPalette>
#include <QTextBlock>
#include <QTextLayout>
#include <QWheelEvent>
#include <functional>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
constexpr quint32 EsMultiline = 0x0004, EsWantReturn = 0x1000, EsPassword = 0x0020;
QPalette panePalette() { QPalette palette; palette.setColor(QPalette::Text, Qt::black); palette.setColor(QPalette::Base, Qt::white); palette.setColor(QPalette::Link, QColor(0, 0, 255)); return palette; }
QFont controlFont(quint32 fontId) { const GdiFont g = GdiFont::fromFontDesc(fontId); QFont font(g.face.isEmpty() ? QStringLiteral("Arial") : g.face); font.setPixelSize(std::max(9, std::abs(g.height ? g.height : 13))); font.setBold(g.weight >= 700); return font; }
QFont ateFont() { QFont font(QStringLiteral("Times New Roman")); font.setPixelSize(16); return font; }
}

CtlWindow::CtlWindow(const QString &title, int ctlGroupId, int menuId) : WindowBase(title, QSize(300, 200)), group_(loadCtlGroup(ctlGroupId)), menu_(menuId) {}

QSize CtlWindow::canvasForClient(const QSize &client) {
#ifdef Q_OS_WIN
  return client + QSize(2, TitleBarHeight + 3);
#else
  return client + QSize(0, TitleBarHeight);
#endif
}
QRect CtlWindow::clientRect() const {
#ifdef Q_OS_WIN
  return QRect(1, TitleBarHeight, width(), height());
#else
  return QRect(0, TitleBarHeight, width(), height() - TitleBarHeight);
#endif
}
QRect CtlWindow::layoutRect() const {
  const QRect c = clientRect(); if (menu_.items.isEmpty()) return c;
  const int bottom = PaintedMenuBar::bottom(menu_.layout(c.left(), c.top() + 4, c.width()), c.top() + 4);
  return QRect(QPoint(c.left(), bottom), c.bottomRight());
}
void CtlWindow::relayout() {
  const QRect c = clientRect(); if (!menu_.items.isEmpty()) menuRects_ = menu_.layout(c.left(), c.top() + 4, c.width());
  if (group_) ctlMove(*group_, layoutRect(), aimEnvironment());
}

CtlWindow::Pane &CtlWindow::pane(quint32 id) { return panes_[id]; }
TextEditor &CtlWindow::editor(quint32 id) {
  Pane &p = pane(id);
  if (!p.editor) { CtlObject *o = object(id); const bool compose = isComposePane(id); p.editor = std::make_unique<TextEditor>(o && o->kind == CtlObject::Kind::Edit ? controlFont(o->fontId) : compose ? prefs::composeFont() : ateFont()); if (compose) p.editor->cursor.setCharFormat(prefs::composeFormat()); }
  return *p.editor;
}
QTextDocument &CtlWindow::document(quint32 id) {
  Pane &p = pane(id);
  if (!p.document) { p.document = std::make_unique<QTextDocument>(); p.document->setDefaultFont(ateFont()); p.document->setDocumentMargin(2); }
  return *p.document;
}
bool CtlWindow::isEditable(const CtlObject &o) const { return o.kind == CtlObject::Kind::Edit || (o.kind == CtlObject::Kind::Ate && editableAtes_.contains(o.id)); }
QString CtlWindow::staticText(const CtlObject &o) const { return aimEnvironment().string(o.textId); }
QRect CtlWindow::toolbarRect(const CtlObject &o) const { const QRect r = o.windowRect(); return (o.kind == CtlObject::Kind::Ate && (o.style & 0x1000)) ? QRect(r.left() + 1, r.top() + 1, r.width() - 2, AteToolbar::Height) : QRect(); }
QRect CtlWindow::textRect(const CtlObject &o) const { QRect r = o.windowRect().adjusted(2, 2, -2, -2); const QRect bar = toolbarRect(o); if (bar.isValid()) r.setTop(bar.bottom() + 1); return r.adjusted(2, 1, -2, -1); }

void CtlWindow::paintContent(QPainter &p) {
  relayout();
  p.fillRect(clientRect(), art::Face);
  if (!menu_.items.isEmpty()) menu_.paint(p, menuRects_, hoveredMenu_, openMenu_);
  if (group_) paintObject(p, *group_);
}

void CtlWindow::paintObject(QPainter &p, CtlObject &o) {
  if (!o.shown()) return;
  if (paintCustom(p, o)) return;
  const QRect r = o.windowRect();
  switch (o.kind) {
  case CtlObject::Kind::Group: case CtlObject::Kind::TabGroup:
    if (o.flags & CtlObject::Padding) art::drawEtched(p, r);
    for (const auto &child : o.children) paintObject(p, *child);
    return;
  case CtlObject::Kind::Static: {
    const QString text = staticText(o); if (text.isEmpty()) return;
    const GdiFont font = GdiFont::fromFontDesc(o.fontId);
    const bool multi = text.contains(QLatin1Char('\n')) || gdiTextSize(font, text, GdiSingleLine | GdiNoPrefix).width() > r.width();
    drawGdiText(p, r, text, font, Qt::black, art::Face, multi ? GdiWordBreak | GdiNoPrefix : GdiSingleLine | GdiVCenter | GdiNoPrefix);
    return;
  }
  case CtlObject::Kind::Separator: art::drawEtchedLine(p, r); return;
  case CtlObject::Kind::RateMeter: p.fillRect(r, QColor(64, 64, 64)); for (int i = 0; i < 15; ++i) p.fillRect(QRect(r.left() + 2 + i * 3, r.top() + 1, 2, r.height() - 2), i < 2 ? QColor(255, 0, 0) : i < 5 ? QColor(255, 255, 0) : QColor(0, 200, 0)); return;
  case CtlObject::Kind::ArtButton: {
    if (!o.art[0]) return;
    const bool enabled = controlEnabled(o.id); const int state = !enabled ? 0 : (pressed_ == o.id && hovered_ == o.id) ? 2 : hovered_ == o.id ? 1 : 0;
    QImage image = art::image(o.art[state] ? o.art[state] : o.art[0]); if (!enabled) image = art::disabled(image);
    p.drawImage(r.topLeft() + QPoint(1, 1), image);
    return;
  }
  case CtlObject::Kind::Button: {
    // Classic push button: raised 2 px bevel, sunken while pressed, caption centred (mnemonic shown).
    const bool down = pressed_ == o.id && hovered_ == o.id, enabled = controlEnabled(o.id);
    p.fillRect(r, art::Face);
    p.setPen(down ? Qt::black : art::Highlight); p.drawLine(r.left(), r.bottom() - 1, r.left(), r.top()); p.drawLine(r.left(), r.top(), r.right() - 1, r.top());
    p.setPen(down ? art::Highlight : Qt::black); p.drawLine(r.right(), r.top(), r.right(), r.bottom()); p.drawLine(r.right(), r.bottom(), r.left(), r.bottom());
    p.setPen(art::Shadow); if (!down) { p.drawLine(r.right() - 1, r.top() + 1, r.right() - 1, r.bottom() - 1); p.drawLine(r.right() - 1, r.bottom() - 1, r.left() + 1, r.bottom() - 1); }
    drawGdiText(p, r.adjusted(2, 2, -2, -2).translated(down ? 1 : 0, down ? 1 : 0), staticText(o), GdiFont::fromFontDesc(o.fontId), enabled ? QColor(Qt::black) : art::Shadow, art::Face, GdiSingleLine | GdiCenter | GdiVCenter);
    if (focus_ == o.id) { QPen pen(Qt::black); pen.setStyle(Qt::DotLine); p.setPen(pen); p.drawRect(r.adjusted(3, 3, -4, -4)); }
    return;
  }
  case CtlObject::Kind::Edit: {
    const bool enabled = controlEnabled(o.id);
    p.fillRect(r, enabled ? QColor(Qt::white) : art::Face); art::drawSunken(p, r);
    paintEditor(p, o, r.adjusted(3, 2, -3, -2), pane(o.id), !(o.style & EsMultiline));
    return;
  }
  case CtlObject::Kind::Ate: {
    p.fillRect(r, prefs::composeWindowColor()); art::drawSunken(p, r);
    const QRect bar = toolbarRect(o);
    if (bar.isValid()) { Pane &pn = pane(o.id); if (!pn.toolbar) pn.toolbar = std::make_unique<AteToolbar>(AteToolbar::Set::Chat); pn.toolbar->layout(bar); pn.toolbar->paint(p, toolPane_ == o.id ? hoveredTool_ : -1, toolPane_ == o.id ? pressedTool_ : -1, {}); }
    if (isEditable(o)) paintEditor(p, o, textRect(o), pane(o.id), false);
    else paintDocument(p, textRect(o), pane(o.id), o.id);
    return;
  }
  case CtlObject::Kind::Tree: case CtlObject::Kind::ListBox: case CtlObject::Kind::TreeView: case CtlObject::Kind::ListView: {
    p.fillRect(r, Qt::white); art::drawSunken(p, r);
    const QRect area = r.adjusted(2, 2, -2, -2); const QStringList rows = listRows(o.id); const GdiFont font = GdiFont::fromFontDesc(o.fontId);
    const int rowH = gdiTextSize(font, QStringLiteral("Wg")).height() + 2;
    p.save(); p.setClipRect(area);
    for (int i = 0; i < rows.size(); ++i) {
      const QRect row(area.left(), area.top() + i * rowH, area.width(), rowH); const bool selected = selectedRow(o.id) == i;
      const QColor background = selected ? QColor(0, 120, 215) : QColor(Qt::white);
      p.fillRect(row, background); drawGdiText(p, row.adjusted(2, 0, 0, 0), rows[i], font, selected ? QColor(Qt::white) : listRowColor(o.id, i), background, GdiSingleLine | GdiVCenter | GdiNoPrefix);
    }
    p.restore();
    return;
  }
  default: return;
  }
}

void CtlWindow::paintEditor(QPainter &p, const CtlObject &o, const QRect &area, Pane &pn, bool singleLine) {
  TextEditor &ed = editor(o.id); Q_UNUSED(pn);
  QTextDocument &doc = ed.document; doc.setTextWidth(singleLine ? 100000 : qMax(1, area.width()));
  const QTextBlock block = ed.cursor.block(); const QTextLayout *layout = block.layout(); const int position = ed.cursor.position() - block.position();
  const QTextLine line = layout ? layout->lineForTextPosition(position) : QTextLine(); const QRectF blockRect = doc.documentLayout()->blockBoundingRect(block);
  qreal &scroll = pane(o.id).scroll; const qreal caretY = line.isValid() ? blockRect.top() + line.y() + line.height() : blockRect.bottom();
  scroll = qBound(qreal(0), caretY - area.height(), qMax(qreal(0), doc.documentLayout()->documentSize().height() - area.height()));
  qreal hscroll = 0; if (singleLine && line.isValid()) hscroll = qMax(qreal(0), blockRect.left() + line.cursorToX(position) - area.width() + 4);
  p.save(); p.setClipRect(area); p.translate(area.left() - hscroll, area.top() - scroll + (singleLine ? qMax(qreal(0), (area.height() - doc.size().height()) / 2) : 0));
  auto context = o.kind == CtlObject::Kind::Ate ? ate::paintContext(doc, prefs::composeWindowColor()) : QAbstractTextDocumentLayout::PaintContext(); if (o.kind != CtlObject::Kind::Ate) context.palette = panePalette(); context.clip = QRectF(hscroll, scroll, area.width(), area.height());
  if (ed.cursor.hasSelection()) { QAbstractTextDocumentLayout::Selection selection; selection.cursor = ed.cursor; selection.format.setBackground(QColor(0, 120, 215)); selection.format.setForeground(Qt::white); context.selections.append(selection); }
  doc.documentLayout()->draw(&p, context);
  if (focus_ == o.id && isActive() && line.isValid()) { const qreal x = blockRect.left() + line.cursorToX(position), y = blockRect.top() + line.y(); p.setPen(o.kind == CtlObject::Kind::Ate && qGray(prefs::composeWindowColor().rgb()) < 128 ? Qt::white : Qt::black); p.drawLine(QPointF(x, y), QPointF(x, y + line.height())); }
  p.restore();
}
void CtlWindow::paintDocument(QPainter &p, const QRect &area, Pane &pn, quint32 id) {
  const qreal zoom = documentZoom(id), width = area.width() / zoom, height = area.height() / zoom;
  QTextDocument &doc = document(id); doc.setTextWidth(qMax(qreal(1), width));
  const qreal maximum = qMax(qreal(0), doc.documentLayout()->documentSize().height() - height);
  pn.scroll = follow_.value(id, false) ? maximum : qBound(qreal(0), pn.scroll, maximum);
  p.save(); p.setClipRect(area); p.translate(area.left(), area.top()); p.scale(zoom, zoom); p.translate(0, -pn.scroll);
  auto context = ate::paintContext(doc, prefs::composeWindowColor()); context.clip = QRectF(0, pn.scroll, width, height);
  doc.documentLayout()->draw(&p, context); p.restore();
}

CtlObject *CtlWindow::hit(const QPoint &point) const {
  CtlObject *found = nullptr;
  std::function<void(CtlObject &)> visit = [&](CtlObject &o) { if (!o.shown()) return; if (o.kind != CtlObject::Kind::Group && o.kind != CtlObject::Kind::TabGroup && o.windowRect().contains(point)) found = &o; for (const auto &child : o.children) visit(*child); };
  if (group_) visit(*group_); return found;
}
QList<quint32> CtlWindow::focusOrder() const {
  QList<quint32> order;
  std::function<void(CtlObject &)> visit = [&](CtlObject &o) { if (!o.shown()) return; if (isEditable(o) && controlEnabled(o.id)) order.append(o.id); for (const auto &child : o.children) visit(*child); };
  if (group_) visit(*group_); return order;
}

void CtlWindow::openMenu(int index) {
  if (index < 0 || index >= menuRects_.size()) return;
  openMenu_ = index; requestUpdate();
  const int id = popupMenu(this, preparedMenu(index), canvasToGlobal(menuRects_[index].bottomLeft() + QPoint(0, 1)));
  openMenu_ = hoveredMenu_ = -1; requestUpdate();
  if (id) menuCommand(id);
}
void CtlWindow::toolbarCommand(quint32 ateId, int command) {
  TextEditor &ed = editor(ateId); const QTextCharFormat current = ed.cursor.charFormat(); QTextCharFormat format;
  switch (command) {
  case AteToolbar::Bold: format.setFontWeight(current.fontWeight() >= QFont::Bold ? QFont::Normal : QFont::Bold); break;
  case AteToolbar::Italic: format.setFontItalic(!current.fontItalic()); break;
  case AteToolbar::Underline: format.setFontUnderline(!current.fontUnderline()); break;
  case AteToolbar::Link: if (ate::editLink(this, ed.cursor)) editorChanged(ateId); requestUpdate(); return;
  case AteToolbar::Smiley: { const int glyph = AteToolbar::pickSmiley(this, QCursor::pos() - QPoint(0, 100)); if (glyph >= 0) ed.cursor.insertText(AteToolbar::smileyCode(glyph)); requestUpdate(); return; }
  default: return;
  }
  if (ed.cursor.hasSelection()) ed.cursor.mergeCharFormat(format); else { QTextCharFormat merged = current; merged.merge(format); ed.cursor.setCharFormat(merged); }
  requestUpdate();
}

void CtlWindow::contentMousePress(const QPoint &point, Qt::MouseButton button) {
  if (button != Qt::LeftButton) return;
  relayout();
  for (int i = 0; i < menuRects_.size(); ++i) if (menuRects_[i].contains(point)) { openMenu(i); return; }
  CtlObject *o = hit(point); if (!o) return;
  if (o->kind == CtlObject::Kind::Ate) { const QRect bar = toolbarRect(*o); Pane &pn = pane(o->id); if (bar.contains(point) && pn.toolbar) { const int tool = pn.toolbar->hit(point); if (tool >= 0) { toolPane_ = o->id; pressedTool_ = tool; requestUpdate(); } return; } }
  if ((o->kind == CtlObject::Kind::ArtButton && o->art[0]) || o->kind == CtlObject::Kind::Button) { if (controlEnabled(o->id)) { pressed_ = hovered_ = o->id; requestUpdate(); } return; }
  if (isEditable(*o) && controlEnabled(o->id)) {
    focus_ = o->id; TextEditor &ed = editor(o->id); const QRect area = o->kind == CtlObject::Kind::Edit ? o->windowRect().adjusted(3, 2, -3, -2) : textRect(*o);
    ed.document.setTextWidth(o->kind == CtlObject::Kind::Edit && !(o->style & EsMultiline) ? 100000 : qMax(1, area.width()));
    const int position = ed.document.documentLayout()->hitTest(QPointF(point.x() - area.left(), point.y() - area.top() + pane(o->id).scroll), Qt::FuzzyHit); if (position >= 0) ed.cursor.setPosition(position);
    requestUpdate(); return;
  }
  if (o->kind == CtlObject::Kind::Tree || o->kind == CtlObject::Kind::ListBox) {
    const GdiFont font = GdiFont::fromFontDesc(o->fontId); const int rowH = gdiTextSize(font, QStringLiteral("Wg")).height() + 2;
    const int row = (point.y() - o->windowRect().top() - 2) / rowH; setSelectedRow(o->id, row < listRows(o->id).size() ? row : -1);
  }
}
void CtlWindow::contentMouseRelease(const QPoint &point, Qt::MouseButton button) {
  if (button != Qt::LeftButton) return;
  if (pressedTool_ >= 0) { const int tool = pressedTool_; const quint32 ate = toolPane_; pressedTool_ = -1; requestUpdate(); Pane &pn = pane(ate); if (pn.toolbar && pn.toolbar->hit(point) == tool) toolbarCommand(ate, pn.toolbar->items()[tool].command); return; }
  if (!pressed_) return;
  const quint32 id = pressed_; pressed_ = 0; requestUpdate();
  CtlObject *o = hit(point); if (o && o->id == id) command(int(id));
}
void CtlWindow::contentMouseDoubleClick(const QPoint &point, Qt::MouseButton button) {
  if (button != Qt::LeftButton) return;
  CtlObject *o = hit(point); if (o && (o->kind == CtlObject::Kind::Tree || o->kind == CtlObject::Kind::ListBox) && selectedRow(o->id) >= 0) listActivated(o->id, selectedRow(o->id));
}
void CtlWindow::contentMouseMove(const QPoint &point) {
  int menu = -1; for (int i = 0; i < menuRects_.size(); ++i) if (menuRects_[i].contains(point)) menu = i;
  CtlObject *o = hit(point); const quint32 hovered = o && ((o->kind == CtlObject::Kind::ArtButton && o->art[0]) || o->kind == CtlObject::Kind::Button) ? o->id : 0;
  int tool = -1; quint32 toolPane = 0; if (o && o->kind == CtlObject::Kind::Ate) { Pane &pn = pane(o->id); if (pn.toolbar) { tool = pn.toolbar->hit(point); toolPane = o->id; } }
  setCursor(o && isEditable(*o) && tool < 0 ? Qt::IBeamCursor : Qt::ArrowCursor);
  if (menu != hoveredMenu_ || hovered != hovered_ || tool != hoveredTool_) { hoveredMenu_ = menu; hovered_ = hovered; hoveredTool_ = tool; if (toolPane) toolPane_ = toolPane; requestUpdate(); }
}
void CtlWindow::contentLeave() { hovered_ = 0; hoveredMenu_ = -1; hoveredTool_ = -1; requestUpdate(); }
void CtlWindow::contentKeyPress(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape) { command(2); return; }
  if (event->key() == Qt::Key_Tab && focus_ && isComposePane(focus_) && prefs::tabInsertsTab()) { editor(focus_).cursor.insertText(QStringLiteral("\t")); editorChanged(focus_); requestUpdate(); return; }
  if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) { const QList<quint32> order = focusOrder(); if (!order.isEmpty()) { const int at = order.indexOf(focus_); focus_ = order[(at + (event->key() == Qt::Key_Backtab ? order.size() - 1 : 1)) % order.size()]; requestUpdate(); } return; }
  CtlObject *o = focus_ ? object(focus_) : nullptr;
  if (!o || !isEditable(*o)) { if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) submitEditor(0); return; }
  const bool singleLine = o->kind == CtlObject::Kind::Edit && !(o->style & EsMultiline);
  if (isComposePane(o->id) && event->key() == Qt::Key_Tab && prefs::tabInsertsTab()) { editor(o->id).cursor.insertText(QStringLiteral("\t")); editorChanged(o->id); requestUpdate(); return; }
  if (isComposePane(o->id) && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && prefs::enterInsertsReturn()) { // Enter = new line, Ctrl+Enter sends
    if (event->modifiers().testFlag(Qt::ControlModifier)) submitEditor(o->id); else { editor(o->id).cursor.insertText(QStringLiteral("\n")); editorChanged(o->id); }
    requestUpdate(); return;
  }
  if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && o->kind == CtlObject::Kind::Edit && (o->style & EsWantReturn)) { editor(o->id).cursor.insertText(QStringLiteral("\n")); editorChanged(o->id); requestUpdate(); return; } // ES_WANTRETURN
  bool submit = false;
  if (editor(o->id).handleKey(event, singleLine, &submit)) { if (submit && !submitEditor(o->id) && !singleLine && o->kind == CtlObject::Kind::Ate) {} editorChanged(o->id); requestUpdate(); return; }
  WindowBase::contentKeyPress(event);
}
bool CtlWindow::event(QEvent *event) {
  if (event->type() == QEvent::InputMethod) { auto *input = static_cast<QInputMethodEvent *>(event); CtlObject *o = focus_ ? object(focus_) : nullptr; if (o && isEditable(*o) && !input->commitString().isEmpty()) { editor(o->id).input(input->commitString(), o->kind == CtlObject::Kind::Edit && !(o->style & EsMultiline)); editorChanged(o->id); requestUpdate(); } event->accept(); return true; }
  return WindowBase::event(event);
}
void CtlWindow::wheelEvent(QWheelEvent *event) {
  CtlObject *o = hit(canvasPoint(event->position().toPoint())); if (!o || o->kind != CtlObject::Kind::Ate) { event->ignore(); return; }
  follow_[o->id] = false; pane(o->id).scroll = qMax(qreal(0), pane(o->id).scroll - event->angleDelta().y() / 2.0); requestUpdate(); event->accept();
}
