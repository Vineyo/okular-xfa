/*
    SPDX-FileCopyrightText: 2026 OpenAI
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "xfaeditor.h"
#include "xfafixture.h"

#include <QClipboard>
#include <QFile>
#include <QInputMethodEvent>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

class XfaEditorTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void mouseKeyboardAndIme()
    {
        auto document = XfaDocument::load(xfaFixture(), {});
        QVERIFY(document);
        XfaCanvas canvas(document.get());
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        const QPoint field(qRound(canvas.width() * 80.0 / 300), qRound(canvas.height() * 145.0 / 400));
        QTest::mouseClick(&canvas, Qt::LeftButton, {}, field);
        QTest::keyClick(&canvas, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(canvas.textInput()->textCursor().selectedText(), document->focusedText(0));
        QTest::keyClicks(&canvas, QStringLiteral("ABC"));
        QCOMPARE(document->focusedText(0), QStringLiteral("ABC"));
        QTest::keyClick(&canvas, Qt::Key_Backspace);
        QCOMPARE(document->focusedText(0), QStringLiteral("AB"));
        QTest::keyClick(&canvas, Qt::Key_Left);
        QTest::keyClicks(&canvas, QStringLiteral("Z"));
        QCOMPARE(document->focusedText(0), QStringLiteral("AZB"));
        QTest::keyClick(&canvas, Qt::Key_Delete);
        QCOMPARE(document->focusedText(0), QStringLiteral("AZ"));
        QTest::keyClick(&canvas, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(&canvas, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("AZ"));
        QInputMethodEvent ime;
        ime.setCommitString(QStringLiteral("日本"));
        QCoreApplication::sendEvent(&canvas, &ime);
        QCOMPARE(document->focusedText(0), QStringLiteral("日本"));
        QVERIFY(document->isModified());
        const QSize oldSize = canvas.size();
        canvas.setZoom(2);
        QVERIFY(canvas.width() > oldSize.width());
        canvas.setZoom(1.25);
        QCOMPARE(canvas.size(), oldSize);
    }

    void saveCopyAndKeepOriginal()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString source = directory.filePath(QStringLiteral("source.pdf"));
        const QString destination = directory.filePath(QStringLiteral("filled.pdf"));
        const QByteArray original = xfaFixture();
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(original), original.size());
        file.close();
        auto document = XfaDocument::load(original, {});
        QVERIFY(document);
        auto edit = document->clone();
        QVERIFY(edit);
        const QPointF point(80.0 / 300, 145.0 / 400);
        edit->mouseEvent(0, XfaDocument::MouseEvent::Press, point);
        edit->mouseEvent(0, XfaDocument::MouseEvent::Release, point);
        edit->selectAll(0);
        edit->replaceSelection(0, QStringLiteral("Filled copy"));
        XfaEditorDialog dialog(std::move(edit), source);
        QString error;
        QVERIFY(!dialog.saveAs(source, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY2(dialog.saveAs(destination, &error), qPrintable(error));
        QCOMPARE(dialog.savedFile(), destination);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
        QFile saved(destination);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        auto reopened = XfaDocument::load(saved.readAll(), {});
        QVERIFY(reopened);
        reopened->mouseEvent(0, XfaDocument::MouseEvent::Press, point);
        reopened->mouseEvent(0, XfaDocument::MouseEvent::Release, point);
        QCOMPARE(reopened->focusedText(0), QStringLiteral("Filled copy"));
    }

    void closeCanBeCancelled()
    {
        auto document = XfaDocument::load(xfaFixture(), {});
        QVERIFY(document);
        const QPointF point(80.0 / 300, 145.0 / 400);
        document->mouseEvent(0, XfaDocument::MouseEvent::Press, point);
        document->mouseEvent(0, XfaDocument::MouseEvent::Release, point);
        document->selectAll(0);
        document->replaceSelection(0, QStringLiteral("Unsaved"));
        XfaEditorDialog dialog(std::move(document), {});
        dialog.show();
        QTimer::singleShot(0, [] {
            auto question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (question) {
                question->button(QMessageBox::Cancel)->click();
            }
        });
        dialog.reject();
        QVERIFY(dialog.isVisible());
        QTimer::singleShot(0, [] {
            auto question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (question) {
                question->button(QMessageBox::Discard)->click();
            }
        });
        dialog.reject();
        QVERIFY(!dialog.isVisible());
    }

    void samplePreview()
    {
        const QString sample = qEnvironmentVariable("OKULAR_XFA_TEST_PDF");
        if (sample.isEmpty()) {
            QSKIP("Set OKULAR_XFA_TEST_PDF to preview the supplied form.");
        }
        QFile file(sample);
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto document = XfaDocument::load(file.readAll(), {});
        QVERIFY(document);
        auto raw = document.get();
        XfaEditorDialog dialog(std::move(document), sample);
        dialog.show();
        auto canvas = dialog.findChild<XfaCanvas *>();
        QVERIFY(canvas);
        const QSizeF size = raw->pageSize(0);
        const QPoint field(qRound(canvas->width() * 450 / size.width()), qRound(canvas->height() * (1 - 454 / size.height())));
        QTest::mouseClick(canvas, Qt::LeftButton, {}, field);
        QTest::keyClick(canvas, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(canvas, QStringLiteral("TEST12345"));
        QCOMPARE(raw->focusedText(0), QStringLiteral("TEST12345"));
        QTRY_VERIFY(raw->isModified());
        const QString screenshot = qEnvironmentVariable("OKULAR_XFA_UI_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(dialog.grab().save(screenshot));
        }
        QTemporaryDir output;
        QString error;
        QVERIFY2(dialog.saveAs(output.filePath(QStringLiteral("sample-filled.pdf")), &error), qPrintable(error));
    }
};

QTEST_MAIN(XfaEditorTest)
#include "testxfaeditor.moc"
