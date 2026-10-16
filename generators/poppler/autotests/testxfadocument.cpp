/*
    SPDX-FileCopyrightText: 2026 OpenAI
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "xfadocument.h"
#include "xfafixture.h"

#include <QDomDocument>
#include <QFile>
#include <QPrinter>
#include <QTemporaryDir>
#include <QTest>

#include <thread>

#if HAVE_PDFIUM_XFA
#include <cpp/fpdf_scopers.h>
#include <fpdfview.h>
#endif

class XfaDocumentTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void invalidInput();
    void ordinaryPdf();
#if HAVE_PDFIUM_XFA
    void dynamicPages();
    void rendersXfa();
    void singleXdpStream();
    void dataBinding();
    void tiles();
    void invalidRequests();
    void malformedXfa();
    void foreground();
    void ownsInput();
    void multipleDocuments();
    void applicationThreadOnly();
    void printRoundTrip();
    void invalidPrintRequests();
    void editDynamicAndReopen_data();
    void editDynamicAndReopen();
    void failedSaveKeepsChanges();
    void singleStreamSaveRejected();
    void sampleFormRoundTrip();
#else
    void disabled();
#endif
};

void XfaDocumentTest::invalidInput()
{
    QVERIFY(!XfaDocument::load({}, {}));
    QVERIFY(!XfaDocument::load("not a PDF", {}));
}

void XfaDocumentTest::ordinaryPdf()
{
    QVERIFY(!XfaDocument::load(xfaFixture(1, false, false, true), {}));
}

#if HAVE_PDFIUM_XFA
void XfaDocumentTest::dynamicPages()
{
    auto document = XfaDocument::load(xfaFixture(2), {});
    QVERIFY(document);
    QCOMPARE(document->pageCount(), 2);
    QCOMPARE(document->pageSize(0), QSizeF(300, 400));
    QCOMPARE(document->pageSize(1), QSizeF(300, 400));
    QVERIFY(!document->render(1, QSize(300, 400)).isNull());
}

void XfaDocumentTest::rendersXfa()
{
    auto document = XfaDocument::load(xfaFixture(), {});
    QVERIFY(document);
    const QImage image = document->render(0, QSize(300, 400));
    QCOMPARE(image.size(), QSize(300, 400));
    QCOMPARE(image.pixelColor(60, 70), QColor(Qt::red));
    QCOMPARE(image.pixelColor(290, 390), QColor(Qt::white));
    int darkPixels = 0;
    for (int y = 130; y < 160; ++y) {
        for (int x = 20; x < 260; ++x) {
            darkPixels += image.pixelColor(x, y).lightness() < 100;
        }
    }
    QVERIFY(darkPixels > 20);
    const QImage printed = document->render(0, QSize(600, 800), {}, true);
    QCOMPARE(printed.size(), QSize(600, 800));
    QCOMPARE(printed.pixelColor(120, 140), QColor(Qt::red));
}

void XfaDocumentTest::singleXdpStream()
{
    auto document = XfaDocument::load(xfaFixture(2, false, false, false, true), {});
    QVERIFY(document);
    QCOMPARE(document->pageCount(), 2);
    QCOMPARE(document->render(0, QSize(300, 400)).pixelColor(60, 70), QColor(Qt::red));
}

void XfaDocumentTest::dataBinding()
{
    auto first = XfaDocument::load(xfaFixture(1, false, false, false, false, "FIRST"), {});
    auto second = XfaDocument::load(xfaFixture(1, false, false, false, false, "SECOND"), {});
    QVERIFY(first);
    QVERIFY(second);
    const QRect field(20, 130, 240, 30);
    const QImage firstField = first->render(0, QSize(300, 400), field);
    const QImage secondField = second->render(0, QSize(300, 400), field);
    QVERIFY(!firstField.isNull());
    QVERIFY(firstField != secondField);
}

void XfaDocumentTest::tiles()
{
    auto document = XfaDocument::load(xfaFixture(), {});
    QVERIFY(document);
    const QSize size(600, 800);
    const QImage full = document->render(0, size);
    for (const QRect &tile : {QRect(0, 0, 300, 400), QRect(300, 400, 300, 400), QRect(20, 60, 400, 290)}) {
        QCOMPARE(document->render(0, size, tile), full.copy(tile));
    }
}

void XfaDocumentTest::invalidRequests()
{
    auto document = XfaDocument::load(xfaFixture(), {});
    QVERIFY(document);
    QVERIFY(document->pageSize(-1).isEmpty());
    QVERIFY(document->pageSize(1).isEmpty());
    QVERIFY(document->render(-1, QSize(300, 400)).isNull());
    QVERIFY(document->render(1, QSize(300, 400)).isNull());
    QVERIFY(document->render(0, QSize()).isNull());
    QVERIFY(document->render(0, QSize(300, 400), QRect(-1, 0, 20, 20)).isNull());
    QVERIFY(document->render(0, QSize(300, 400), QRect(299, 399, 2, 2)).isNull());
    QVERIFY(document->render(0, QSize(300, 400), QRect(0, 0, -1, 20)).isNull());
    QVERIFY(document->render(0, QSize(1000000, 1000000)).isNull());
}

void XfaDocumentTest::malformedXfa()
{
    QVERIFY(!XfaDocument::load(xfaFixture(1, false, true), {}));
}

void XfaDocumentTest::foreground()
{
    auto document = XfaDocument::load(xfaFixture(1, true), {});
    QVERIFY(document);
    QCOMPARE(document->pageCount(), 1);
    QCOMPARE(document->pageSize(0), QSizeF(200, 200));
    QVERIFY(!document->render(0, QSize(200, 200)).isNull());
}

void XfaDocumentTest::ownsInput()
{
    QByteArray bytes = xfaFixture();
    auto document = XfaDocument::load(bytes, {});
    QVERIFY(document);
    bytes.fill('x');
    bytes.clear();
    QCOMPARE(document->render(0, QSize(300, 400)).pixelColor(60, 70), QColor(Qt::red));
}

void XfaDocumentTest::multipleDocuments()
{
    auto first = XfaDocument::load(xfaFixture(), {});
    auto second = XfaDocument::load(xfaFixture(2), {});
    QVERIFY(first);
    QVERIFY(second);
    first.reset();
    QVERIFY(!second->render(1, QSize(300, 400)).isNull());
    first = XfaDocument::load(xfaFixture(), {});
    QVERIFY(first);
    second.reset();
    QVERIFY(!first->render(0, QSize(300, 400)).isNull());
}

void XfaDocumentTest::applicationThreadOnly()
{
    auto document = XfaDocument::load(xfaFixture(), {});
    QVERIFY(document);
    bool rejected = false;
    std::thread worker([&] { rejected = !XfaDocument::load(xfaFixture(), {}) && document->render(0, QSize(300, 400)).isNull() && document->pageSize(0).isEmpty() && document->pageCount() == 0; });
    worker.join();
    QVERIFY(rejected);
    QVERIFY(!document->render(0, QSize(300, 400)).isNull());
}

void XfaDocumentTest::printRoundTrip()
{
    auto document = XfaDocument::load(xfaFixture(2), {});
    QVERIFY(document);
    QTemporaryDir output;
    QVERIFY(output.isValid());
    for (bool fitToPage : {true, false}) {
        for (const QList<int> &selection : {QList<int> {1, 2}, QList<int> {2}}) {
            QPrinter printer;
            printer.setOutputFormat(QPrinter::PdfFormat);
            printer.setOutputFileName(output.filePath(QStringLiteral("printed.pdf")));
            QCOMPARE(document->print(printer, selection, fitToPage), XfaDocument::PrintResult::Success);
            ScopedFPDFDocument printed(FPDF_LoadDocument(printer.outputFileName().toUtf8().constData(), nullptr));
            QVERIFY(printed);
            QCOMPARE(FPDF_GetPageCount(printed.get()), selection.size());
            for (int i = 0; i < selection.size(); ++i) {
                ScopedFPDFPage page(FPDF_LoadPage(printed.get(), i));
                QVERIFY(page);
                QImage image(600, 800, QImage::Format_RGBA8888);
                image.fill(Qt::white);
                ScopedFPDFBitmap bitmap(FPDFBitmap_CreateEx(image.width(), image.height(), FPDFBitmap_BGRA, image.bits(), static_cast<int>(image.bytesPerLine())));
                QVERIFY(bitmap);
                FPDF_RenderPageBitmap(bitmap.get(), page.get(), 0, 0, image.width(), image.height(), 0, FPDF_REVERSE_BYTE_ORDER);
                bool containsRed = false;
                for (int y = 0; y < image.height() && !containsRed; ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        const QColor color = image.pixelColor(x, y);
                        if (color.red() > 200 && color.green() < 50 && color.blue() < 50) {
                            containsRed = true;
                            break;
                        }
                    }
                }
                QVERIFY(containsRed);
            }
        }
    }
}

void XfaDocumentTest::invalidPrintRequests()
{
    auto document = XfaDocument::load(xfaFixture(), {});
    QVERIFY(document);
    QPrinter printer;
    QCOMPARE(document->print(printer, {}, true), XfaDocument::PrintResult::NoPages);
    QCOMPARE(document->print(printer, {0}, true), XfaDocument::PrintResult::InvalidPageSize);
    QCOMPARE(document->print(printer, {2}, true), XfaDocument::PrintResult::InvalidPageSize);
}

void XfaDocumentTest::editDynamicAndReopen_data()
{
    QTest::addColumn<QString>("text");
    QTest::newRow("latin") << QStringLiteral("Updated XFA");
    QTest::newRow("empty") << QString();
    QTest::newRow("spaces") << QStringLiteral("  padded value  ");
    QTest::newRow("unicode") << QStringLiteral("日本");
}

void XfaDocumentTest::editDynamicAndReopen()
{
    QFETCH(QString, text);
    auto document = XfaDocument::load(xfaFixture(), {});
    QVERIFY(document);
    const QPointF position(80.0 / 300, 145.0 / 400);
    QVERIFY(document->canFillForms());
    QVERIFY(document->fieldAt(0, position) > 0);
    document->mouseEvent(0, XfaDocument::MouseEvent::Press, position);
    document->mouseEvent(0, XfaDocument::MouseEvent::Release, position);
    QVERIFY(document->selectAll(0));
    QVERIFY(document->replaceSelection(0, text));
    QCOMPARE(document->focusedText(0), text);
    QVERIFY(document->isModified());
    QString error;
    const QByteArray saved = document->saveData(&error);
    QVERIFY2(!saved.isEmpty(), qPrintable(error));
    auto reopened = XfaDocument::load(saved, {});
    QVERIFY(reopened);
    reopened->mouseEvent(0, XfaDocument::MouseEvent::Press, position);
    reopened->mouseEvent(0, XfaDocument::MouseEvent::Release, position);
    QCOMPARE(reopened->focusedText(0), text);
}

void XfaDocumentTest::failedSaveKeepsChanges()
{
    auto document = XfaDocument::load(xfaFixture(), {});
    QVERIFY(document);
    const QPointF position(80.0 / 300, 145.0 / 400);
    document->mouseEvent(0, XfaDocument::MouseEvent::Press, position);
    document->mouseEvent(0, XfaDocument::MouseEvent::Release, position);
    document->selectAll(0);
    document->replaceSelection(0, QStringLiteral("Keep my edit"));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    QVERIFY(!document->save(directory.path(), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(document->isModified());
    QVERIFY2(document->save(directory.filePath(QStringLiteral("filled.pdf")), &error), qPrintable(error));
    QVERIFY(!document->isModified());
}

void XfaDocumentTest::singleStreamSaveRejected()
{
    auto document = XfaDocument::load(xfaFixture(1, false, false, false, true), {});
    QVERIFY(document);
    QString error;
    QVERIFY(document->saveData(&error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("single-stream")));
}

void XfaDocumentTest::sampleFormRoundTrip()
{
    const QString fileName = qEnvironmentVariable("OKULAR_XFA_TEST_PDF");
    if (fileName.isEmpty()) {
        QSKIP("Set OKULAR_XFA_TEST_PDF to run the supplied static form regression test.");
    }
    QFile file(fileName);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray original = file.readAll();
    auto document = XfaDocument::load(original, {});
    QVERIFY(document);
    QCOMPARE(document->pageCount(), 3);
    const QSizeF size = document->pageSize(0);
    const QPointF passport(450 / size.width(), 1 - 454 / size.height());
    QVERIFY(document->fieldAt(0, passport) > 0);
    document->mouseEvent(0, XfaDocument::MouseEvent::Press, passport);
    document->mouseEvent(0, XfaDocument::MouseEvent::Release, passport);
    document->selectAll(0);
    QVERIFY(document->replaceSelection(0, QStringLiteral("TEST12345")));
    QCOMPARE(document->focusedText(0), QStringLiteral("TEST12345"));
    const QList<QPair<QPointF, QString>> textEdits {
        {QPointF(250 / size.width(), 1 - 413 / size.height()), QStringLiteral("Test authority")},
        {QPointF(250 / size.width(), 1 - 433 / size.height()), QStringLiteral("Test place")},
        {QPointF(500 / size.width(), 1 - 616 / size.height()), QStringLiteral("日本")},
        // These later-added PDF widgets have no counterpart in the XFA template.
        {QPointF(155 / size.width(), 1 - 555 / size.height()), QStringLiteral("1990-01-02")},
        {QPointF(500 / size.width(), 1 - 413 / size.height()), QStringLiteral("2030-10-16")},
    };
    for (const auto &[position, text] : textEdits) {
        QVERIFY(document->fieldAt(0, position) > 0);
        document->mouseEvent(0, XfaDocument::MouseEvent::Move, position);
        document->mouseEvent(0, XfaDocument::MouseEvent::Press, position);
        document->mouseEvent(0, XfaDocument::MouseEvent::Release, position);
        QVERIFY(document->selectAll(0));
        QVERIFY(document->replaceSelection(0, text));
        QCOMPARE(document->focusedText(0), text);
    }
    const QPointF female(168 / size.width(), 1 - 534 / size.height());
    document->mouseEvent(0, XfaDocument::MouseEvent::Press, female);
    document->mouseEvent(0, XfaDocument::MouseEvent::Release, female);
    const QPointF nationality(250 / size.width(), 1 - 515 / size.height());
    QCOMPARE(document->fieldAt(0, nationality), int(XfaDocument::ChoiceField));
    document->mouseEvent(0, XfaDocument::MouseEvent::Press, nationality);
    document->mouseEvent(0, XfaDocument::MouseEvent::Release, nationality);
    QVERIFY(document->keyPress(0, Qt::Key_Down));
    document->keyPress(0, Qt::Key_Return);
    QString error;
    const QByteArray saved = document->saveData(&error);
    QVERIFY2(!saved.isEmpty(), qPrintable(error));
    auto reopened = XfaDocument::load(saved, {});
    QVERIFY(reopened);
    reopened->mouseEvent(0, XfaDocument::MouseEvent::Press, passport);
    reopened->mouseEvent(0, XfaDocument::MouseEvent::Release, passport);
    QCOMPARE(reopened->focusedText(0), QStringLiteral("TEST12345"));
    for (const auto &[position, text] : textEdits) {
        reopened->mouseEvent(0, XfaDocument::MouseEvent::Press, position);
        reopened->mouseEvent(0, XfaDocument::MouseEvent::Release, position);
        QCOMPARE(reopened->focusedText(0), text);
    }
    // Inspect persisted XFA data as well: matching AcroForm appearances alone
    // cannot prove that radio export values or repeated fields were saved.
    ScopedFPDFDocument persisted(FPDF_LoadMemDocument64(saved.constData(), saved.size(), nullptr));
    QVERIFY(persisted);
    QByteArray datasets;
    for (int i = 0; i < FPDF_GetXFAPacketCount(persisted.get()); ++i) {
        char name[128] {};
        FPDF_GetXFAPacketName(persisted.get(), i, name, sizeof(name));
        if (QByteArray(name) != "datasets") {
            continue;
        }
        unsigned long length = 0;
        QVERIFY(FPDF_GetXFAPacketContent(persisted.get(), i, nullptr, 0, &length));
        datasets.resize(length);
        QVERIFY(FPDF_GetXFAPacketContent(persisted.get(), i, datasets.data(), length, &length));
    }
    QDomDocument data;
    QVERIFY(data.setContent(datasets));
    const auto texts = [&data](const QString &name) {
        QStringList result;
        const auto nodes = data.elementsByTagName(name);
        for (int i = 0; i < nodes.size(); ++i) {
            result.append(nodes.at(i).toElement().text());
        }
        return result;
    };
    QCOMPARE(texts(QStringLiteral("T49")), QStringList({QStringLiteral("TEST12345")}));
    QCOMPARE(texts(QStringLiteral("T57")), QStringList({QStringLiteral("Test authority"), QStringLiteral("Test place")}));
    QCOMPARE(texts(QStringLiteral("tyuubun")).first(), QStringLiteral("日本"));
    QCOMPARE(texts(QStringLiteral("RB1")).first(), QStringLiteral("F"));
    QCOMPARE(texts(QStringLiteral("T50")).first(), QStringLiteral("SAR"));
    const QString output = qEnvironmentVariable("OKULAR_XFA_TEST_OUTPUT");
    if (!output.isEmpty()) {
        QFile result(output);
        QVERIFY(result.open(QIODevice::WriteOnly));
        QCOMPARE(result.write(saved), saved.size());
    }
    QVERIFY(file.seek(0));
    QCOMPARE(file.readAll(), original);
}
#else
void XfaDocumentTest::disabled()
{
    QVERIFY(!XfaDocument::load(xfaFixture(), {}));
}
#endif

QTEST_MAIN(XfaDocumentTest)
#include "testxfadocument.moc"
