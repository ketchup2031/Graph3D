#include "io/LinkedComponents.h"

#include "io/FileIO.h"
#include "io/ProjectIO.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <exception>
#include <map>

namespace graph3d::io {
namespace {

QString absoluteName(const QString& path)
{
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

QString pathKey(const QString& path)
{
#ifdef Q_OS_WIN
    return path.toCaseFolded();
#else
    return path;
#endif
}

struct LoadedProject {
    document::Document document;
    QByteArray digest;
};

class SourceLoader {
public:
    explicit SourceLoader(const QString& owner)
    {
        if (!owner.isEmpty()) {
            visited.insert(pathKey(absoluteName(owner)));
        }
    }

    document::LinkedComponent component(const QString& source, int object)
    {
        const auto path = absoluteName(source);
        const auto& file = load(path);
        const auto* entry = file.document.find(object);
        if (!entry || entry->status != document::Status::Ready || !document::isBody(entry->parameters)) {
            fileError(QCoreApplication::translate("LinkedComponents", "Linked object #%1 is unavailable in %2.")
                .arg(object).arg(QDir::toNativeSeparators(path)));
        }
        auto record = file.document.componentRecord(object);
        record.identity.clear();
        if (const auto* assembly = std::get_if<document::Assembly>(&entry->parameters)) {
            for (std::size_t i = 0; i < assembly->components.size(); ++i) {
                const auto* child = file.document.find(assembly->components[i].body);
                record.children[i].identity = portableComponentIdentity(*child, path).toStdString();
            }
        }
        if (record.properties.name.empty()) {
            record.properties.name = QFileInfo(path).completeBaseName().toStdString();
        }
        return {path.toStdString(), object, file.digest.toHex().toStdString(), entry->shape, std::move(record)};
    }

    bool refresh(document::Document& document, int selected, QCryptographicHash* digest = nullptr)
    {
        std::vector<std::pair<int, document::LinkedComponent>> updates;
        for (const auto& entry : document.objects()) {
            if (const auto* source = std::get_if<document::LinkedComponent>(&entry.parameters);
                source && (selected == 0 || entry.id == selected)) {
                auto current = component(QString::fromStdString(source->sourcePath), source->sourceObject);
                if (digest) {
                    digest->addData(QByteArray::fromStdString(current.digest));
                }
                updates.emplace_back(entry.id, std::move(current));
            }
        }
        return document.refreshLinkedComponents(std::move(updates));
    }

private:
    const LoadedProject& load(const QString& path)
    {
        const auto key = pathKey(path);
        if (visited.contains(key)) {
            fileError(QCoreApplication::translate("LinkedComponents", "Circular component reference: %1")
                .arg(QDir::toNativeSeparators(path)));
        }
        if (const auto prior = projects.find(key); prior != projects.end()) {
            return prior->second;
        }
        if (visited.size() >= 16 || projects.size() >= 128) {
            fileError(QCoreApplication::translate("LinkedComponents", "The linked project hierarchy exceeds the update limit."));
        }
        QByteArray bytes;
        try {
            bytes = readFile(path);
        } catch (const std::exception& error) {
            fileError(QCoreApplication::translate("LinkedComponents", "Cannot read component source %1: %2")
                .arg(QDir::toNativeSeparators(path), QString::fromUtf8(error.what())));
        }
        remaining -= bytes.size();
        if (remaining < 0) {
            fileError(QCoreApplication::translate("LinkedComponents", "The linked project hierarchy exceeds the update limit."));
        }
        auto project = decodeProject(bytes, path);
        QCryptographicHash digest(QCryptographicHash::Sha256);
        digest.addData(bytes);
        visited.insert(key);
        refresh(project, 0, &digest);
        visited.remove(key);
        return projects.emplace(key, LoadedProject{std::move(project), digest.result()}).first->second;
    }

    QSet<QString> visited;
    std::map<QString, LoadedProject> projects;
    qint64 remaining = 256 * 1024 * 1024;
};

}  // namespace

document::LinkedComponent readLinkedComponent(const QString& source, int object, const QString& owner)
{
    return SourceLoader(owner).component(source, object);
}

QString portableComponentIdentity(const document::Object& object, const QString& owner)
{
    const auto* external = std::get_if<document::LinkedComponent>(&object.parameters);
    if (!external) {
        return QStringLiteral("object:%1").arg(object.id);
    }
    const auto base = QFileInfo(owner).absoluteDir();
    auto relative = base.relativeFilePath(QString::fromStdString(external->sourcePath));
#ifdef Q_OS_WIN
    relative = relative.toCaseFolded();
#endif
    auto identity = QStringLiteral("link:%1#%2").arg(relative).arg(external->sourceObject);
    if (object.properties != document::PartProperties{}) {
        identity += QStringLiteral(":override:%1").arg(object.id);
    }
    return identity;
}

bool updateLinkedComponents(document::Document& document, const QString& owner, int selected)
{
    return SourceLoader(owner).refresh(document, selected);
}

}  // namespace graph3d::io
