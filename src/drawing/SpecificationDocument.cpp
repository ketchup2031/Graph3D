#include "drawing/SpecificationDocument.h"
#include "drawing/Specification.h"

#include <QCoreApplication>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <stdexcept>

namespace graph3d::drawing {
namespace {
QString tr(const char* text) { return QCoreApplication::translate("SpecificationDocument", text); }
bool validText(const QString& text, int limit = 1000) { return text.size() <= limit && !text.contains(QChar::Null); }
int nextNumber(SpecificationDocument& spec)
{
    if (spec.nextPosition >= 1'000'000) { throw std::invalid_argument("No more specification position numbers are available."); }
    return spec.nextPosition++;
}
}

void validateSpecification(const SpecificationDocument& spec)
{
    if (spec.rows.size() > 1000 || spec.history.size() > 10000 || spec.nextPosition < 1 || spec.nextPosition > 1000000
        || !validText(spec.title) || !validText(spec.designation) || !validText(spec.sourcePath, 32768)
        || !validText(spec.drawingPath, 32768) || spec.drawingSheet < 0 || spec.drawingSheet > 63
        || (spec.sourcePath.isEmpty() ? spec.assemblyId != 0 || spec.variant.has_value() || !spec.drawingPath.isEmpty()
                                     : spec.assemblyId < 1 || spec.assemblyId > 10000)
        || (spec.variant && !validText(*spec.variant, 128))) {
        throw std::invalid_argument("Invalid specification or document links.");
    }
    QSet<int> retired;
    for (const auto& [key, pos] : spec.history) {
        if (key.isEmpty() || !validText(key, 32790) || pos < 1 || pos >= spec.nextPosition || retired.contains(pos)) {
            throw std::invalid_argument("Invalid specification position history.");
        }
        retired.insert(pos);
    }
    QSet<int> positions;
    QSet<QString> keys;
    for (const auto& row : spec.rows) {
        if (row.position < 1 || row.position >= spec.nextPosition || positions.contains(row.position)
            || row.quantity < 0 || row.quantity > 1000000 || !validText(row.section, 128)
            || !validText(row.designation) || !validText(row.name) || !validText(row.material) || !validText(row.note)) {
            throw std::invalid_argument("Use unique positive positions, quantities from 0 to 1,000,000, and valid row text.");
        }
        positions.insert(row.position);
        if (row.key.isEmpty()) {
            if (retired.contains(row.position)) { throw std::invalid_argument("A manual row uses a reserved model position."); }
        } else {
            if (keys.contains(row.key) || !spec.history.contains(row.key) || spec.history.at(row.key) != row.position) {
                throw std::invalid_argument("A model row has an inconsistent position.");
            }
            keys.insert(row.key);
        }
    }
}

void mergeSpecification(SpecificationDocument& spec, const AssemblyDrawing& model)
{
    auto updated = spec;
    if (updated.title.isEmpty()) { updated.title = model.name; }
    if (updated.designation.isEmpty()) { updated.designation = model.designation; }
    std::map<QString, SpecRow> prior;
    std::vector<SpecRow> rows;
    for (const auto& row : updated.rows) {
        if (row.key.isEmpty()) { rows.push_back(row); }
        else { prior.emplace(row.key, row); }
    }
    for (const auto& row : model.rows) {
        auto position = updated.history.find(row.key);
        if (position == updated.history.end()) {
            const auto number = nextNumber(updated);
            position = updated.history.emplace(row.key, number).first;
        }
        const auto old = prior.find(row.key);
        const auto quantity = static_cast<int>(row.instances.size());
        if (old != prior.end()) {
            auto kept = old->second;
            kept.quantity = quantity;
            rows.push_back(std::move(kept));
        } else {
            rows.push_back({row.key, row.standard ? tr("Standard items") : tr("Parts"), row.designation, row.name, row.material,
                row.note, position->second, quantity});
        }
    }
    std::ranges::sort(rows, {}, &SpecRow::position);
    updated.rows = std::move(rows);
    validateSpecification(updated);
    spec = std::move(updated);
}

QStringList auditSpecification(const SpecificationDocument& spec, const AssemblyDrawing* model, const ModelViews* linked)
{
    QStringList issues;
    if (model) {
        for (const auto& row : model->rows) {
            const auto specRow = std::ranges::find(spec.rows, row.key, &SpecRow::key);
            if (specRow == spec.rows.end()) {
                const auto label = row.designation.isEmpty() ? row.name : row.designation + QStringLiteral(" · ") + row.name;
                issues << tr("Missing table row: %1 (%2 in the model).").arg(label).arg(row.instances.size());
            } else if (specRow->quantity != static_cast<int>(row.instances.size())) {
                issues << tr("Position %1: model quantity %2, table quantity %3.")
                    .arg(specRow->position).arg(row.instances.size()).arg(specRow->quantity);
            }
        }
        for (const auto& row : spec.rows) {
            if (!row.key.isEmpty() && std::ranges::find(model->rows, row.key, &SpecificationRow::key) == model->rows.end()) {
                issues << tr("Position %1 no longer exists in the model.").arg(row.position);
            }
        }
    }
    if (linked && linked->assembly) {
        const auto& drawing = *linked->assembly;
        for (const auto& marker : drawing.callouts) {
            const auto row = std::ranges::find(drawing.rows, marker.position, &SpecificationRow::position);
            if (row == drawing.rows.end() || marker.instance < 0 || marker.instance >= static_cast<int>(row->instances.size())) {
                issues << tr("A drawing callout points to an unavailable occurrence at position %1.").arg(marker.position);
            } else if (model && marker.componentSlot) {
                const auto live = std::ranges::find_if(model->rows, [&](const auto& candidate) {
                    return std::ranges::find(candidate.componentSlots, marker.componentSlot) != candidate.componentSlots.end();
                });
                if (live == model->rows.end() || live->key != row->key) {
                    issues << tr("Drawing callout %1 refers to a different component in the current model.").arg(marker.position);
                }
            }
        }
        for (const auto& row : drawing.rows) {
            const auto expected = std::ranges::find(spec.rows, row.key, &SpecRow::key);
            if (expected == spec.rows.end()) {
                issues << tr("Drawing position %1 has no matching row in the table.").arg(row.position);
            } else {
                if (expected->position != row.position) {
                    issues << tr("Drawing position %1 should be %2.").arg(row.position).arg(expected->position);
                }
                if (expected->quantity != static_cast<int>(row.instances.size())) {
                    issues << tr("Position %1: drawing quantity %2, table quantity %3.")
                        .arg(expected->position).arg(row.instances.size()).arg(expected->quantity);
                }
            }
            if (model) {
                const auto current = std::ranges::find(model->rows, row.key, &SpecificationRow::key);
                if (current == model->rows.end() || current->componentSlots != row.componentSlots
                    || current->instances.size() != row.instances.size()) {
                    issues << tr("Drawing position %1 does not match current component occurrences.").arg(row.position);
                }
            }
            if (std::ranges::none_of(drawing.callouts, [&](const auto& marker) {
                return marker.position == row.position && marker.view >= 0 && marker.view < 3 && linked->showBase[marker.view];
            })) {
                issues << tr("Position %1 has no visible callout on the linked drawing sheet.").arg(row.position);
            }
        }
        for (const auto& row : spec.rows) {
            if (!row.key.isEmpty() && std::ranges::find(drawing.rows, row.key, &SpecificationRow::key) == drawing.rows.end()) {
                issues << tr("Table position %1 is missing from the linked drawing sheet.").arg(row.position);
            }
        }
    }
    return issues;
}

void synchronizeDrawingPositions(const SpecificationDocument& spec, AssemblyDrawing& drawing)
{
    validateSpecification(spec);
    const auto previous = drawing;
    auto history = spec.history;
    QSet<int> reserved;
    for (const auto& [key, value] : history) { reserved.insert(value); }
    for (const auto& row : spec.rows) { reserved.insert(row.position); }
    int high = std::max(spec.nextPosition, drawing.nextPosition);
    for (const auto& [key, position] : drawing.positionHistory) {
        if (!history.contains(key) && !reserved.contains(position)) {
            history[key] = position;
            reserved.insert(position);
        }
    }
    for (auto& row : drawing.rows) {
        auto found = history.find(row.key);
        if (found == history.end()) {
            if (high >= 1000000) { throw std::invalid_argument("No more specification position numbers are available."); }
            found = history.emplace(row.key, high++).first;
        }
        row.position = found->second;
    }
    for (auto& marker : drawing.callouts) {
        const auto old = std::ranges::find(previous.rows, marker.position, &SpecificationRow::position);
        if (old != previous.rows.end()) { marker.position = history.at(old->key); }
    }
    drawing.positionHistory = std::move(history);
    drawing.nextPosition = high;
    std::ranges::sort(drawing.rows, {}, &SpecificationRow::position);
}

QByteArray specificationCsv(const SpecificationDocument& spec)
{
    validateSpecification(spec);
    QByteArray output("\xef\xbb\xbf");
    const auto append = [&](QStringList values) {
        for (auto& value : values) {
            // Keep component data literal in spreadsheet applications.
            if (!value.trimmed().isEmpty() && QStringLiteral("=+-@").contains(value.trimmed().front())) { value.prepend(u'\''); }
            value.replace(QStringLiteral("\""), QStringLiteral("\"\""));
            value = QStringLiteral("\"") + value + QStringLiteral("\"");
        }
        output += values.join(u';').toUtf8() + "\r\n";
    };
    QStringList header{tr("Section")};
    for (const auto& cell : specificationHeaders()) { header << cell; }
    append(header);
    auto rows = spec.rows;
    std::ranges::stable_sort(rows, {}, &SpecRow::section);
    for (const auto& row : rows) {
        append({row.section, QString::number(row.position), row.designation, row.name, row.material,
            QString::number(row.quantity), row.note});
    }
    return output;
}

void exportSpecificationPdf(const QString& path, const SpecificationDocument& spec)
{
    validateSpecification(spec);
    AssemblyDrawing table;
    table.name = spec.title;
    table.designation = spec.designation;
    auto rows = spec.rows;
    std::ranges::stable_sort(rows, {}, &SpecRow::section);
    QString section;
    for (const auto& row : rows) {
        if (row.section != section) {
            section = row.section;
            SpecificationRow band;
            band.name = section;
            band.sectionTitle = true;
            table.rows.push_back(std::move(band));
        }
        SpecificationRow item{row.key, row.position, row.designation, row.name, row.material, row.note, {}};
        item.explicitQuantity = row.quantity;
        table.rows.push_back(std::move(item));
    }
    const auto pages = paginateSpecification(table);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { throw std::runtime_error(file.errorString().toStdString()); }
    {
        QPdfWriter pdf(&file);
        pdf.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Portrait, {}, QPageLayout::Millimeter));
        pdf.setResolution(300);
        pdf.setTitle(spec.title);
        QPainter painter(&pdf);
        if (!painter.isActive()) { throw std::runtime_error("Could not start specification PDF export."); }
        for (std::size_t i = 0; i < pages.size(); ++i) {
            if (i && !pdf.newPage()) { throw std::runtime_error("Could not add a specification PDF page."); }
            painter.save();
            painter.scale(pdf.width() / 210., pdf.height() / 297.);
            paintSpecificationPage(painter, table, pages[i], 0, static_cast<int>(i) + 1, static_cast<int>(pages.size()));
            painter.restore();
        }
    }
    if (!file.commit()) { throw std::runtime_error(file.errorString().toStdString()); }
}
}  // namespace graph3d::drawing
