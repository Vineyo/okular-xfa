/*
    SPDX-FileCopyrightText: 2026 OpenAI
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "xfafixture.h"

#include "core/document.h"
#include "core/page.h"
#include "core/textpage.h"
#include "settings_core.h"

#include <QMimeDatabase>
#include <QPrinter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

#include <poppler-qt6.h>

class XfaGeneratorTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        Okular::SettingsCore::instance(QStringLiteral("xfageneratortest"));
    }

    void dynamicPages()
    {
        QTemporaryFile file;
        QVERIFY(writeFixture(file, xfaFixture(2)));
        Okular::Document document(nullptr);
        QCOMPARE(open(document, file), Okular::Document::OpenSuccess);
        QCOMPARE(document.pages(), 2u);
        QVERIFY(document.metaData(QStringLiteral("HasReadOnlyXfaForm")).toBool());
        QVERIFY(!document.metaData(QStringLiteral("HasUnsupportedXfaForm")).toBool());
        QCOMPARE(document.page(0)->width() / document.page(0)->height(), 0.75);
        QVERIFY(document.page(0)->formFields().isEmpty());
        QVERIFY(document.page(1)->formFields().isEmpty());
    }

    void readOnlyEvenWhenDrmIsIgnored()
    {
        QTemporaryFile file;
        QVERIFY(writeFixture(file, xfaFixture()));
        Okular::Document document(nullptr);
        QCOMPARE(open(document, file), Okular::Document::OpenSuccess);
        const bool oldObeyDRM = Okular::SettingsCore::obeyDRM();
        Okular::SettingsCore::setObeyDRM(false);
        const bool readOnly = !document.isAllowed(Okular::AllowModify) && !document.isAllowed(Okular::AllowNotes) && !document.isAllowed(Okular::AllowFillForms);
        Okular::SettingsCore::setObeyDRM(oldObeyDRM);
        QVERIFY(readOnly);
        QVERIFY(!document.supportsSearching());
        QVERIFY(!document.canSaveChanges());
        QVERIFY(!document.canSign());
        QVERIFY(document.metaData(QStringLiteral("DocumentScripts"), QStringLiteral("JavaScript")).toStringList().isEmpty());
    }

    void staticXfaSearch()
    {
        QTemporaryFile file;
        QVERIFY(writeFixture(file, xfaFixture(1, true)));
        Okular::Document document(nullptr);
        QCOMPARE(open(document, file), Okular::Document::OpenSuccess);
        QVERIFY(document.metaData(QStringLiteral("HasReadOnlyXfaForm")).toBool());
        QVERIFY(document.supportsSearching());
        QSignalSpy spy(&document, &Okular::Document::searchFinished);
        document.searchText(42, QStringLiteral("Searchable"), true, Qt::CaseSensitive, Okular::Document::AllDocument, false, QColor(Qt::yellow));
        QTRY_COMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(1).value<Okular::Document::SearchStatus>(), Okular::Document::MatchFound);
        QVERIFY(document.page(0)->hasHighlights(42));
        document.searchText(43, QStringLiteral("not in this form"), true, Qt::CaseSensitive, Okular::Document::AllDocument, false, QColor(Qt::yellow));
        QTRY_COMPARE(spy.count(), 2);
        QCOMPARE(spy.at(1).at(1).value<Okular::Document::SearchStatus>(), Okular::Document::NoMatchFound);
    }

    void sampleFormSearch()
    {
        const QString fileName = qEnvironmentVariable("OKULAR_XFA_TEST_PDF");
        if (fileName.isEmpty()) {
            QSKIP("Set OKULAR_XFA_TEST_PDF to check search on a supplied static XFA form.");
        }
        Okular::Document document(nullptr);
        QCOMPARE(document.openDocument(fileName, QUrl::fromLocalFile(fileName), QMimeDatabase().mimeTypeForName(QStringLiteral("application/pdf"))), Okular::Document::OpenSuccess);
        QVERIFY(document.metaData(QStringLiteral("HasReadOnlyXfaForm")).toBool());
        QVERIFY(document.supportsSearching());
        QSignalSpy spy(&document, &Okular::Document::searchFinished);
        document.searchText(44, QStringLiteral("护照"), true, Qt::CaseInsensitive, Okular::Document::AllDocument, false, QColor(Qt::yellow));
        QTRY_COMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(1).value<Okular::Document::SearchStatus>(), Okular::Document::MatchFound);
    }

    void fallbackOnMalformedXfa()
    {
        QTemporaryFile file;
        QVERIFY(writeFixture(file, xfaFixture(1, false, true)));
        Okular::Document document(nullptr);
        QCOMPARE(open(document, file), Okular::Document::OpenSuccess);
        QCOMPARE(document.pages(), 1u);
        QVERIFY(document.metaData(QStringLiteral("HasUnsupportedXfaForm")).toBool());
        QVERIFY(!document.metaData(QStringLiteral("HasReadOnlyXfaForm")).toBool());
    }

    void normalPdfAfterXfa()
    {
        QTemporaryFile xfa;
        QTemporaryFile pdf;
        QVERIFY(writeFixture(xfa, xfaFixture(2)));
        QVERIFY(writeFixture(pdf, xfaFixture(1, false, false, true)));
        Okular::Document document(nullptr);
        QCOMPARE(open(document, xfa), Okular::Document::OpenSuccess);
        document.closeDocument();
        QCOMPARE(open(document, pdf), Okular::Document::OpenSuccess);
        QCOMPARE(document.pages(), 1u);
        QVERIFY(document.supportsSearching());
        QVERIFY(document.canSaveChanges());
        QVERIFY(!document.metaData(QStringLiteral("HasReadOnlyXfaForm")).toBool());
    }

    void printsXfaPageTree()
    {
        QTemporaryFile file;
        QTemporaryDir output;
        QVERIFY(output.isValid());
        QVERIFY(writeFixture(file, xfaFixture(2)));
        Okular::Document document(nullptr);
        QCOMPARE(open(document, file), Okular::Document::OpenSuccess);
        QPrinter printer;
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(output.filePath(QStringLiteral("printed.pdf")));
        QCOMPARE(document.print(printer), Okular::Document::NoPrintError);
        auto printed = Poppler::Document::load(printer.outputFileName());
        QVERIFY(printed);
        QCOMPARE(printed->numPages(), 2);
        for (int i = 0; i < 2; ++i) {
            const QImage image = printed->page(i)->renderToImage(72, 72);
            QVERIFY(!image.isNull());
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

private:
    static bool writeFixture(QTemporaryFile &file, const QByteArray &bytes)
    {
        return file.open() && file.write(bytes) == bytes.size() && file.flush();
    }

    static Okular::Document::OpenResult open(Okular::Document &document, const QTemporaryFile &file)
    {
        return document.openDocument(file.fileName(), QUrl::fromLocalFile(file.fileName()), QMimeDatabase().mimeTypeForName(QStringLiteral("application/pdf")));
    }
};

QTEST_MAIN(XfaGeneratorTest)
#include "testxfagenerator.moc"
