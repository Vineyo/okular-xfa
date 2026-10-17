/*
    SPDX-FileCopyrightText: 2026 OpenAI
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef OKULAR_XFADOCUMENT_H
#define OKULAR_XFADOCUMENT_H

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QRect>
#include <QSizeF>
#include <QString>

#include <functional>
#include <memory>

class QPrinter;

/** XFA layout, rendering and form editing. All calls must be on the application thread.
 *
 * The implementation owns the original bytes for PDFium's entire document lifetime.
 * It is also built without PDFium, in which case load() always returns nullptr.
 */
class XfaDocument final
{
public:
    static bool isAvailable();
    static std::unique_ptr<XfaDocument> load(QByteArray data, const QString &password);
    ~XfaDocument();

    // Foreground XFA retains the original PDF page tree and text coordinates.
    bool hasPdfTextLayer() const;
    int pageCount() const;
    QSizeF pageSize(int page) const;
    QImage render(int page, const QSize &fullSize, const QRect &tile = {}, bool printing = false) const;

    // The editor owns a separate document, leaving the main viewer unchanged.
    std::unique_ptr<XfaDocument> clone() const;
    bool canFillForms() const;
    enum class MouseEvent { Move, Press, Release, DoubleClick };
    // Coordinates are normalized to the displayed, unrotated page, top-left origin.
    bool mouseEvent(int page, MouseEvent type, const QPointF &position, Qt::KeyboardModifiers modifiers = {}, Qt::MouseButtons buttons = {});
    enum FieldKind { NoField, TextField, ButtonField, ChoiceField, OtherField };
    int fieldAt(int page, const QPointF &position) const;
    bool keyPress(int page, int qtKey, Qt::KeyboardModifiers modifiers = {});
    bool typeText(int page, const QString &text);
    bool replaceSelection(int page, const QString &text);
    QString focusedText(int page) const;
    bool hasFocusedTextField(int page) const;
    QString selectedText(int page) const;
    bool selectAll(int page);
    bool undo(int page);
    bool redo(int page);
    bool blur();
    bool isModified() const;
    void setUpdateCallback(std::function<void()> callback);
    QByteArray saveData(QString *error = nullptr);
    bool save(const QString &fileName, QString *error = nullptr);

    enum class PrintResult { Success, NoPages, InvalidPrinter, InvalidPageSize, RenderError, PrinterError };
    PrintResult print(QPrinter &printer, const QList<int> &oneBasedPages, bool fitToPage) const;

    XfaDocument(const XfaDocument &) = delete;
    XfaDocument &operator=(const XfaDocument &) = delete;

private:
    XfaDocument();
    class Private;
    std::unique_ptr<Private> d;
};

#endif
