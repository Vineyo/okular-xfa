/*
    SPDX-FileCopyrightText: 2026 OpenAI
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "xfaeditor.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("xfa-editor"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Experimental XFA form filling and Save As editor"));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("pdf"), QStringLiteral("XFA PDF form to fill"));
    parser.process(app);
    if (parser.positionalArguments().size() != 1) {
        parser.showHelp(1);
    }
    const QString path = QFileInfo(parser.positionalArguments().first()).absoluteFilePath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(nullptr, QStringLiteral("Could Not Open Form"), file.errorString());
        return 1;
    }
    auto document = XfaDocument::load(file.readAll(), {});
    file.close();
    if (!document || !document->canFillForms()) {
        QMessageBox::critical(nullptr, QStringLiteral("Could Not Edit Form"), QStringLiteral("This document could not be loaded as an editable XFA form. It may require a password, or form filling may be restricted."));
        return 1;
    }
    XfaEditorDialog editor(std::move(document), path);
    editor.exec();
    return 0;
}
