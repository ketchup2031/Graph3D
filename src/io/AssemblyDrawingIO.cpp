#include "io/AssemblyDrawingIO.h"
#include "io/FileIO.h"
#include "io/LinkedComponents.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace graph3d::io {
namespace {

using namespace drawing;
constexpr int positionLimit = 1'000'000;
constexpr int calloutLimit = 1000;

[[noreturn]] void invalid()
{
    fileError(QStringLiteral("Invalid assembly specification or position callout."));
}

QString string(const QJsonValue& value, int limit = 1000)
{
    const auto text = value.toString();
    if (!value.isString() || text.size() > limit || text.contains(QChar::Null)) {
        invalid();
    }
    return text;
}

int bounded(const QJsonValue& value, int min, int max)
{
    const auto integer = value.toInt(-1);
    if (integer < min || integer > max) {
        invalid();
    }
    return integer;
}

QPointF point(const QJsonValue& value)
{
    const auto pair = value.toArray();
    if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble()
        || std::abs(pair[0].toDouble()) > 1.e9 || std::abs(pair[1].toDouble()) > 1.e9) {
        invalid();
    }
    return {pair[0].toDouble(), pair[1].toDouble()};
}

QJsonArray point(QPointF value)
{
    return {value.x(), value.y()};
}

QString instanceKey(const document::ComponentRecord& item, int index)
{
    if (!item.identity.empty()) {
        return QString::fromStdString(item.identity);
    }
    const auto& p = item.properties;
    if (!p.designation.empty()) {
        const auto identity = QJsonArray{QString::fromStdString(p.designation), QString::fromStdString(p.name),
                                         QString::fromStdString(p.material), QString::fromStdString(p.note)};
        return QStringLiteral("designation:") + QString::fromUtf8(QJsonDocument(identity).toJson(QJsonDocument::Compact));
    }
    return QStringLiteral("instance:%1").arg(index + 1);
}

}  // namespace

std::shared_ptr<const AssemblyDrawing> buildAssemblyDrawing(
    const ModelSource& source, const document::Object& object, const AssemblyDrawing* previous)
{
    const auto* assembly = std::get_if<document::Assembly>(&object.parameters);
    const auto* linked = std::get_if<document::LinkedComponent>(&object.parameters);
    if (!assembly && (!linked || linked->record.children.empty())) {
        return {};
    }
    const auto record = source.document.componentRecord(object.id);
    const auto centers = componentViewCenters(object.shape);
    if (centers.size() != record.children.size() || centers.empty() || centers.size() > geometry::maxAssemblyComponents) {
        fileError(QStringLiteral("Assembly geometry does not match its component list."));
    }
    auto result = std::make_shared<AssemblyDrawing>();
    result->name = QString::fromStdString(record.properties.name);
    result->designation = QString::fromStdString(record.properties.designation);
    QHash<QString, int> existing;
    if (previous) {
        result->nextPosition = previous->nextPosition;
        result->includeSpecification = previous->includeSpecification;
        for (const auto& row : previous->rows) {
            existing.insert(row.key, row.position);
            result->positionHistory[row.key] = row.position;
        }
        for (const auto& [key, position] : previous->positionHistory) {
            existing.insert(key, position);
            result->positionHistory[key] = position;
        }
    }
    QHash<QString, std::size_t> indexes;
    for (std::size_t i = 0; i < centers.size(); ++i) {
        const auto& child = record.children[i];
        const auto key = assembly ? portableComponentIdentity(*source.document.find(assembly->components[i].body), source.path)
                                  : instanceKey(child, static_cast<int>(i));
        auto found = indexes.constFind(key);
        if (found == indexes.cend()) {
            const auto& p = child.properties;
            int position = existing.value(key);
            if (position == 0) {
                if (result->nextPosition >= positionLimit) {
                    invalid();
                }
                position = result->nextPosition++;
            }
            auto name = QString::fromStdString(p.name);
            if (name.isEmpty()) {
                name = QStringLiteral("#%1").arg(assembly ? assembly->components[i].body : static_cast<int>(i) + 1);
            }
            result->rows.push_back({key, position, QString::fromStdString(p.designation), name,
                                    QString::fromStdString(p.material), QString::fromStdString(p.note), {}});
            result->rows.back().standard = child.standard;
            result->positionHistory[key] = position;
            found = indexes.insert(key, result->rows.size() - 1);
        }
        std::array<QPointF, 3> projected{};
        for (int view = 0; view < 3; ++view) {
            projected[view] = {centers[i][view].x, centers[i][view].y};
        }
        result->rows[*found].instances.push_back(projected);
        result->rows[*found].componentSlots.push_back(static_cast<int>(i) + 1);
    }
    std::ranges::sort(result->rows, {}, &SpecificationRow::position);
    if (previous) {
        for (const auto& callout : previous->callouts) {
            int slot = callout.componentSlot;
            const auto old = std::ranges::find(previous->rows, callout.position, &SpecificationRow::position);
            if (!slot && old != previous->rows.end() && callout.instance >= 0
                && callout.instance < static_cast<int>(old->componentSlots.size())) {
                slot = old->componentSlots[callout.instance];
            }
            if (slot) {
                for (const auto& row : result->rows) {
                    const auto found = std::ranges::find(row.componentSlots, slot);
                    if (found != row.componentSlots.end()) {
                        if (old == previous->rows.end() || (old->key != row.key && !(old->standard && row.standard))) { break; }
                        auto marker = callout;
                        marker.position = row.position;
                        marker.instance = static_cast<int>(found - row.componentSlots.begin());
                        // Keep legacy equality and let the row supply the slot on the next refresh.
                        if (callout.componentSlot) { marker.componentSlot = slot; }
                        result->callouts.push_back(marker);
                        break;
                    }
                }
                continue;
            }
            const auto row = std::ranges::find(result->rows, callout.position, &SpecificationRow::position);
            if (row != result->rows.end() && callout.instance >= 0
                && static_cast<std::size_t>(callout.instance) < row->instances.size()) {
                result->callouts.push_back(callout);
            }
        }
    }
    return result;
}

AssemblyDrawing readAssemblyDrawing(const QJsonValue& value)
{
    const auto object = value.toObject();
    const auto rows = object.value("rows").toArray();
    const auto callouts = object.value("callouts").toArray();
    if (rows.isEmpty() || rows.size() > geometry::maxAssemblyComponents
        || !object.value("callouts").isArray() || callouts.size() > calloutLimit
        || !object.value("include").isBool()) {
        invalid();
    }
    AssemblyDrawing result;
    result.name = string(object.value("name"));
    result.designation = string(object.value("designation"));
    result.nextPosition = bounded(object.value("next"), 2, positionLimit);
    result.includeSpecification = object.value("include").toBool();
    QSet<QString> keys;
    QSet<int> usedSlots;
    QHash<int, int> quantities;
    int instanceCount = 0;
    for (const auto value : rows) {
        const auto row = value.toObject();
        const auto key = string(row.value("key"), 32790);
        const auto position = bounded(row.value("position"), 1, result.nextPosition - 1);
        const auto instances = row.value("instances").toArray();
        if (key.isEmpty() || keys.contains(key) || quantities.contains(position) || instances.isEmpty()
            || instances.size() > geometry::maxAssemblyComponents - instanceCount) {
            invalid();
        }
        instanceCount += static_cast<int>(instances.size());
        keys.insert(key);
        quantities.insert(position, static_cast<int>(instances.size()));
        SpecificationRow entry{key, position, string(row.value("designation")), string(row.value("name")),
            string(row.value("material")), string(row.value("note")), {}};
        if (row.contains("standard") && !row.value("standard").isBool()) { invalid(); }
        entry.standard = row.value("standard").toBool();
        for (const auto value : instances) {
            const auto views = value.toArray();
            if (views.size() != 3) {
                invalid();
            }
            entry.instances.push_back({point(views[0]), point(views[1]), point(views[2])});
        }
        if (row.contains("slots")) {
            const auto values = row.value("slots").toArray();
            if (values.size() != instances.size()) { invalid(); }
            for (const auto item : values) {
                const int slot = bounded(item, 1, geometry::maxAssemblyComponents);
                if (usedSlots.contains(slot)) { invalid(); }
                usedSlots.insert(slot);
                entry.componentSlots.push_back(slot);
            }
        }
        result.rows.push_back(std::move(entry));
    }
    std::ranges::sort(result.rows, {}, &SpecificationRow::position);
    if (object.contains("history")) {
        const auto entries = object.value("history").toObject();
        if (!object.value("history").isObject() || entries.size() > 10000) { invalid(); }
        QSet<int> used;
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            const int pos = bounded(it.value(), 1, result.nextPosition - 1);
            if (it.key().isEmpty() || it.key().size() > 32790 || used.contains(pos)) { invalid(); }
            used.insert(pos);
            result.positionHistory[it.key()] = pos;
        }
        for (const auto& row : result.rows) {
            if (const auto it = result.positionHistory.find(row.key); it == result.positionHistory.end() || it->second != row.position) { invalid(); }
        }
    }
    for (const auto value : callouts) {
        const auto entry = value.toObject();
        const auto position = bounded(entry.value("position"), 1, result.nextPosition - 1);
        const auto instance = bounded(entry.value("instance"), 0, quantities.value(position) - 1);
        const auto slot = entry.contains("slot") ? bounded(entry.value("slot"), 1, geometry::maxAssemblyComponents) : 0;
        if (slot) {
            const auto row = std::ranges::find(result.rows, position, &SpecificationRow::position);
            if (row == result.rows.end() || row->componentSlots.empty() || row->componentSlots[instance] != slot) { invalid(); }
        }
        result.callouts.push_back({position, instance, bounded(entry.value("view"), 0, 2),
            point(entry.value("offset")), point(entry.value("label")), slot});
    }
    return result;
}

QJsonObject writeAssemblyDrawing(const AssemblyDrawing& assembly)
{
    QJsonArray rows;
    for (const auto& row : assembly.rows) {
        QJsonArray instances;
        for (const auto& views : row.instances) {
            instances.append(QJsonArray{point(views[0]), point(views[1]), point(views[2])});
        }
        QJsonObject value{{"key", row.key}, {"position", row.position}, {"designation", row.designation},
            {"name", row.name}, {"material", row.material}, {"note", row.note}, {"instances", instances}};
        if (row.standard) { value.insert("standard", true); }
        if (!row.componentSlots.empty()) {
            QJsonArray slotValues;
            for (int slot : row.componentSlots) { slotValues.append(slot); }
            value.insert("slots", slotValues);
        }
        rows.append(value);
    }
    QJsonArray callouts;
    for (const auto& marker : assembly.callouts) {
        QJsonObject value{{"position", marker.position}, {"instance", marker.instance}, {"view", marker.view},
            {"offset", point(marker.offset)}, {"label", point(marker.label)}};
        if (marker.componentSlot) { value.insert("slot", marker.componentSlot); }
        callouts.append(value);
    }
    QJsonObject result{{"name", assembly.name}, {"designation", assembly.designation}, {"next", assembly.nextPosition},
                       {"include", assembly.includeSpecification}, {"rows", rows}, {"callouts", callouts}};
    if (!assembly.positionHistory.empty()) {
        QJsonObject history;
        for (const auto& [key, pos] : assembly.positionHistory) { history.insert(key, pos); }
        result.insert("history", history);
    }
    readAssemblyDrawing(result);
    return result;
}

}  // namespace graph3d::io
