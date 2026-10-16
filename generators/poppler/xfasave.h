/*
    SPDX-FileCopyrightText: 2026 OpenAI
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#ifndef OKULAR_XFASAVE_H
#define OKULAR_XFASAVE_H

#include <QByteArray>
#include <QString>

// Synchronize the AcroForm and XFA representations of edited foreground forms.
// Unsupported/ambiguous bindings fail without writing an output file.
QByteArray finalizeXfaSave(const QByteArray &original, const QByteArray &saved, const QString &password, bool foreground, QString *error);

#endif
