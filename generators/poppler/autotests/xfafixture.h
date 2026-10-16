/*
    SPDX-FileCopyrightText: 2026 OpenAI
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#ifndef OKULAR_XFA_TEST_FIXTURE_H
#define OKULAR_XFA_TEST_FIXTURE_H
#include <QByteArray>
#include <QList>

namespace
{
inline QByteArray xfaStream(const QByteArray &bytes)
{
    return "<< /Length " + QByteArray::number(bytes.size()) + " >>\nstream\n" + bytes + "\nendstream";
}

// Self-contained fixtures: the PDF fallback is a 200x200 black page, while
// dynamic XFA lays out 300x400 pages with a red rectangle and a bound text field.
// No external sample document, network access, or third-party fixture is needed.
inline QByteArray xfaFixture(int pageCount = 1, bool foreground = false, bool malformed = false, bool ordinary = false, bool singleStream = false, const QByteArray &dataValue = "Bound XFA value")
{
    QByteArray body;
    for (int i = 0; i < pageCount; ++i) {
        body += "<subform name=\"page" + QByteArray::number(i) + "\" layout=\"position\" w=\"300pt\" h=\"350pt\">";
        if (i) {
            body += "<breakBefore targetType=\"pageArea\" startNew=\"1\"/>";
        }
        body +=
            "<draw x=\"20pt\" y=\"30pt\" w=\"100pt\" h=\"80pt\">\n"
            "            <value><rectangle><edge presence=\"hidden\"/><fill><color value=\"255,0,0\"/></fill></rectangle></value></draw>\n"
            "            <field name=\"name\" x=\"20pt\" y=\"130pt\" w=\"240pt\" h=\"30pt\">\n"
            "            <ui><textEdit/></ui><font typeface=\"Helvetica\" size=\"14pt\"/>\n"
            "            <value><text>Default</text></value></field></subform>\n";
    }
    QByteArray xml =
        "<template xmlns=\"http://www.xfa.org/schema/xfa-template/3.3/\">\n"
        "        <subform name=\"form\" layout=\"tb\" locale=\"en_US\">\n"
        "        <pageSet><pageArea name=\"Page1\" id=\"Page1\">\n"
        "        <contentArea x=\"0pt\" y=\"0pt\" w=\"300pt\" h=\"400pt\"/>\n"
        "        <medium short=\"300pt\" long=\"400pt\"/></pageArea></pageSet>\n" +
        body + "</subform></template>";
    if (malformed) {
        xml = "<template><broken>";
    }
    QList<QByteArray> objects {
        QByteArray("<< /Type /Catalog /Pages 2 0 R ") + (ordinary ? "" : "/AcroForm 3 0 R ") + (foreground ? "" : "/NeedsRendering true ") + ">>",
        "<< /Type /Pages /Kids [4 0 R] /Count 1 >>",
        "<< /Fields [] /XFA [(preamble) 7 0 R (config) 8 0 R (template) 9 0 R (datasets) 10 0 R (postamble) 11 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Resources << >> /Contents 5 0 R >>",
        xfaStream("0 0 0 rg 0 0 200 200 re f"),
        "<< >>",
        xfaStream("<xdp:xdp xmlns:xdp=\"http://ns.adobe.com/xdp/\">"),
        xfaStream(QByteArray("<config xmlns=\"http://www.xfa.org/schema/xci/3.0/\"><present><pdf><interactive>1</interactive></pdf></present>\n"
                             "            <acrobat><acrobat7><dynamicRender>") +
                  (foreground ? "forbidden" : "required") + "</dynamicRender></acrobat7></acrobat></config>"),
        xfaStream(xml),
        xfaStream("<xfa:datasets xmlns:xfa=\"http://www.xfa.org/schema/xfa-data/1.0/\"><xfa:data><form><page0><name>" + dataValue + "</name></page0></form></xfa:data></xfa:datasets>\n"),
        xfaStream("</xdp:xdp>"),
    };
    if (singleStream) {
        QByteArray xdp;
        for (int index : {6, 7, 8, 9, 10}) {
            const QByteArray &object = objects[index];
            const auto begin = object.indexOf("\nstream\n") + 8;
            xdp += object.mid(begin, object.size() - begin - 10);
        }
        objects[2] = "<< /Fields [] /XFA 12 0 R >>";
        objects.append(xfaStream(xdp));
    }
    QByteArray pdf = "%PDF-1.7\n";
    QList<qsizetype> offsets {0};
    for (qsizetype i = 0; i < objects.size(); ++i) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const qsizetype xref = pdf.size();
    pdf += "xref\n0 " + QByteArray::number(offsets.size()) + "\n0000000000 65535 f \n";
    for (qsizetype i = 1; i < offsets.size(); ++i) {
        pdf += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(offsets.size()) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}
}

#endif
