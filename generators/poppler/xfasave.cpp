/*
    SPDX-FileCopyrightText: 2026 OpenAI
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "xfasave.h"

#include <QDomDocument>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QXmlStreamReader>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>

#include <memory>
#include <stdexcept>

namespace
{
QString tag(const QDomElement &node)
{
    return node.localName().isEmpty() ? node.tagName() : node.localName();
}

QDomElement child(const QDomElement &node, const QString &name, int index = 0)
{
    for (QDomElement e = node.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        if (tag(e) == name && index-- == 0) {
            return e;
        }
    }
    return {};
}

struct Segment {
    QString name;
    int index;
};
using Path = QList<Segment>;

struct Binding {
    Path path;
    bool supported = true;
    QDomElement control;
};

struct Context {
    Path path;
    QHash<QString, int> consumed;
    bool supported = true;
};

bool isContainer(const QString &type)
{
    return type == QLatin1String("subform") || type == QLatin1String("subformSet") || type == QLatin1String("area");
}

QList<QDomElement> structureChildren(const QDomElement &parent)
{
    QList<QDomElement> result;
    for (auto e = parent.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        if (isContainer(tag(e)) || tag(e) == QLatin1String("field") || tag(e) == QLatin1String("exclGroup")) {
            result.append(e);
        }
    }
    return result;
}

bool sameStructure(const QDomElement &a, const QDomElement &b, int depth = 0)
{
    if (depth > 128) {
        throw std::runtime_error("The XFA form structure is too deeply nested.");
    }
    const auto left = structureChildren(a);
    const auto right = structureChildren(b);
    if (left.size() != right.size()) {
        return false;
    }
    for (int i = 0; i < left.size(); ++i) {
        if (tag(left[i]) != tag(right[i]) || left[i].attribute(QStringLiteral("name")) != right[i].attribute(QStringLiteral("name")) || !sameStructure(left[i], right[i], depth + 1)) {
            return false;
        }
    }
    return true;
}

// Build SOM paths and data paths together. Transparent layout containers share
// the parent's data context, so repeated field names are matched by occurrence
// across pages, rather than by a potentially ambiguous leaf name.
void bindings(const QDomElement &parent, const QString &prefix, const std::shared_ptr<Context> &context, QHash<QString, Binding> &result, const std::shared_ptr<QHash<QString, int>> &scope = std::make_shared<QHash<QString, int>>())
{
    for (QDomElement e = parent.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        const QString type = tag(e);
        const bool field = type == QLatin1String("field") || type == QLatin1String("exclGroup");
        if (!field && !isContainer(type)) {
            continue;
        }
        const QString name = e.attribute(QStringLiteral("name"));
        const QString component = name.isEmpty() ? QLatin1Char('#') + type : name;
        // Area containers are transparent to XFA name lookup. Adobe's PDF
        // field names retain their path, but number their descendants within
        // the enclosing subform scope (including nested anonymous areas).
        const QString som = prefix + (prefix.isEmpty() ? QString() : QStringLiteral(".")) + component + QLatin1Char('[') + QString::number((*scope)[component]++) + QLatin1Char(']');
        const auto nestedScope = type == QLatin1String("area") || type == QLatin1String("subformSet") ? scope : std::make_shared<QHash<QString, int>>();
        const QDomElement bind = child(e, QStringLiteral("bind"));
        const QString match = bind.attribute(QStringLiteral("match"), QStringLiteral("once"));
        const bool transparent = name.isEmpty() || type == QLatin1String("area") || type == QLatin1String("subformSet") || match == QLatin1String("none");
        const bool supported = context->supported && (bind.isNull() || match == QLatin1String("once") || match == QLatin1String("none")) && !bind.hasAttribute(QStringLiteral("ref"));
        if (field) {
            Binding binding {context->path, supported && !transparent, e};
            if (!transparent) {
                binding.path.append({name, context->consumed[name]++});
            }
            result.insert(som, binding);
        } else if (transparent) {
            if (supported) {
                bindings(e, som, context, result, nestedScope);
            } else {
                auto unsupported = std::make_shared<Context>(*context);
                unsupported->supported = false;
                bindings(e, som, unsupported, result, nestedScope);
            }
        } else {
            auto nested = std::make_shared<Context>();
            nested->path = context->path;
            nested->path.append({name, context->consumed[name]++});
            nested->supported = supported;
            bindings(e, som, nested, result, nestedScope);
        }
    }
}

QString value(QPDFObjectHandle object)
{
    if (object.isNull()) {
        return {};
    }
    if (object.isString()) {
        return QString::fromStdString(object.getUTF8Value());
    }
    if (object.isName()) {
        const QString name = QString::fromStdString(object.getName()).mid(1);
        return name == QLatin1String("Off") ? QString() : name;
    }
    throw std::runtime_error("An edited field has an unsupported value type (for example a multi-selection list).");
}

QString boundValue(QPDFObjectHandle field, const QDomElement &control)
{
    const QString raw = value(field.getKey("/V"));
    if (tag(control) == QLatin1String("exclGroup")) {
        if (raw.isEmpty()) {
            return {};
        }
        auto widgets = field.getKey("/Kids");
        QList<QDomElement> choices;
        for (auto e = control.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
            if (tag(e) == QLatin1String("field")) {
                choices.append(e);
            }
        }
        if (!widgets.isArray() || widgets.getArrayNItems() != choices.size()) {
            throw std::runtime_error("The radio group's PDF widgets do not match its XFA choices.");
        }
        QString result;
        int matches = 0;
        for (int i = 0; i < widgets.getArrayNItems(); ++i) {
            auto appearances = widgets.getArrayItem(i).getKey("/AP").getKey("/N");
            if (appearances.isDictionary() && appearances.hasKey("/" + raw.toStdString())) {
                const auto item = child(choices[i], QStringLiteral("items")).firstChildElement();
                if (item.isNull()) {
                    throw std::runtime_error("An XFA radio choice has no export value.");
                }
                result = item.text();
                ++matches;
            }
        }
        if (matches != 1) {
            throw std::runtime_error("The radio group's PDF state cannot be mapped unambiguously to an XFA export value.");
        }
        return result;
    }
    if (!child(child(control, QStringLiteral("ui")), QStringLiteral("checkButton")).isNull()) {
        const auto items = child(control, QStringLiteral("items"));
        const auto on = items.firstChildElement();
        if (on.isNull()) {
            throw std::runtime_error("An XFA checkbox has no export values.");
        }
        const auto off = on.nextSiblingElement();
        return raw.isEmpty() ? (off.isNull() ? QString() : off.text()) : on.text();
    }
    return raw;
}

void fields(QPDFObjectHandle array, const QString &prefix, QHash<QString, QPDFObjectHandle> &result, int depth = 0)
{
    if (depth > 128) {
        throw std::runtime_error("The PDF form field tree is too deeply nested.");
    }
    if (!array.isArray()) {
        return;
    }
    for (QPDFObjectHandle field : array.getArrayAsVector()) {
        if (!field.isDictionary()) {
            continue;
        }
        auto nameObject = field.getKey("/T");
        if (!nameObject.isString()) {
            continue; // Widget children inherit their parent's field name/value.
        }
        const QString name = prefix + (prefix.isEmpty() ? QString() : QStringLiteral(".")) + QString::fromStdString(nameObject.getUTF8Value());
        if (result.contains(name)) {
            throw std::runtime_error("Duplicate fully qualified PDF form field names prevent safe XFA synchronization.");
        }
        result.insert(name, field);
        fields(field.getKey("/Kids"), name, result, depth + 1);
    }
}

QSet<QString> templateNames(const QDomElement &root)
{
    QSet<QString> names;
    QList<QDomElement> pending {root};
    while (!pending.isEmpty()) {
        const auto element = pending.takeLast();
        if (element.hasAttribute(QStringLiteral("name"))) {
            names.insert(element.attribute(QStringLiteral("name")));
        }
        for (auto e = element.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
            pending.append(e);
        }
    }
    return names;
}

bool isStandalonePdfTextWidget(const QString &name, QPDFObjectHandle field, const QSet<QString> &xfaNames)
{
    // Foreground XFA PDFs can also contain ordinary widgets added by another
    // editor. Their values/appearances are already saved by PDFium and must not
    // be invented in the XFA datasets. Only accept unambiguous top-level text
    // widgets: possible SOM paths or names shared with XFA still fail closed.
    if (name.isEmpty() || xfaNames.contains(name)) {
        return false;
    }
    for (const QChar character : QStringLiteral(".[]#\\$")) {
        if (name.contains(character)) {
            return false;
        }
    }
    return field.getKey("/Subtype").isNameAndEquals("/Widget") && field.getKey("/FT").isNameAndEquals("/Tx") && field.getKey("/Parent").isNull();
}

QPDFObjectHandle packet(QPDF &pdf, const std::string &name)
{
    auto xfa = pdf.getRoot().getKey("/AcroForm").getKey("/XFA");
    if (!xfa.isArray()) {
        throw std::runtime_error("Saving edited foreground XFA currently requires an XFA packet array.");
    }
    for (int i = 0; i + 1 < xfa.getArrayNItems(); i += 2) {
        auto label = xfa.getArrayItem(i);
        if (label.isString() && label.getUTF8Value() == name) {
            auto stream = xfa.getArrayItem(i + 1);
            if (stream.isStream()) {
                return stream;
            }
        }
    }
    return QPDFObjectHandle::newNull();
}

QDomDocument xml(QPDFObjectHandle stream)
{
    if (!stream.isStream()) {
        throw std::runtime_error("A required XFA template or datasets packet is missing.");
    }
    auto buffer = stream.getStreamData();
    const QByteArray bytes(reinterpret_cast<const char *>(buffer->getBuffer()), buffer->getSize());
    // XFA does not need DTDs or external entities. Reject them before XML parsing.
    if (bytes.contains("<!DOCTYPE") || bytes.contains("<!ENTITY")) {
        throw std::runtime_error("XFA packets containing XML DTDs/entities cannot be synchronized.");
    }
    QDomDocument result;
    QXmlStreamReader reader(bytes);
    QList<QDomElement> stack;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            QDomElement element = result.createElementNS(reader.namespaceUri().toString(), reader.qualifiedName().toString());
            for (const auto &ns : reader.namespaceDeclarations()) {
                bool generated = ns.prefix() == reader.prefix() && ns.namespaceUri() == reader.namespaceUri();
                for (const auto &attribute : reader.attributes()) {
                    generated = generated || (!attribute.prefix().isEmpty() && ns.prefix() == attribute.prefix() && ns.namespaceUri() == attribute.namespaceUri());
                }
                if (generated) {
                    continue;
                } // QDom serializes declarations for namespaced nodes itself.
                const QString name = ns.prefix().isEmpty() ? QStringLiteral("xmlns") : QStringLiteral("xmlns:") + ns.prefix().toString();
                element.setAttribute(name, ns.namespaceUri().toString());
            }
            for (const auto &attribute : reader.attributes()) {
                element.setAttributeNS(attribute.namespaceUri().toString(), attribute.qualifiedName().toString(), attribute.value().toString());
            }
            if (stack.isEmpty()) {
                result.appendChild(element);
            } else {
                stack.last().appendChild(element);
            }
            stack.append(element);
            if (stack.size() > 128) {
                throw std::runtime_error("The XFA XML is too deeply nested.");
            }
        } else if (reader.isDTD()) {
            throw std::runtime_error("XML document type declarations are not supported in XFA packets.");
        } else if (reader.isEndElement()) {
            if (!stack.isEmpty()) {
                stack.removeLast();
            }
        } else if (reader.isCharacters() && !stack.isEmpty()) {
            stack.last().appendChild(result.createTextNode(reader.text().toString()));
        } else if (reader.isProcessingInstruction() && !stack.isEmpty()) {
            stack.last().appendChild(result.createProcessingInstruction(reader.processingInstructionTarget().toString(), reader.processingInstructionData().toString()));
        }
    }
    if (reader.hasError() || result.documentElement().isNull()) {
        throw std::runtime_error("An XFA packet contains invalid XML.");
    }
    return result;
}

void setText(QDomElement node, const QString &text)
{
    while (!node.firstChild().isNull()) {
        node.removeChild(node.firstChild());
    }
    node.appendChild(node.ownerDocument().createTextNode(text));
}

QDomElement ensureChild(QDomElement parent, const Segment &segment)
{
    QDomElement result = child(parent, segment.name, segment.index);
    if (!result.isNull()) {
        return result;
    }
    int count = 0;
    for (QDomElement e = parent.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        count += tag(e) == segment.name;
    }
    while (count++ <= segment.index) {
        result = parent.ownerDocument().createElement(segment.name);
        parent.appendChild(result);
    }
    return result;
}

// PDFium's serialized form packet retains template order, including named and
// anonymous layout containers. Only existing value nodes need replacement;
// absent values inherit the synchronized datasets packet.
void updateForm(QDomElement parent, const QString &prefix, const QHash<QString, QString> &changes, const std::shared_ptr<QHash<QString, int>> &scope = std::make_shared<QHash<QString, int>>())
{
    for (QDomElement e = parent.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        const QString type = tag(e);
        const bool field = type == QLatin1String("field") || type == QLatin1String("exclGroup");
        if (!field && !isContainer(type)) {
            continue;
        }
        const QString name = e.attribute(QStringLiteral("name"));
        const QString component = name.isEmpty() ? QLatin1Char('#') + type : name;
        const QString som = prefix + (prefix.isEmpty() ? QString() : QStringLiteral(".")) + component + QLatin1Char('[') + QString::number((*scope)[component]++) + QLatin1Char(']');
        if (changes.contains(som)) {
            QDomElement v = child(e, QStringLiteral("value"));
            if (!v.isNull()) {
                QDomElement item = v.firstChildElement();
                if (item.isNull()) {
                    item = v.ownerDocument().createElementNS(v.namespaceURI(), QStringLiteral("text"));
                    v.appendChild(item);
                }
                setText(item, changes.value(som));
            }
        }
        if (!field) {
            updateForm(e, som, changes, type == QLatin1String("area") || type == QLatin1String("subformSet") ? scope : std::make_shared<QHash<QString, int>>());
        }
    }
}

void replaceXml(QPDFObjectHandle stream, const QDomDocument &document)
{
    const QByteArray bytes = document.toByteArray(-1);
    QXmlStreamReader validator(bytes);
    while (!validator.atEnd()) {
        validator.readNext();
    }
    if (validator.hasError()) {
        throw std::runtime_error("The synchronized XFA XML did not pass validation.");
    }
    stream.replaceStreamData(std::string(bytes.constData(), bytes.size()), QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
}
}

QByteArray finalizeXfaSave(const QByteArray &original, const QByteArray &saved, const QString &password, bool foreground, QString *error)
{
    try {
        QPDF before;
        QPDF after;
        const QByteArray encodedPassword = password.toUtf8();
        if (foreground) {
            before.processMemoryFile("original XFA PDF", original.constData(), original.size(), encodedPassword.constData());
        }
        after.processMemoryFile("edited XFA PDF", saved.constData(), saved.size(), encodedPassword.constData());
        QHash<QString, QPDFObjectHandle> oldFields;
        QHash<QString, QPDFObjectHandle> newFields;
        if (foreground) {
            fields(before.getRoot().getKey("/AcroForm").getKey("/Fields"), {}, oldFields);
            fields(after.getRoot().getKey("/AcroForm").getKey("/Fields"), {}, newFields);
        }
        QHash<QString, QString> changes;
        for (auto it = newFields.begin(); it != newFields.end(); ++it) {
            if (!oldFields.contains(it.key())) {
                throw std::runtime_error("The form field tree changed unexpectedly during editing.");
            }
            // Compare serialized values first so untouched non-scalar values do
            // not restrict otherwise unrelated edits.
            auto oldValue = oldFields.value(it.key()).getKey("/V");
            auto newValue = it.value().getKey("/V");
            if (oldValue.unparse() != newValue.unparse()) {
                changes.insert(it.key(), value(newValue));
            }
        }
        auto templateXml = xml(packet(after, "template"));
        QHash<QString, Binding> mapping;
        bindings(templateXml.documentElement(), {}, std::make_shared<Context>(), mapping);
        const auto xfaNames = templateNames(templateXml.documentElement());
        auto datasetsStream = packet(after, "datasets");
        QDomDocument datasets = xml(datasetsStream);
        auto removeSerializationNewline = [](auto &&self, QDomElement element) -> void {
            if (element.firstChildElement().isNull()) {
                QDomNode first = element.firstChild();
                if (first.isText() && first.nodeValue().startsWith(QLatin1Char('\n'))) {
                    first.setNodeValue(first.nodeValue().mid(1));
                }
            }
            for (QDomElement e = element.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
                self(self, e);
            }
        };
        // PDFium CFX_XMLElement::Save emits a newline after every opening tag,
        // including scalar values. Remove exactly that one generated character;
        // user-entered leading newlines and spaces are retained.
        removeSerializationNewline(removeSerializationNewline, datasets.documentElement());
        QDomElement data = child(datasets.documentElement(), QStringLiteral("data"));
        if (data.isNull()) {
            throw std::runtime_error("The XFA datasets packet has no data element.");
        }
        for (auto it = changes.begin(); it != changes.end();) {
            if (!mapping.contains(it.key()) && isStandalonePdfTextWidget(it.key(), oldFields.value(it.key()), xfaNames) && isStandalonePdfTextWidget(it.key(), newFields.value(it.key()), xfaNames)) {
                it = changes.erase(it);
                continue;
            }
            const Binding binding = mapping.value(it.key());
            if (!mapping.contains(it.key()) || !binding.supported || binding.path.isEmpty()) {
                if (error) {
                    *error = QStringLiteral("Cannot safely synchronize XFA field %1. Its data binding is unsupported; no output was written.").arg(it.key());
                }
                return {};
            }
            QDomElement target = data;
            for (const Segment &segment : binding.path) {
                target = ensureChild(target, segment);
            }
            if (!target.firstChildElement().isNull()) {
                throw std::runtime_error("An edited field maps to a structured XFA data node rather than a text value.");
            }
            it.value() = boundValue(newFields.value(it.key()), binding.control);
            setText(target, it.value());
            ++it;
        }
        replaceXml(datasetsStream, datasets);
        auto formStream = packet(after, "form");
        if (formStream.isStream()) {
            QDomDocument form = xml(formStream);
            if (!changes.isEmpty() && !sameStructure(templateXml.documentElement(), form.documentElement())) {
                throw std::runtime_error("The serialized XFA form state has a different or sparse structure. Its values cannot be synchronized safely; no output was written.");
            }
            removeSerializationNewline(removeSerializationNewline, form.documentElement());
            updateForm(form.documentElement(), {}, changes);
            replaceXml(formStream, form);
        }
        QPDFWriter writer(after);
        writer.setOutputMemory();
        writer.setPreserveEncryption(true);
        writer.write();
        auto output = writer.getBufferSharedPointer();
        return QByteArray(reinterpret_cast<const char *>(output->getBuffer()), output->getSize());
    } catch (const std::exception &exception) {
        if (error) {
            *error = QString::fromUtf8(exception.what());
        }
        return {};
    }
}
