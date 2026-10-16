/*
    SPDX-FileCopyrightText: 2026 OpenAI
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#ifndef OKULAR_XFAEDITOR_H
#define OKULAR_XFAEDITOR_H

#include "xfadocument.h"

#include <QDialog>
#include <QWidget>

class QSpinBox;
class QLabel;
class QPlainTextEdit;

class XfaCanvas final : public QWidget
{
    Q_OBJECT
public:
    explicit XfaCanvas(XfaDocument *document, QWidget *parent = nullptr);
    ~XfaCanvas() override;
    void detachDocument();
    bool commitField();
    int pageNumber() const
    {
        return m_page;
    }
    bool setPage(int page);
    void setZoom(double zoom);
    double zoom() const
    {
        return m_zoom;
    }
    QPlainTextEdit *textInput() const
    {
        return m_textInput;
    }
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

Q_SIGNALS:
    void stateChanged();
    void saveRequested();

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void invalidate();
    void updateSize();
    QPointF normalized(const QPointF &position) const;
    void dispatchMouse(QMouseEvent *event, XfaDocument::MouseEvent type);
    void refreshTextInput();
    void syncTextInput();
    XfaDocument *m_document;
    QImage m_image;
    int m_page = 0;
    double m_zoom = 1.25;
    bool m_updatePending = false;
    QPointF m_lastPoint;
    QString m_preedit;
    QPlainTextEdit *m_textInput;
    bool m_editingText = false;
    bool m_forwardingInput = false;
};

class XfaEditorDialog final : public QDialog
{
    Q_OBJECT
public:
    XfaEditorDialog(std::unique_ptr<XfaDocument> document, const QString &sourceFile, QWidget *parent = nullptr);
    ~XfaEditorDialog() override;
    QString savedFile() const
    {
        return m_savedFile;
    }
    bool saveAs(const QString &fileName, QString *error = nullptr);

public Q_SLOTS:
    void reject() override;

private:
    bool chooseSaveFile();
    void updateState();
    std::unique_ptr<XfaDocument> m_document;
    QString m_sourceFile;
    QString m_savedFile;
    XfaCanvas *m_canvas;
    QSpinBox *m_page;
    QLabel *m_status;
};

#endif
