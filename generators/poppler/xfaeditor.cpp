/*
    SPDX-FileCopyrightText: 2026 OpenAI
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "xfaeditor.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextCursor>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

XfaCanvas::XfaCanvas(XfaDocument *document, QWidget *parent)
    : QWidget(parent)
    , m_document(document)
{
    setObjectName(QStringLiteral("xfaCanvas"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_InputMethodEnabled);
    m_textInput = new QPlainTextEdit(this);
    m_textInput->setObjectName(QStringLiteral("xfaFieldText"));
    m_textInput->setPlaceholderText(tr("Click a text field in the form to edit its value here."));
    m_textInput->setReadOnly(true);
    m_textInput->setMaximumHeight(75);
    m_textInput->hide();
    m_textInput->installEventFilter(this);
    connect(m_textInput, &QPlainTextEdit::textChanged, this, &XfaCanvas::syncTextInput);
    m_document->setUpdateCallback([this] { invalidate(); });
    updateSize();
}

XfaCanvas::~XfaCanvas()
{
    detachDocument();
}

void XfaCanvas::detachDocument()
{
    if (m_document) {
        m_document->setUpdateCallback({});
    }
    m_document = nullptr;
}

bool XfaCanvas::commitField()
{
    if (!m_document || !m_document->blur()) {
        return false;
    }
    refreshTextInput();
    invalidate();
    return true;
}

void XfaCanvas::invalidate()
{
    // PDFium invokes host callbacks while its own state is being updated. Query
    // it only after the callback returns, never recursively from an FFI callback.
    if (m_updatePending) {
        return;
    }
    m_updatePending = true;
    QTimer::singleShot(0, this, [this] {
        m_updatePending = false;
        if (!m_document) {
            return;
        }
        m_page = std::clamp(m_page, 0, std::max(0, m_document->pageCount() - 1));
        m_image = {};
        updateSize();
        update();
        Q_EMIT stateChanged();
    });
}

void XfaCanvas::updateSize()
{
    if (!m_document) {
        return;
    }
    const QSizeF page = m_document->pageSize(m_page);
    if (page.isEmpty()) {
        return;
    }
    // Keep screen allocations bounded even for pathological page dimensions.
    const double scale = std::min(m_zoom, 10000.0 / std::max(page.width(), page.height()));
    const QSize size = (page * scale).toSize().expandedTo(QSize(1, 1));
    if (size != this->size()) {
        m_image = {};
        setFixedSize(size);
    }
}

bool XfaCanvas::setPage(int page)
{
    if (!m_document || page < 0 || page >= m_document->pageCount()) {
        return false;
    }
    if (page == m_page) {
        return true;
    }
    if (!m_document->blur()) {
        return false;
    }
    m_page = page;
    m_editingText = false;
    m_textInput->setReadOnly(true);
    m_preedit.clear();
    invalidate();
    updateSize();
    return true;
}

void XfaCanvas::setZoom(double zoom)
{
    if (!std::isfinite(zoom)) {
        return;
    }
    m_zoom = std::clamp(zoom, 0.5, 3.0);
    m_image = {};
    updateSize();
    update();
}

QPointF XfaCanvas::normalized(const QPointF &position) const
{
    return QPointF(position.x() / width(), position.y() / height());
}

void XfaCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::white);
    if (!m_document) {
        return;
    }
    const qreal ratio = devicePixelRatioF();
    if (m_image.isNull() || m_image.devicePixelRatio() != ratio) {
        m_image = m_document->render(m_page, (QSizeF(size()) * ratio).toSize());
        m_image.setDevicePixelRatio(ratio);
    }
    if (m_image.isNull()) {
        painter.drawText(rect(), Qt::AlignCenter, tr("This page could not be rendered."));
    } else {
        painter.drawImage(QPoint(0, 0), m_image);
    }
    if (!m_preedit.isEmpty()) {
        const QPointF position(m_lastPoint.x() * width(), m_lastPoint.y() * height());
        const QRectF box(position, QSizeF(fontMetrics().horizontalAdvance(m_preedit) + 12, fontMetrics().height() + 6));
        painter.fillRect(box, palette().base());
        painter.setPen(palette().text().color());
        painter.drawText(box.adjusted(5, 0, -5, 0), Qt::AlignVCenter, m_preedit);
        painter.drawLine(box.bottomLeft(), box.bottomRight());
    }
}

void XfaCanvas::dispatchMouse(QMouseEvent *event, XfaDocument::MouseEvent type)
{
    if (!m_document) {
        return;
    }
    m_lastPoint = normalized(event->position());
    m_document->mouseEvent(m_page, type, m_lastPoint, event->modifiers(), event->buttons());
    if (type == XfaDocument::MouseEvent::Release || type == XfaDocument::MouseEvent::DoubleClick) {
        const int fieldType = m_document->fieldAt(m_page, m_lastPoint);
        if (fieldType == XfaDocument::TextField) {
            refreshTextInput();
        } else {
            m_editingText = false;
            m_textInput->setReadOnly(true);
        }
    }
    invalidate();
    event->accept();
}

void XfaCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        dispatchMouse(event, XfaDocument::MouseEvent::Press);
    } else {
        QWidget::mousePressEvent(event);
    }
}
void XfaCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        dispatchMouse(event, XfaDocument::MouseEvent::Release);
    } else {
        QWidget::mouseReleaseEvent(event);
    }
}
void XfaCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        dispatchMouse(event, XfaDocument::MouseEvent::DoubleClick);
    }
}
void XfaCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_document) {
        return;
    }
    const int type = m_document->fieldAt(m_page, normalized(event->position()));
    setCursor(type == XfaDocument::ButtonField ? Qt::PointingHandCursor : (type == XfaDocument::TextField ? Qt::IBeamCursor : Qt::ArrowCursor));
    dispatchMouse(event, XfaDocument::MouseEvent::Move);
}

bool XfaCanvas::event(QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        auto key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            keyPressEvent(key);
            return true;
        }
    }
    return QWidget::event(event);
}

bool XfaCanvas::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_textInput && event->type() == QEvent::KeyPress) {
        auto key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab || key->matches(QKeySequence::Save) || key->matches(QKeySequence::SaveAs)) {
            keyPressEvent(key);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void XfaCanvas::refreshTextInput()
{
    m_editingText = m_document && m_document->hasFocusedTextField(m_page);
    m_textInput->setReadOnly(!m_editingText);
    const QSignalBlocker blocker(m_textInput);
    m_textInput->setPlainText(m_editingText ? m_document->focusedText(m_page) : QString());
    m_textInput->moveCursor(QTextCursor::End);
}

void XfaCanvas::syncTextInput()
{
    if (!m_editingText || !m_document) {
        return;
    }
    if (!m_document->hasFocusedTextField(m_page)) {
        refreshTextInput();
        return;
    }
    m_document->selectAll(m_page);
    m_document->replaceSelection(m_page, m_textInput->toPlainText());
    const QString accepted = m_document->focusedText(m_page);
    if (accepted != m_textInput->toPlainText()) {
        const int position = m_textInput->textCursor().position();
        const QSignalBlocker blocker(m_textInput);
        m_textInput->setPlainText(accepted);
        QTextCursor cursor = m_textInput->textCursor();
        cursor.setPosition(std::min(position, int(accepted.size())));
        m_textInput->setTextCursor(cursor);
    }
    invalidate();
}

void XfaCanvas::keyPressEvent(QKeyEvent *event)
{
    // Ignored modifier keys can propagate from the text editor to its parent.
    // Do not send that same event back into the editor recursively.
    if (m_forwardingInput) {
        event->accept();
        return;
    }
    if (!m_document) {
        return;
    }
    if (event->matches(QKeySequence::Save) || event->matches(QKeySequence::SaveAs)) {
        Q_EMIT saveRequested();
    } else if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) {
        m_document->keyPress(m_page, event->key(), event->modifiers());
        refreshTextInput();
    } else if (m_editingText) {
        const QScopedValueRollback<bool> forwarding(m_forwardingInput, true);
        QKeyEvent forwarded(event->type(), event->key(), event->modifiers(), event->text(), event->isAutoRepeat(), event->count());
        QCoreApplication::sendEvent(m_textInput, &forwarded);
    } else if (event->matches(QKeySequence::Copy)) {
        QApplication::clipboard()->setText(m_document->selectedText(m_page));
    } else if (event->matches(QKeySequence::Cut)) {
        QApplication::clipboard()->setText(m_document->selectedText(m_page));
        m_document->replaceSelection(m_page, {});
    } else if (event->matches(QKeySequence::Paste)) {
        m_document->replaceSelection(m_page, QApplication::clipboard()->text());
    } else if (event->matches(QKeySequence::SelectAll)) {
        m_document->selectAll(m_page);
    } else if (event->matches(QKeySequence::Undo)) {
        m_document->undo(m_page);
    } else if (event->matches(QKeySequence::Redo)) {
        m_document->redo(m_page);
    } else {
        m_document->keyPress(m_page, event->key(), event->modifiers());
        if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && !event->text().isEmpty() && event->key() != Qt::Key_Tab && event->key() != Qt::Key_Backtab && event->key() != Qt::Key_Escape &&
            event->key() != Qt::Key_Backspace && event->key() != Qt::Key_Delete && event->key() != Qt::Key_Return && event->key() != Qt::Key_Enter && event->key() != Qt::Key_Space) {
            m_document->typeText(m_page, event->text());
        }
    }
    invalidate();
    if (auto input = QGuiApplication::inputMethod()) {
        input->update(Qt::ImQueryInput);
    }
    event->accept();
}

void XfaCanvas::inputMethodEvent(QInputMethodEvent *event)
{
    if (!m_document) {
        return;
    }
    if (m_editingText) {
        QInputMethodEvent forwarded(event->preeditString(), event->attributes());
        forwarded.setCommitString(event->commitString(), event->replacementStart(), event->replacementLength());
        QCoreApplication::sendEvent(m_textInput, &forwarded);
        invalidate();
        event->accept();
        return;
    }
    if (!event->commitString().isEmpty()) {
        m_document->replaceSelection(m_page, event->commitString());
    }
    m_preedit = event->preeditString();
    invalidate();
    event->accept();
}

QVariant XfaCanvas::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (!m_document) {
        return {};
    }
    if (m_editingText) {
        QVariant result = m_textInput->inputMethodQuery(query);
        if (query == Qt::ImCursorRectangle) {
            QRectF rect = result.toRectF();
            rect.translate(mapFromGlobal(m_textInput->mapToGlobal(QPoint(0, 0))));
            return rect;
        }
        return result;
    }
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImCursorRectangle:
        return QRectF(QPointF(m_lastPoint.x() * width(), m_lastPoint.y() * height()), QSizeF(1, fontMetrics().height()));
    case Qt::ImSurroundingText:
        return m_document->focusedText(m_page);
    case Qt::ImCurrentSelection:
        return m_document->selectedText(m_page);
    default:
        return QWidget::inputMethodQuery(query);
    }
}

void XfaCanvas::focusOutEvent(QFocusEvent *event)
{
    if (event->reason() != Qt::PopupFocusReason) {
        QTimer::singleShot(0, this, [this] {
            if (m_document && !hasFocus() && !m_textInput->hasFocus()) {
                commitField();
            }
        });
    }
    QWidget::focusOutEvent(event);
}

void XfaCanvas::contextMenuEvent(QContextMenuEvent *event)
{
    if (!m_document) {
        return;
    }
    QMenu menu(this);
    menu.addAction(tr("Copy"), this, [this] {
        if (m_editingText) {
            m_textInput->copy();
        } else {
            QApplication::clipboard()->setText(m_document->selectedText(m_page));
        }
    });
    menu.addAction(tr("Paste"), this, [this] {
        if (m_editingText) {
            m_textInput->paste();
        } else {
            m_document->replaceSelection(m_page, QApplication::clipboard()->text());
        }
    });
    menu.addAction(tr("Select All"), this, [this] {
        if (m_editingText) {
            m_textInput->selectAll();
        } else {
            m_document->selectAll(m_page);
        }
    });
    menu.exec(event->globalPos());
    setFocus(Qt::PopupFocusReason);
    invalidate();
}

XfaEditorDialog::XfaEditorDialog(std::unique_ptr<XfaDocument> document, const QString &sourceFile, QWidget *parent)
    : QDialog(parent)
    , m_document(std::move(document))
    , m_sourceFile(sourceFile)
{
    setObjectName(QStringLiteral("xfaEditor"));
    setWindowTitle(tr("Fill XFA Form"));
    resize(950, 850);
    auto layout = new QVBoxLayout(this);
    auto tools = new QToolBar(this);
    auto save = tools->addAction(tr("Save As…"));
    save->setObjectName(QStringLiteral("xfaSaveAs"));
    connect(save, &QAction::triggered, this, [this] { chooseSaveFile(); });
    tools->addSeparator();
    auto previous = tools->addAction(tr("Previous Page"));
    m_page = new QSpinBox(tools);
    m_page->setObjectName(QStringLiteral("xfaPage"));
    m_page->setPrefix(tr("Page "));
    tools->addWidget(m_page);
    auto next = tools->addAction(tr("Next Page"));
    tools->addSeparator();
    auto zoomOut = tools->addAction(tr("Zoom Out"));
    auto zoomIn = tools->addAction(tr("Zoom In"));
    layout->addWidget(tools);
    auto scroll = new QScrollArea(this);
    scroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_canvas = new XfaCanvas(m_document.get(), scroll);
    scroll->setWidget(m_canvas);
    layout->addWidget(scroll, 1);
    layout->addWidget(new QLabel(tr("Selected text field:"), this));
    layout->addWidget(m_canvas->textInput());
    m_canvas->textInput()->show();
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &XfaEditorDialog::reject);
    layout->addWidget(buttons);
    connect(m_canvas, &XfaCanvas::saveRequested, this, [this] { chooseSaveFile(); });
    connect(m_canvas, &XfaCanvas::stateChanged, this, &XfaEditorDialog::updateState);
    connect(m_page, &QSpinBox::valueChanged, this, [this](int page) {
        m_canvas->setPage(page - 1);
        updateState();
        m_canvas->setFocus();
    });
    connect(previous, &QAction::triggered, this, [this] { m_page->setValue(m_page->value() - 1); });
    connect(next, &QAction::triggered, this, [this] { m_page->setValue(m_page->value() + 1); });
    connect(zoomOut, &QAction::triggered, this, [this] { m_canvas->setZoom(m_canvas->zoom() / 1.2); });
    connect(zoomIn, &QAction::triggered, this, [this] { m_canvas->setZoom(m_canvas->zoom() * 1.2); });
    updateState();
    m_canvas->setFocus();
}

XfaEditorDialog::~XfaEditorDialog()
{
    m_canvas->detachDocument();
}

void XfaEditorDialog::updateState()
{
    const QSignalBlocker blocker(m_page);
    m_page->setRange(1, std::max(1, m_document->pageCount()));
    m_page->setValue(m_canvas->pageNumber() + 1);
    m_page->setSuffix(tr(" of %1").arg(m_document->pageCount()));
    setWindowTitle(m_document->isModified() ? tr("Fill XFA Form — Unsaved Changes") : tr("Fill XFA Form"));
    m_status->setText(m_savedFile.isEmpty() ? tr("Click a field, then type or edit its value below. Use Save As to create a filled copy.") : tr("Saved copy: %1").arg(m_savedFile));
}

bool XfaEditorDialog::saveAs(const QString &fileName, QString *error)
{
    const QFileInfo destination(fileName);
    const QFileInfo source(m_sourceFile);
    if (!m_sourceFile.isEmpty() && (destination.absoluteFilePath() == source.absoluteFilePath() || (!destination.canonicalFilePath().isEmpty() && destination.canonicalFilePath() == source.canonicalFilePath()))) {
        if (error) {
            *error = tr("Choose a different filename for the filled copy. The source form is kept unchanged.");
        }
        return false;
    }
    if (!m_canvas->commitField()) {
        if (error) {
            *error = tr("The active field could not be committed. Check its value before saving.");
        }
        return false;
    }
    if (!m_document->save(fileName, error)) {
        return false;
    }
    m_savedFile = destination.absoluteFilePath();
    updateState();
    return true;
}

bool XfaEditorDialog::chooseSaveFile()
{
    QFileInfo source(m_sourceFile);
    const QString suggested = m_savedFile.isEmpty() ? source.absolutePath() + QLatin1Char('/') + source.completeBaseName() + QStringLiteral("_filled.pdf") : m_savedFile;
    const QString fileName = QFileDialog::getSaveFileName(this, tr("Save Filled XFA Form"), suggested, tr("PDF documents (*.pdf)"));
    if (fileName.isEmpty()) {
        return false;
    }
    QString error;
    if (!saveAs(fileName, &error)) {
        QMessageBox::critical(this, tr("Could Not Save XFA Form"), error);
        return false;
    }
    return true;
}

void XfaEditorDialog::reject()
{
    m_document->blur();
    if (m_document->isModified()) {
        const auto choice = QMessageBox::warning(this, tr("Unsaved Form Changes"), tr("Save your changes to a filled copy before closing?"), QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (choice == QMessageBox::Cancel || (choice == QMessageBox::Save && !chooseSaveFile())) {
            return;
        }
    }
    QDialog::reject();
}
