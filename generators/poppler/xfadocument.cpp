/*
    SPDX-FileCopyrightText: 2026 OpenAI
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "xfadocument.h"

#include <QCoreApplication>
#include <QHash>
#include <QPainter>
#include <QPrinter>
#include <QSaveFile>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

bool XfaDocument::isAvailable()
{
    return HAVE_PDFIUM_XFA;
}

#if HAVE_PDFIUM_XFA
#include "xfasave.h"
#include <fpdf_annot.h>
#include <fpdf_formfill.h>
#include <fpdf_fwlevent.h>
#include <fpdf_save.h>
#include <fpdfview.h>

namespace
{
// This adapter requires a self-initializing PDFium distribution (see README.xfa.md).
// Its V8 runtime is initialized once, and never moved between threads or restarted.
class PdfiumRuntime
{
public:
    PdfiumRuntime()
    {
        FPDF_InitLibrary();
    }
    ~PdfiumRuntime()
    {
        FPDF_DestroyLibrary();
    }
};

bool onApplicationThread()
{
    return QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread();
}

bool validPageSize(const QSizeF &size)
{
    return std::isfinite(size.width()) && std::isfinite(size.height()) && size.width() > 0 && size.height() > 0 && size.width() <= 1000000 && size.height() <= 1000000;
}
}

class XfaDocument::Private : public FPDF_FORMFILLINFO
{
public:
    Private()
        : FPDF_FORMFILLINFO {}
    {
        version = 2;
        m_pJsPlatform = &jsPlatform;
        jsPlatform.version = 3;
        // Layout needs a JS platform, but documents get no network, filesystem,
        // email, printing, navigation or dialog capabilities from this host.
        // Timers drive interactive caret/widget updates on the application thread.
        jsPlatform.app_alert = [](IPDF_JSPLATFORM *, FPDF_WIDESTRING, FPDF_WIDESTRING, int, int) { return JSPLATFORM_ALERT_RETURN_CANCEL; };
        FFI_GetPage = [](FPDF_FORMFILLINFO *info, FPDF_DOCUMENT, int index) { return static_cast<Private *>(info)->page(index); };
        FFI_GetCurrentPage = [](FPDF_FORMFILLINFO *info, FPDF_DOCUMENT) {
            auto host = static_cast<Private *>(info);
            return host->page(host->currentPage);
        };
        FFI_GetCurrentPageIndex = [](FPDF_FORMFILLINFO *info, FPDF_DOCUMENT) { return static_cast<Private *>(info)->currentPage; };
        FFI_Invalidate = [](FPDF_FORMFILLINFO *info, FPDF_PAGE, double, double, double, double) { static_cast<Private *>(info)->notify(); };
        FFI_OnChange = [](FPDF_FORMFILLINFO *info) {
            auto host = static_cast<Private *>(info);
            host->modified = true;
            host->notify();
        };
        FFI_PageEvent = [](FPDF_FORMFILLINFO *info, int, FPDF_DWORD) { static_cast<Private *>(info)->notify(); };
        FFI_DisplayCaret = [](FPDF_FORMFILLINFO *info, FPDF_PAGE, FPDF_BOOL visible, double, double, double, double) {
            auto host = static_cast<Private *>(info);
            if (visible) {
                host->textFocused = true;
            }
            host->notify();
        };
        FFI_SetTextFieldFocus = [](FPDF_FORMFILLINFO *info, FPDF_WIDESTRING, FPDF_DWORD, FPDF_BOOL focused) {
            auto host = static_cast<Private *>(info);
            host->textFocused = focused;
            host->notify();
        };
        FFI_SetCursor = [](FPDF_FORMFILLINFO *, int) {};
        FFI_SetTimer = [](FPDF_FORMFILLINFO *info, int elapsed, TimerCallback callback) {
            static int nextId = 1;
            if (nextId == std::numeric_limits<int>::max() || !callback) {
                return 0;
            }
            const int id = nextId++;
            auto host = static_cast<Private *>(info);
            auto timer = new QTimer(QCoreApplication::instance());
            QObject::connect(timer, &QTimer::timeout, timer, [callback, id] { callback(id); });
            host->timers.insert(id, timer);
            timer->start(std::clamp(elapsed, 10, 60000));
            return id;
        };
        FFI_KillTimer = [](FPDF_FORMFILLINFO *info, int id) {
            auto host = static_cast<Private *>(info);
            delete host->timers.take(id);
        };
        FFI_GetPageViewRect = [](FPDF_FORMFILLINFO *, FPDF_PAGE page, double *left, double *top, double *right, double *bottom) {
            *left = *bottom = 0;
            *right = FPDF_GetPageWidthF(page);
            *top = FPDF_GetPageHeightF(page);
        };
    }

    ~Private()
    {
        update = {};
        qDeleteAll(timers);
        timers.clear();
        for (FPDF_PAGE page : pages) {
            if (page) {
                FORM_OnBeforeClosePage(page, form);
                FPDF_ClosePage(page);
            }
        }
        if (form) {
            FPDFDOC_ExitFormFillEnvironment(form);
        }
        if (document) {
            FPDF_CloseDocument(document);
        }
    }

    FPDF_PAGE page(int index)
    {
        const int count = FPDF_GetPageCount(document);
        if (index < 0 || index >= count || count > 100000) {
            return nullptr;
        }
        if (pages.size() < static_cast<size_t>(count)) {
            pages.resize(count);
        }
        if (!pages[index]) {
            pages[index] = FPDF_LoadPage(document, index);
            if (pages[index]) {
                FORM_OnAfterLoadPage(pages[index], form);
            }
        }
        return pages[index];
    }

    void notify()
    {
        if (update) {
            update();
        }
    }

    FPDF_PAGE eventPage(int index)
    {
        if (!onApplicationThread()) {
            return nullptr;
        }
        FPDF_PAGE result = page(index);
        if (result) {
            currentPage = index;
        }
        return result;
    }

    bool position(FPDF_PAGE page, const QPointF &normalized, double *x, double *y) const
    {
        if (!page || !std::isfinite(normalized.x()) || !std::isfinite(normalized.y())) {
            return false;
        }
        return FPDF_DeviceToPage(page, 0, 0, 100000, 100000, 0, qRound(std::clamp(normalized.x(), -1.0, 2.0) * 100000), qRound(std::clamp(normalized.y(), -1.0, 2.0) * 100000), x, y);
    }

    QByteArray bytes;
    IPDF_JSPLATFORM jsPlatform {};
    FPDF_DOCUMENT document = nullptr;
    FPDF_FORMHANDLE form = nullptr;
    std::vector<FPDF_PAGE> pages;
    QString password;
    std::function<void()> update;
    QHash<int, QTimer *> timers;
    int currentPage = 0;
    bool modified = false;
    bool textFocused = false;
};

XfaDocument::XfaDocument()
    : d(std::make_unique<Private>())
{
}

std::unique_ptr<XfaDocument> XfaDocument::load(QByteArray data, const QString &password)
{
    if (!onApplicationThread() || data.isEmpty()) {
        return nullptr;
    }
    static PdfiumRuntime runtime;
    auto result = std::unique_ptr<XfaDocument>(new XfaDocument);
    result->d->bytes = std::move(data);
    auto &d = *result->d;
    d.password = password;
    d.document = FPDF_LoadMemDocument64(d.bytes.constData(), d.bytes.size(), password.toUtf8().constData());
    if (!d.document) {
        return nullptr;
    }
    const int type = FPDF_GetFormType(d.document);
    if (type != FORMTYPE_XFA_FULL && type != FORMTYPE_XFA_FOREGROUND) {
        return nullptr;
    }
    d.form = FPDFDOC_InitFormFillEnvironment(d.document, &d);
    if (!d.form || !FPDF_LoadXFA(d.document)) {
        return nullptr;
    }
    const int count = result->pageCount();
    if (count <= 0 || count > 100000) {
        return nullptr;
    }
    for (int i = 0; i < count; ++i) {
        if (!validPageSize(result->pageSize(i))) {
            return nullptr;
        }
    }
    d.modified = false;
    return result;
}

std::unique_ptr<XfaDocument> XfaDocument::clone() const
{
    return onApplicationThread() ? load(d->bytes, d->password) : nullptr;
}

bool XfaDocument::hasPdfTextLayer() const
{
    return onApplicationThread() && FPDF_GetFormType(d->document) == FORMTYPE_XFA_FOREGROUND;
}

bool XfaDocument::canFillForms() const
{
    return onApplicationThread() && (FPDF_GetDocPermissions(d->document) & (1UL << 8));
}

namespace
{
int flags(Qt::KeyboardModifiers modifiers, Qt::MouseButtons buttons = {})
{
    int result = 0;
    if (modifiers & Qt::ShiftModifier) {
        result |= FWL_EVENTFLAG_ShiftKey;
    }
    if (modifiers & Qt::ControlModifier) {
        result |= FWL_EVENTFLAG_ControlKey;
    }
    if (modifiers & Qt::AltModifier) {
        result |= FWL_EVENTFLAG_AltKey;
    }
    if (modifiers & Qt::MetaModifier) {
        result |= FWL_EVENTFLAG_MetaKey;
    }
    if (buttons & Qt::LeftButton) {
        result |= FWL_EVENTFLAG_LeftButtonDown;
    }
    if (buttons & Qt::RightButton) {
        result |= FWL_EVENTFLAG_RightButtonDown;
    }
    return result;
}
int virtualKey(int key)
{
    switch (key) {
    case Qt::Key_Backspace:
        return FWL_VKEY_Back;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return FWL_VKEY_Tab;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return FWL_VKEY_Return;
    case Qt::Key_Escape:
        return FWL_VKEY_Escape;
    case Qt::Key_Space:
        return FWL_VKEY_Space;
    case Qt::Key_PageUp:
        return FWL_VKEY_Prior;
    case Qt::Key_PageDown:
        return FWL_VKEY_Next;
    case Qt::Key_End:
        return FWL_VKEY_End;
    case Qt::Key_Home:
        return FWL_VKEY_Home;
    case Qt::Key_Left:
        return FWL_VKEY_Left;
    case Qt::Key_Up:
        return FWL_VKEY_Up;
    case Qt::Key_Right:
        return FWL_VKEY_Right;
    case Qt::Key_Down:
        return FWL_VKEY_Down;
    case Qt::Key_Delete:
        return FWL_VKEY_Delete;
    case Qt::Key_Insert:
        return FWL_VKEY_Insert;
    default:
        return key >= Qt::Key_A && key <= Qt::Key_Z ? key : 0;
    }
}
QString formText(FPDF_FORMHANDLE form, FPDF_PAGE page, bool selection)
{
    auto get = selection ? FORM_GetSelectedText : FORM_GetFocusedText;
    const unsigned long size = get(form, page, nullptr, 0);
    if (size < 2 || size > 16 * 1024 * 1024 || size % 2) {
        return {};
    }
    std::vector<unsigned short> buffer(size / 2);
    if (get(form, page, buffer.data(), size) != size) {
        return {};
    }
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(buffer.data()), size / 2 - 1);
}
}

bool XfaDocument::mouseEvent(int index, MouseEvent type, const QPointF &position, Qt::KeyboardModifiers modifiers, Qt::MouseButtons buttons)
{
    if (!canFillForms()) {
        return false;
    }
    FPDF_PAGE page = d->eventPage(index);
    double x, y;
    if (!d->position(page, position, &x, &y)) {
        return false;
    }
    const int modifiersAndButtons = flags(modifiers, buttons);
    switch (type) {
    case MouseEvent::Move:
        return FORM_OnMouseMove(d->form, page, modifiersAndButtons, x, y);
    case MouseEvent::Press:
        // PDFium's widgets need hover state before they accept a press. This
        // also covers clicks synthesized without a preceding mouse move.
        FORM_OnMouseMove(d->form, page, modifiersAndButtons, x, y);
        {
            const bool handled = FORM_OnLButtonDown(d->form, page, modifiersAndButtons, x, y);
            d->textFocused = handled && fieldAt(index, position) == TextField;
            return handled;
        }
    case MouseEvent::Release:
        return FORM_OnLButtonUp(d->form, page, modifiersAndButtons, x, y);
    case MouseEvent::DoubleClick:
        return FORM_OnLButtonDoubleClick(d->form, page, modifiersAndButtons, x, y);
    }
    return false;
}

int XfaDocument::fieldAt(int index, const QPointF &position) const
{
    if (!onApplicationThread()) {
        return -1;
    }
    FPDF_PAGE page = d->page(index);
    double x, y;
    if (!d->position(page, position, &x, &y)) {
        return NoField;
    }
    switch (FPDFPage_HasFormFieldAtPoint(d->form, page, x, y)) {
    case FPDF_FORMFIELD_TEXTFIELD:
    case FPDF_FORMFIELD_XFA_TEXTFIELD:
        return TextField;
    case FPDF_FORMFIELD_CHECKBOX:
    case FPDF_FORMFIELD_RADIOBUTTON:
    case FPDF_FORMFIELD_PUSHBUTTON:
    case FPDF_FORMFIELD_XFA_CHECKBOX:
    case FPDF_FORMFIELD_XFA_PUSHBUTTON:
        return ButtonField;
    case FPDF_FORMFIELD_COMBOBOX:
    case FPDF_FORMFIELD_LISTBOX:
    case FPDF_FORMFIELD_XFA_COMBOBOX:
    case FPDF_FORMFIELD_XFA_LISTBOX:
        return ChoiceField;
    case -1:
    case FPDF_FORMFIELD_UNKNOWN:
        return NoField;
    default:
        return OtherField;
    }
}

bool XfaDocument::keyPress(int index, int key, Qt::KeyboardModifiers modifiers)
{
    if (!canFillForms()) {
        return false;
    }
    FPDF_PAGE page = d->eventPage(index);
    // These are character events for PDFium's controls. Sending Return as a
    // virtual key first clears a combo box's selected index before commit.
    if (page && (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space)) {
        const bool handled = FORM_OnChar(d->form, page, key == Qt::Key_Space ? 32 : 13, flags(modifiers));
        if (handled) {
            d->modified = true;
            d->notify();
        }
        return handled;
    }
    if (page && (key == Qt::Key_Backspace || key == Qt::Key_Delete)) {
        if (!selectedText(index).isEmpty()) {
            FORM_ReplaceSelection(d->form, page, reinterpret_cast<FPDF_WIDESTRING>(u""));
        } else if (key == Qt::Key_Backspace) {
            FORM_OnChar(d->form, page, 8, flags(modifiers));
        } else {
            FORM_OnKeyDown(d->form, page, FWL_VKEY_Delete, flags(modifiers));
        }
        d->modified = true;
        d->notify();
        return true;
    }
    const int code = virtualKey(key);
    if (key == Qt::Key_Backtab) {
        modifiers |= Qt::ShiftModifier;
    }
    const bool wasTextFocused = d->textFocused;
    if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
        d->textFocused = false;
    }
    const bool handled = page && code && FORM_OnKeyDown(d->form, page, code, flags(modifiers));
    if (!handled) {
        d->textFocused = wasTextFocused;
    }
    return handled;
}

bool XfaDocument::typeText(int index, const QString &text)
{
    if (!canFillForms()) {
        return false;
    }
    FPDF_PAGE page = d->eventPage(index);
    if (!page) {
        return false;
    }
    bool handled = false;
    for (char32_t code : text.toUcs4()) {
        handled = FORM_OnChar(d->form, page, code, 0) || handled;
    }
    if (handled && !text.isEmpty()) {
        d->modified = true;
        d->notify();
    }
    return handled;
}

bool XfaDocument::replaceSelection(int index, const QString &text)
{
    if (!canFillForms()) {
        return false;
    }
    FPDF_PAGE page = d->eventPage(index);
    if (!page) {
        return false;
    }
    FORM_ReplaceSelection(d->form, page, reinterpret_cast<FPDF_WIDESTRING>(text.utf16()));
    d->modified = true;
    d->notify();
    return true;
}

QString XfaDocument::focusedText(int index) const
{
    return onApplicationThread() ? formText(d->form, d->page(index), false) : QString();
}

bool XfaDocument::hasFocusedTextField(int index) const
{
    return onApplicationThread() && d->textFocused && d->currentPage == index;
}

QString XfaDocument::selectedText(int index) const
{
    return onApplicationThread() ? formText(d->form, d->page(index), true) : QString();
}

bool XfaDocument::selectAll(int index)
{
    auto page = canFillForms() ? d->eventPage(index) : nullptr;
    return page && FORM_SelectAllText(d->form, page);
}
bool XfaDocument::undo(int index)
{
    auto page = canFillForms() ? d->eventPage(index) : nullptr;
    return page && FORM_Undo(d->form, page);
}
bool XfaDocument::redo(int index)
{
    auto page = canFillForms() ? d->eventPage(index) : nullptr;
    return page && FORM_Redo(d->form, page);
}
bool XfaDocument::blur()
{
    if (!onApplicationThread()) {
        return false;
    }
    if (FORM_ForceToKillFocus(d->form)) {
        d->textFocused = false;
        return true;
    }
    // PDFium also returns false when there was no focused widget. A second
    // save/commit must be allowed in that case. Pure XFA widgets are not
    // exposed by GetFocusedAnnot. Their OnKillFocus always accepts committing;
    // a false result there means the widget was absent or removed by relayout.
    if (FPDF_GetFormType(d->document) == FORMTYPE_XFA_FULL) {
        d->textFocused = false;
        return true;
    }
    int page = -1;
    FPDF_ANNOTATION annotation = nullptr;
    if (!FORM_GetFocusedAnnot(d->form, &page, &annotation)) {
        return false;
    }
    if (annotation) {
        FPDFPage_CloseAnnot(annotation);
        return false;
    }
    d->textFocused = false;
    return true;
}
bool XfaDocument::isModified() const
{
    return onApplicationThread() && d->modified;
}
void XfaDocument::setUpdateCallback(std::function<void()> callback)
{
    d->update = std::move(callback);
}

QByteArray XfaDocument::saveData(QString *error)
{
    if (error) {
        error->clear();
    }
    if (!canFillForms() || !blur()) {
        if (error) {
            *error = QStringLiteral("The active field could not be committed, or filling this document is not permitted.");
        }
        return {};
    }
    if (FPDF_GetXFAPacketCount(d->document) <= 1) {
        if (error) {
            *error = QStringLiteral("Saving edited single-stream XFA is not supported yet. The source document was not changed.");
        }
        return {};
    }
    struct Writer : FPDF_FILEWRITE {
        QByteArray bytes;
        Writer()
            : FPDF_FILEWRITE {}
        {
            version = 1;
            WriteBlock = [](FPDF_FILEWRITE *writer, const void *data, unsigned long size) {
                auto self = static_cast<Writer *>(writer);
                if (size > 512 * 1024 * 1024 || self->bytes.size() > 512 * 1024 * 1024 - qint64(size)) {
                    return 0;
                }
                self->bytes.append(static_cast<const char *>(data), size);
                return 1;
            };
        }
    } writer;
    if (!FPDF_SaveAsCopy(d->document, &writer, 0)) {
        if (error) {
            *error = QStringLiteral("PDFium could not save this XFA document.");
        }
        return {};
    }
    QByteArray output = finalizeXfaSave(d->bytes, writer.bytes, d->password, FPDF_GetFormType(d->document) == FORMTYPE_XFA_FOREGROUND, error);
    if (!output.isEmpty() && !load(output, d->password)) {
        if (error) {
            *error = QStringLiteral("The saved XFA document could not be reopened. No output was written.");
        }
        return {};
    }
    return output;
}

bool XfaDocument::save(const QString &fileName, QString *error)
{
    const QByteArray bytes = saveData(error);
    if (bytes.isEmpty()) {
        return false;
    }
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    d->modified = false;
    d->notify();
    return true;
}

int XfaDocument::pageCount() const
{
    return onApplicationThread() ? FPDF_GetPageCount(d->document) : 0;
}

QSizeF XfaDocument::pageSize(int index) const
{
    if (!onApplicationThread()) {
        return {};
    }
    FPDF_PAGE page = d->page(index);
    return page ? QSizeF(FPDF_GetPageWidthF(page), FPDF_GetPageHeightF(page)) : QSizeF();
}

QImage XfaDocument::render(int index, const QSize &fullSize, const QRect &tile, bool printing) const
{
    if (!onApplicationThread() || fullSize.width() <= 0 || fullSize.height() <= 0) {
        return {};
    }
    const QRect fullRect(QPoint(0, 0), fullSize);
    const QRect region = tile.isNull() ? fullRect : tile;
    // Bound each allocation, including malicious or overflowing tile requests.
    if (!fullRect.contains(region) || region.isEmpty() || qint64(region.width()) * region.height() > 128 * 1024 * 1024) {
        return {};
    }
    FPDF_PAGE page = d->page(index);
    if (!page) {
        return {};
    }
    QImage image(region.size(), QImage::Format_RGBA8888);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::white);
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(image.width(), image.height(), FPDFBitmap_BGRA, image.bits(), static_cast<int>(image.bytesPerLine()));
    if (!bitmap) {
        return {};
    }
    const int flags = FPDF_ANNOT | FPDF_REVERSE_BYTE_ORDER | (printing ? FPDF_PRINTING : 0);
    FPDF_RenderPageBitmap(bitmap, page, -region.x(), -region.y(), fullSize.width(), fullSize.height(), 0, flags);
    FPDF_FFLDraw(d->form, bitmap, page, -region.x(), -region.y(), fullSize.width(), fullSize.height(), 0, flags);
    FPDFBitmap_Destroy(bitmap);
    return image;
}

#else

class XfaDocument::Private
{
};

XfaDocument::XfaDocument() = default;

std::unique_ptr<XfaDocument> XfaDocument::load(QByteArray, const QString &)
{
    return nullptr;
}

int XfaDocument::pageCount() const
{
    return 0;
}

QSizeF XfaDocument::pageSize(int) const
{
    return {};
}

QImage XfaDocument::render(int, const QSize &, const QRect &, bool) const
{
    return {};
}

std::unique_ptr<XfaDocument> XfaDocument::clone() const
{
    return nullptr;
}
bool XfaDocument::hasPdfTextLayer() const
{
    return false;
}
bool XfaDocument::canFillForms() const
{
    return false;
}
bool XfaDocument::mouseEvent(int, MouseEvent, const QPointF &, Qt::KeyboardModifiers, Qt::MouseButtons)
{
    return false;
}
int XfaDocument::fieldAt(int, const QPointF &) const
{
    return -1;
}
bool XfaDocument::keyPress(int, int, Qt::KeyboardModifiers)
{
    return false;
}
bool XfaDocument::typeText(int, const QString &)
{
    return false;
}
bool XfaDocument::replaceSelection(int, const QString &)
{
    return false;
}
QString XfaDocument::focusedText(int) const
{
    return {};
}
bool XfaDocument::hasFocusedTextField(int) const
{
    return false;
}
QString XfaDocument::selectedText(int) const
{
    return {};
}
bool XfaDocument::selectAll(int)
{
    return false;
}
bool XfaDocument::undo(int)
{
    return false;
}
bool XfaDocument::redo(int)
{
    return false;
}
bool XfaDocument::blur()
{
    return false;
}
bool XfaDocument::isModified() const
{
    return false;
}
void XfaDocument::setUpdateCallback(std::function<void()>)
{
}
QByteArray XfaDocument::saveData(QString *error)
{
    if (error) {
        *error = QStringLiteral("XFA support was not enabled at build time.");
    }
    return {};
}
bool XfaDocument::save(const QString &, QString *error)
{
    saveData(error);
    return false;
}

#endif

XfaDocument::~XfaDocument() = default;

XfaDocument::PrintResult XfaDocument::print(QPrinter &printer, const QList<int> &oneBasedPages, bool fitToPage) const
{
    if (oneBasedPages.isEmpty()) {
        return PrintResult::NoPages;
    }
    for (int page : oneBasedPages) {
        if (page < 1 || page > pageCount()) {
            return PrintResult::InvalidPageSize;
        }
    }
    QPainter painter;
    if (!painter.begin(&printer)) {
        return PrintResult::InvalidPrinter;
    }
    for (int i = 0; i < oneBasedPages.size(); ++i) {
        if (i && !printer.newPage()) {
            return PrintResult::PrinterError;
        }
        const int page = oneBasedPages[i] - 1;
        const QSizeF size = pageSize(page);
        if (size.isEmpty()) {
            return PrintResult::InvalidPageSize;
        }
        const QImage image = render(page, (size * (300.0 / 72.0)).toSize(), {}, true);
        if (image.isNull()) {
            return PrintResult::RenderError;
        }
        double scale;
        if (fitToPage) {
            const QRect window = painter.window();
            scale = std::min(window.width() / size.width(), window.height() / size.height());
        } else {
            scale = printer.paperRect(QPrinter::DevicePixel).width() / printer.paperRect(QPrinter::Point).width();
        }
        painter.drawImage(QRectF(QPointF(0, 0), size * scale), image);
    }
    return painter.end() ? PrintResult::Success : PrintResult::PrinterError;
}
