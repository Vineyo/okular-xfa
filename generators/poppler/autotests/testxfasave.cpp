/*
    SPDX-FileCopyrightText: 2026 OpenAI
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "xfafixture.h"
#include "xfasave.h"

#include <QDomDocument>
#include <QTest>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>

namespace
{
using Object = QPDFObjectHandle;
Object packet(QPDF &pdf, const std::string &name)
{
    auto array = pdf.getRoot().getKey("/AcroForm").getKey("/XFA");
    for (int i = 0; i + 1 < array.getArrayNItems(); i += 2) {
        if (array.getArrayItem(i).getUTF8Value() == name) {
            return array.getArrayItem(i + 1);
        }
    }
    return Object::newNull();
}
QByteArray output(QPDF &pdf)
{
    QPDFWriter writer(pdf);
    writer.setOutputMemory();
    writer.write();
    auto buffer = writer.getBufferSharedPointer();
    return QByteArray(reinterpret_cast<const char *>(buffer->getBuffer()), buffer->getSize());
}
void replace(Object stream, const QByteArray &bytes)
{
    stream.replaceStreamData(std::string(bytes.constData(), bytes.size()), Object::newNull(), Object::newNull());
}
Object field(const std::string &name, Object value)
{
    auto result = Object::newDictionary();
    result.replaceKey("/T", Object::newString(name));
    result.replaceKey("/V", value);
    return result;
}
QByteArray fixture(const QByteArray &control, const QByteArray &data, bool edited, bool radio = false)
{
    const QByteArray base = xfaFixture(1, true);
    QPDF pdf;
    pdf.processMemoryFile("fixture", base.constData(), base.size());
    const QByteArray xml =
        "<template xmlns=\"http://www.xfa.org/schema/xfa-template/3.3/\"><subform name=\"form\"><subform name=\"page\"><bind match=\"none\"/>"
        "<area><area><field name=\"name\"/></area></area><area>" +
        control + "</area></subform></subform></template>";
    replace(packet(pdf, "template"), xml);
    replace(packet(pdf, "datasets"), "<xfa:datasets xmlns:xfa=\"http://www.xfa.org/schema/xfa-data/1.0/\"><xfa:data><form>" + data + "</form></xfa:data></xfa:datasets>");
    auto fields = Object::newArray();
    fields.appendItem(field("form[0].page[0].#area[0].#area[1].name[0]", Object::newString("untouched")));
    auto changed = field("form[0].page[0].#area[2].name[1]", Object::newString(edited ? "changed" : "old"));
    if (radio) {
        changed.replaceKey("/V", Object::newName(edited ? "/1" : "/0"));
        auto kids = Object::newArray();
        for (const auto &state : {"/0", "/1"}) {
            auto widget = Object::newDictionary();
            auto ap = Object::newDictionary();
            auto normal = Object::newDictionary();
            normal.replaceKey(state, Object::newDictionary());
            ap.replaceKey("/N", normal);
            widget.replaceKey("/AP", ap);
            kids.appendItem(widget);
        }
        changed.replaceKey("/Kids", kids);
    }
    fields.appendItem(changed);
    pdf.getRoot().getKey("/AcroForm").replaceKey("/Fields", fields);
    return output(pdf);
}
QStringList values(const QByteArray &pdfBytes)
{
    QPDF pdf;
    pdf.processMemoryFile("saved", pdfBytes.constData(), pdfBytes.size());
    auto buffer = packet(pdf, "datasets").getStreamData();
    QDomDocument xml;
    if (!xml.setContent(QByteArray(reinterpret_cast<const char *>(buffer->getBuffer()), buffer->getSize()))) {
        return {};
    }
    QStringList result;
    const auto nodes = xml.elementsByTagName(QStringLiteral("name"));
    for (int i = 0; i < nodes.size(); ++i) {
        result.append(nodes.at(i).toElement().text());
    }
    return result;
}

QByteArray withExtraTextWidget(const QByteArray &bytes, const QString &name, const QString &value)
{
    QPDF pdf;
    pdf.processMemoryFile("fixture", bytes.constData(), bytes.size());
    auto widget = field(name.toStdString(), Object::newUnicodeString(value.toStdString()));
    widget.replaceKey("/T", Object::newUnicodeString(name.toStdString()));
    widget.replaceKey("/Subtype", Object::newName("/Widget"));
    widget.replaceKey("/FT", Object::newName("/Tx"));
    pdf.getRoot().getKey("/AcroForm").getKey("/Fields").appendItem(widget);
    return output(pdf);
}
}

class XfaSaveTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void standalonePdfTextWidget()
    {
        const QString name = QStringLiteral("文本域20");
        const auto before = withExtraTextWidget(fixture("<field name=\"name\"/>", "<name>untouched</name><name>old</name>", false), name, {});
        const auto after = withExtraTextWidget(fixture("<field name=\"name\"/>", "<name>\nuntouched</name><name>\nold</name>", true), name, QStringLiteral("1990-01-02"));
        QString error;
        const auto saved = finalizeXfaSave(before, after, {}, true, &error);
        QVERIFY2(!saved.isEmpty(), qPrintable(error));
        QCOMPARE(values(saved), QStringList({QStringLiteral("untouched"), QStringLiteral("changed")}));
        QPDF reopened;
        reopened.processMemoryFile("saved", saved.constData(), saved.size());
        const auto widgets = reopened.getRoot().getKey("/AcroForm").getKey("/Fields").getArrayAsVector();
        auto extra = widgets.back();
        QCOMPARE(QString::fromStdString(extra.getKey("/T").getUTF8Value()), name);
        QCOMPARE(QString::fromStdString(extra.getKey("/V").getUTF8Value()), QStringLiteral("1990-01-02"));
        auto data = packet(reopened, "datasets").getStreamData();
        const QByteArray xml(reinterpret_cast<const char *>(data->getBuffer()), data->getSize());
        QVERIFY(!xml.contains(name.toUtf8()));
    }

    void ambiguousPdfTextWidget_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("xfa-name-collision") << QStringLiteral("name");
        QTest::newRow("unresolved-som-path") << QStringLiteral("form[0].missing[0]");
    }

    void ambiguousPdfTextWidget()
    {
        QFETCH(QString, name);
        const auto base = fixture("<field name=\"name\"/>", "<name>untouched</name><name>old</name>", false);
        const auto before = withExtraTextWidget(base, name, {});
        const auto after = withExtraTextWidget(base, name, QStringLiteral("1990-01-02"));
        QString error;
        QVERIFY(finalizeXfaSave(before, after, {}, true, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("unsupported")));
    }

    void transparentAreasAndRepeatedFields()
    {
        const QByteArray control = "<field name=\"name\"/>";
        const auto before = fixture(control, "<name>untouched</name><name>old</name>", false);
        const auto after = fixture(control, "<name>\nuntouched</name><name>\nold</name>", true);
        QString error;
        const auto saved = finalizeXfaSave(before, after, {}, true, &error);
        QVERIFY2(!saved.isEmpty(), qPrintable(error));
        QCOMPARE(values(saved), QStringList({QStringLiteral("untouched"), QStringLiteral("changed")}));
    }
    void radioUsesXfaExportValue()
    {
        const QByteArray control = "<exclGroup name=\"name\"><field><items><text>M</text></items></field><field><items><text>F</text></items></field></exclGroup>";
        const auto before = fixture(control, "<name>untouched</name><name>M</name>", false, true);
        const auto after = fixture(control, "<name>\nuntouched</name><name>\nM</name>", true, true);
        QString error;
        const auto saved = finalizeXfaSave(before, after, {}, true, &error);
        QVERIFY2(!saved.isEmpty(), qPrintable(error));
        QCOMPARE(values(saved), QStringList({QStringLiteral("untouched"), QStringLiteral("F")}));
    }
    void unsupportedBindingFails()
    {
        const QByteArray control = "<field name=\"name\"><bind match=\"dataRef\" ref=\"other\"/></field>";
        const auto before = fixture(control, "<name>untouched</name><name>old</name>", false);
        const auto after = fixture(control, "<name>\nuntouched</name><name>\nold</name>", true);
        QString error;
        QVERIFY(finalizeXfaSave(before, after, {}, true, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("unsupported")));
    }
    void preserveUserWhitespace()
    {
        const auto serialized = fixture("<field name=\"name\"/>", "<name>\n\n leading</name><name>\n trailing </name>", false);
        QString error;
        const auto saved = finalizeXfaSave({}, serialized, {}, false, &error);
        QVERIFY2(!saved.isEmpty(), qPrintable(error));
        QCOMPARE(values(saved), QStringList({QStringLiteral("\n leading"), QStringLiteral(" trailing ")}));
    }
};
QTEST_GUILESS_MAIN(XfaSaveTest)
#include "testxfasave.moc"
