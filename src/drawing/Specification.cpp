#include "drawing/Specification.h"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QPainter>
#include <QTextLayout>

#include <algorithm>
#include <cmath>

namespace graph3d::drawing {
namespace {

constexpr std::array<double, 6> widths{11, 39, 56, 31, 13, 35};
constexpr double left = 20;
constexpr double tableTop = 32;
constexpr double headerHeight = 9;
constexpr double leading = 4.2;
constexpr double rowPadding = 3;
constexpr double maxY = 278;

QString tr(const char* text) { return QCoreApplication::translate("Specification", text); }

QFont font(int size)
{
    QFont result(QStringLiteral("Sans Serif"));
    result.setPixelSize(size);
    return result;
}

QStringList wrap(const QString& text, double width)
{
    QStringList lines;
    const auto content = text.simplified();
    QTextLayout layout(content, font(3));
    QTextOption options;
    options.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(options);
    layout.beginLayout();
    for (auto line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(width - 3);
        lines << content.mid(line.textStart(), line.textLength()).trimmed();
    }
    layout.endLayout();
    return lines;
}

double height(int lines) { return rowPadding + leading * lines; }

}  // namespace

SpecificationCells specificationHeaders()
{
    return {tr("Pos."), tr("Designation"), tr("Name"), tr("Material"), tr("Qty."), tr("Note")};
}

SpecificationCells specificationCells(const SpecificationRow& row)
{
    return {QString::number(row.position), row.designation, row.name, row.material,
            QString::number(row.instances.size()), row.note};
}

std::vector<SpecificationPage> paginateSpecification(const AssemblyDrawing& assembly)
{
    std::vector<SpecificationPage> pages(1);
    double y = tableTop + headerHeight;
    for (const auto& row : assembly.rows) {
        std::array<QStringList, 6> cells;
        const auto text = specificationCells(row);
        int lines = 1;
        for (std::size_t column = 0; column < cells.size(); ++column) {
            cells[column] = wrap(text[column], widths[column]);
            lines = std::max(lines, static_cast<int>(cells[column].size()));
        }
        int from = 0;
        while (from < lines) {
            const int capacity = static_cast<int>(std::floor((maxY - y - rowPadding) / leading));
            const auto fitsOnEmptyPage = height(lines) <= maxY - tableTop - headerHeight;
            if (capacity < 1 || (from == 0 && capacity < lines && fitsOnEmptyPage && !pages.back().empty())) {
                pages.emplace_back();
                y = tableTop + headerHeight;
                continue;
            }
            const int count = std::min(capacity, lines - from);
            SpecificationBand band{{}, count};
            for (std::size_t column = 0; column < cells.size(); ++column) {
                band.cells[column] = cells[column].mid(from, count);
            }
            pages.back().push_back(std::move(band));
            y += height(count);
            from += count;
        }
    }
    return pages;
}

void paintSpecificationPage(QPainter& p, const AssemblyDrawing& assembly, const SpecificationPage& page,
                            int sheet, int index, int count)
{
    p.fillRect(QRectF(0, 0, 210, 297), Qt::white);
    p.setPen(QPen(Qt::black, .4));
    p.drawRect(QRectF(left, 5, 185, 287));
    auto title = font(6);
    title.setBold(true);
    p.setFont(title);
    p.drawText(QRectF(24, 8, 177, 10), Qt::AlignLeft | Qt::AlignVCenter, tr("Assembly specification"));
    p.setFont(font(4));
    QStringList heading;
    for (const auto& field : {assembly.designation, assembly.name}) {
        if (!field.trimmed().isEmpty()) {
            heading << field;
        }
    }
    p.drawText(QRectF(24, 19, 177, 9), Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetricsF(p.font()).elidedText(heading.join(QStringLiteral(" · ")), Qt::ElideRight, 177));
    p.setFont(font(3));
    double y = tableTop;
    const auto header = specificationHeaders();
    double x = left;
    for (std::size_t column = 0; column < widths.size(); ++column) {
        const QRectF cell(x, y, widths[column], headerHeight);
        p.fillRect(cell, QColor("#e9eef4"));
        p.drawRect(cell);
        p.drawText(cell, Qt::AlignCenter, header[column]);
        x += widths[column];
    }
    y += headerHeight;
    p.setPen(QPen(Qt::black, .18));
    for (const auto& band : page) {
        x = left;
        for (std::size_t column = 0; column < widths.size(); ++column) {
            p.drawRect(QRectF(x, y, widths[column], height(band.lines)));
            for (qsizetype line = 0; line < band.cells[column].size(); ++line) {
                const QRectF cell(x + 1.5, y + rowPadding / 2 + line * leading, widths[column] - 3, leading);
                const auto alignment = column == 0 || column == 4 ? Qt::AlignHCenter : Qt::AlignLeft;
                p.drawText(cell, alignment | Qt::AlignVCenter, band.cells[column][line]);
            }
            x += widths[column];
        }
        y += height(band.lines);
    }
    p.drawText(QRectF(24, 282, 177, 8), Qt::AlignRight | Qt::AlignVCenter,
        tr("Drawing sheet %1 · Specification %2/%3").arg(sheet).arg(index).arg(count));
}

QPointF positionOrigin(const ModelViews& model, const SpecificationRow& row, int instance, int view)
{
    const auto& placement = model.layout[view];
    return placement.center + row.instances[instance][view] * placement.scale;
}

void paintPositionCallouts(QPainter& p, const ModelViews& model)
{
    if (!model.assembly) {
        return;
    }
    p.save();
    p.setFont(font(4));
    p.setPen(QPen(Qt::black, .22));
    for (const auto& marker : model.assembly->callouts) {
        const auto row = std::ranges::find(model.assembly->rows, marker.position, &SpecificationRow::position);
        if (row == model.assembly->rows.end() || marker.view < 0 || marker.view > 2 || !model.showBase[marker.view]
            || marker.instance < 0
            || static_cast<std::size_t>(marker.instance) >= row->instances.size()) {
            continue;
        }
        const auto tip = positionOrigin(model, *row, marker.instance, marker.view)
            + marker.offset * model.layout[marker.view].scale;
        const auto label = QString::number(marker.position);
        const auto shelf = std::max(8.0, QFontMetricsF(p.font()).horizontalAdvance(label) + 3);
        p.drawLine(tip, marker.label);
        p.setBrush(Qt::black);
        p.drawEllipse(tip, .65, .65);
        p.setBrush(Qt::NoBrush);
        p.drawLine(marker.label, marker.label + QPointF(shelf, 0));
        p.drawText(QRectF(marker.label.x(), marker.label.y() - 6, shelf, 5.5), Qt::AlignHCenter | Qt::AlignBottom, label);
    }
    p.restore();
}

}  // namespace graph3d::drawing
