#include "drawing/Draft.h"
#include "drawing/ModelReference.h"
#include "drawing/Specification.h"
#include "drawing/DraftCurves.h"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QLineF>
#include <QPageLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPdfWriter>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace graph3d::drawing {

static void paintDraftTable(QPainter& p, QPointF origin, const DraftTable& table, QColor color,
                            double labelFraction = 0);

namespace {

constexpr double epsilon = 0.001;
constexpr double coordinateLimit = 10000;

QFont lettering(int size)
{
    QFont font(QStringLiteral("Sans Serif"));
    font.setPixelSize(size);
    return font;
}

double sweepOf(const DraftItem& item)
{
    const auto a = item.b - item.a;
    const auto b = item.points.front() - item.a;
    return std::remainder(std::atan2(b.y(), b.x()) - std::atan2(a.y(), a.x()), 2 * std::numbers::pi);
}

QPainterPath itemPath(const DraftItem& item)
{
    QPainterPath path;
    const auto segment = [&](QPointF a, QPointF b) {
        path.moveTo(a);
        path.lineTo(b);
    };
    switch (item.kind) {
    case DraftKind::Leader:
    case DraftKind::Datum:
    case DraftKind::Tolerance:
    case DraftKind::Roughness:
    case DraftKind::Thread:
    case DraftKind::Weld:
    case DraftKind::Fragment:
    case DraftKind::Table:
        break;
    case DraftKind::Line:
    case DraftKind::Rectangle:
    case DraftKind::Circle:
    case DraftKind::Arc:
    case DraftKind::Polyline:
    case DraftKind::Ellipse:
    case DraftKind::Spline:
    case DraftKind::ConstructionLine:
        return draftCurvePath(item);
    case DraftKind::Text:
        path.addText(item.a, lettering(4), item.text);
        break;
    case DraftKind::Horizontal:
    case DraftKind::Vertical: {
        const bool horizontal = item.kind == DraftKind::Horizontal;
        const auto first = horizontal ? QPointF(item.a.x(), item.label.y()) : QPointF(item.label.x(), item.a.y());
        const auto last = horizontal ? QPointF(item.b.x(), item.label.y()) : QPointF(item.label.x(), item.b.y());
        const auto tick = horizontal ? QPointF(0.65, -0.65) : QPointF(0.65, 0.65);
        segment(item.a, first);
        segment(item.b, last);
        segment(first, last);
        segment(first - tick, first + tick);
        segment(last - tick, last + tick);
        break;
    }
    case DraftKind::Aligned: {
        const auto axis = item.b - item.a;
        const auto length = QLineF(item.a, item.b).length();
        if (length < epsilon) {
            break;
        }
        const QPointF normal(-axis.y() / length, axis.x() / length);
        const auto shift = normal * QPointF::dotProduct(item.label - item.a, normal);
        const auto first = item.a + shift;
        const auto last = item.b + shift;
        const auto tick = axis * (0.65 / length) - normal * 0.65;
        segment(item.a, first);
        segment(item.b, last);
        segment(first, last);
        segment(first - tick, first + tick);
        segment(last - tick, last + tick);
        break;
    }
    case DraftKind::Angular: {
        const auto leg = item.b - item.a;
        const double radius = std::max(2.0, QLineF(item.a, item.label).length());
        const double angle = std::atan2(leg.y(), leg.x());
        const double sweep = sweepOf(item);
        const QRectF box(item.a - QPointF(radius, radius), QSizeF(2 * radius, 2 * radius));
        const auto point = [&](double theta) { return item.a + QPointF(std::cos(theta), std::sin(theta)) * radius; };
        segment(item.a, point(angle));
        segment(item.a, point(angle + sweep));
        constexpr double degrees = 180 / std::numbers::pi;
        path.arcMoveTo(box, -angle * degrees);
        path.arcTo(box, -angle * degrees, -sweep * degrees);
        break;
    }
    case DraftKind::Radius:
    case DraftKind::Diameter: {
        const auto radius = item.b - item.a;
        const auto length = QLineF(item.a, item.b).length();
        if (length >= epsilon) {
            const auto unit = radius / length;
            const QPointF side(-unit.y(), unit.x());
            segment(item.kind == DraftKind::Diameter ? item.a - radius : item.a, item.b);
            segment(item.b, item.label);
            segment(item.b, item.b - unit * 2 + side * .65);
            segment(item.b, item.b - unit * 2 - side * .65);
        }
        break;
    }
    case DraftKind::Hatch:
        path.setFillRule(Qt::OddEvenFill);
        for (const auto& loop : item.loops) {
            path.moveTo(loop.front());
            for (std::size_t i = 1; i < loop.size(); ++i) {
                path.lineTo(loop[i]);
            }
            path.closeSubpath();
        }
        break;
    }
    return path;
}

QPointF dimensionText(const DraftItem& item)
{
    switch (item.kind) {
    case DraftKind::Horizontal: return {(item.a.x() + item.b.x()) / 2, item.label.y() - 1};
    case DraftKind::Vertical: return {item.label.x() + 1, (item.a.y() + item.b.y()) / 2};
    case DraftKind::Aligned: {
        const auto axis = item.b - item.a;
        const auto length = QLineF(item.a, item.b).length();
        if (length < epsilon) {
            return item.label;
        }
        const QPointF normal(-axis.y() / length, axis.x() / length);
        return (item.a + item.b) / 2 + normal * QPointF::dotProduct(item.label - item.a, normal)
            - QPointF(0, 1);
    }
    case DraftKind::Angular: {
        const auto axis = item.b - item.a;
        const double angle = std::atan2(axis.y(), axis.x()) + sweepOf(item) / 2;
        const double radius = std::max(2.0, QLineF(item.a, item.label).length()) + 3;
        return item.a + QPointF(std::cos(angle), std::sin(angle)) * radius;
    }
    default: return item.label - QPointF(0, 1);
    }
}

struct AnnotationShape {
    QPainterPath lines;
    QPainterPath ink;
    QRectF box;
};

AnnotationShape annotationShape(const DraftItem& item)
{
    AnnotationShape shape;
    auto& lines = shape.lines;
    auto& ink = shape.ink;
    const auto segment = [&](QPointF a, QPointF b) {
        lines.moveTo(a);
        lines.lineTo(b);
    };
    const auto font = lettering(3.5);
    const QFontMetricsF metrics(font);
    const auto at = item.label;
    const auto delta = at - item.a;
    const double length = std::hypot(delta.x(), delta.y());
    if (length > epsilon) {
        segment(item.a, at);
        const auto axis = delta / length;
        const QPointF side(-axis.y(), axis.x());
        const bool datum = item.kind == DraftKind::Datum;
        const auto tip = datum ? item.a + axis * 2.5 : item.a;
        const auto base = datum ? item.a : item.a + axis * 3;
        const auto halfWidth = datum ? 1.3 : .7;
        if (item.kind == DraftKind::Weld) {
            segment(tip, base + side * halfWidth);
        } else {
            ink.moveTo(tip);
            ink.lineTo(base + side * halfWidth);
            ink.lineTo(base - side * halfWidth);
            ink.closeSubpath();
        }
    }
    if (item.kind == DraftKind::Leader || item.kind == DraftKind::Thread || item.kind == DraftKind::Weld) {
        const auto width = std::max(7.0, metrics.horizontalAdvance(item.text) + 2);
        segment(at, at + QPointF(width, 0));
        const bool reverse = item.kind == DraftKind::Weld && item.weldReverse;
        ink.addText(at + QPointF(1, reverse ? 4 : -1), font, item.text);
        shape.box = {at.x(), at.y() + (reverse ? 0 : -5), width, 5};
    } else if (item.kind == DraftKind::Roughness) {
        const auto shelf = at + QPointF(7, -6);
        segment(at, at + QPointF(2, 3));
        segment(at + QPointF(2, 3), shelf);
        const auto width = std::max(7.0, metrics.horizontalAdvance(item.text) + 2);
        segment(shelf, shelf + QPointF(width, 0));
        ink.addText(shelf + QPointF(1, -1), font, item.text);
        shape.box = {at.x(), shelf.y() - 5, width + 7, 14};
    } else {
        auto fields = item.kind == DraftKind::Datum ? QStringList{item.text} : item.text.split(u'|');
        double x = at.x();
        for (const auto& field : fields) {
            const double width = std::max(6.0, metrics.horizontalAdvance(field.trimmed()) + 3);
            lines.addRect({x, at.y() - 3, width, 6});
            ink.addText({x + (width - metrics.horizontalAdvance(field.trimmed())) / 2, at.y() + 1.2},
                        font, field.trimmed());
            x += width;
        }
        shape.box = {at.x(), at.y() - 3, x - at.x(), 6};
    }
    return shape;
}

QString numberedRequirements(const Draft& draft)
{
    QStringList numbered;
    for (const auto& line : draft.paper.requirements.split(u'\n', Qt::SkipEmptyParts)) {
        if (!line.trimmed().isEmpty()) {
            numbered << QStringLiteral("%1. %2").arg(numbered.size() + 1).arg(line.trimmed());
        }
    }
    return numbered.join(u'\n');
}

QRectF requirementsArea(const Draft& draft, const QRectF& frame, int sheet)
{
    const auto& custom = draft.paper.titleBlock;
    const double width = custom ? custom->width : draft.paper.eskd ? 185 : 145;
    const double height = custom ? custom->rows.size() * custom->rowHeight + 1
                                 : draft.paper.eskd ? (sheet == 1 ? 56 : 16) : 25;
    const double bottom = frame.bottom() - height;
    return QRectF(frame.right() - width, frame.top() + 2, width - 2, bottom - frame.top() - 2);
}

void paintRequirements(QPainter& p, const Draft& draft, const QRectF& frame, int sheet)
{
    const auto requirements = numberedRequirements(draft);
    if (requirements.isEmpty()) {
        return;
    }
    const auto area = requirementsArea(draft, frame, sheet);
    p.save();
    p.setClipRect(area, Qt::IntersectClip);
    p.setPen(Qt::black);
    p.setFont(lettering(3.5));
    p.drawText(area, Qt::AlignBottom | Qt::AlignLeft | Qt::TextWrapAnywhere, requirements);
    p.restore();
}

void paintEskdBlock(QPainter& p, const Draft& draft, const QRectF& frame, int sheet, int total)
{
    const bool first = sheet == 1;
    const double height = first ? 55 : 15;
    p.save();
    p.translate(frame.right() - 185, frame.bottom() - height);
    p.fillRect(QRectF(0, 0, 185, height), Qt::white);
    p.setPen(QPen(Qt::black, .45));
    p.drawRect(QRectF(0, 0, 185, height));
    const auto horizontal = [&](double y, double left, double right) {
        p.drawLine(QPointF(left, y), QPointF(right, y));
    };
    const auto vertical = [&](double x, double top, double bottom) {
        p.drawLine(QPointF(x, top), QPointF(x, bottom));
    };
    const auto cell = [&](QRectF area, const QString& value, int size = 3, int align = Qt::AlignCenter) {
        p.setFont(lettering(size));
        p.drawText(area.adjusted(.6, 0, -.6, 0), align | Qt::AlignVCenter | Qt::TextSingleLine,
            QFontMetricsF(p.font()).elidedText(value.simplified(), Qt::ElideRight, area.width() - 1.2));
    };
    const auto format = draft.a3 ? QStringLiteral("A3") : QStringLiteral("A4");
    if (first) {
        vertical(75, 0, 55);
        horizontal(15, 75, 185);
        vertical(145, 15, 55);
        horizontal(40, 75, 145);
        horizontal(20, 145, 185);
        horizontal(30, 145, 185);
        horizontal(35, 145, 185);
        vertical(160, 15, 30);
        vertical(170, 15, 30);
        vertical(160, 30, 35);
    } else {
        vertical(65, 0, 15);
        vertical(165, 0, 15);
        vertical(175, 0, 15);
        horizontal(5, 165, 185);
    }
    p.setPen(QPen(Qt::black, .2));
    const std::array firstColumns{0, 10, 17, 27, 50, 65, 75};
    const std::array nextColumns{0, 7, 17, 40, 55, 65};
    const QStringList headers{QStringLiteral("Зона"), QStringLiteral("Изм."), QStringLiteral("Лист"),
                              QStringLiteral("№ докум."), QStringLiteral("Подп."), QStringLiteral("Дата")};
    const auto table = [&](const auto& columns, int changeBottom, int bottom) {
        for (int y = 5; y <= changeBottom; y += 5) {
            horizontal(y, 0, columns.back());
        }
        for (std::size_t i = 1; i + 1 < columns.size(); ++i) {
            vertical(columns[i], 0, changeBottom);
        }
        const int start = first ? 0 : 1;
        for (std::size_t i = 0; i + 1 < columns.size(); ++i) {
            cell(QRectF(columns[i], changeBottom - 5, columns[i + 1] - columns[i], 5),
                 headers[i + start], 2);
        }
        if (first) {
            for (int y = changeBottom + 5; y < bottom; y += 5) {
                horizontal(y, 0, columns.back());
            }
            for (const auto x : {17, 40, 65}) {
                vertical(x, changeBottom, bottom);
            }
        }
    };
    if (first) {
        table(firstColumns, 30, 55);
        const QStringList roles{QStringLiteral("Разраб."), QStringLiteral("Пров."), QStringLiteral("Т. контр."),
                                QStringLiteral("Н. контр."), QStringLiteral("Утв.")};
        for (int i = 0; i < roles.size(); ++i) {
            cell(QRectF(0, 30 + i * 5, 17, 5), roles[i], 2);
        }
        cell({17, 30, 23, 5}, draft.author);
        cell({75, 0, 110, 15}, draftTitleField(draft, TitleField::Designation, sheet, total), 5);
        cell({75, 15, 70, 25}, draftTitleField(draft, TitleField::Title, sheet, total), 5);
        cell({75, 40, 70, 15}, draft.paper.material, 4);
        cell({145, 15, 15, 5}, QStringLiteral("Лит."), 2);
        cell({160, 15, 10, 5}, QStringLiteral("Масса"), 2);
        cell({160, 20, 10, 10}, draftTitleField(draft, TitleField::Mass, sheet, total), 2);
        cell({170, 15, 15, 5}, QStringLiteral("Масштаб"), 2);
        const double scale = draft.modelViews ? draft.modelViews->layout[0].scale : 1;
        const auto ratio = scale >= 1 ? QStringLiteral("%1:1").arg(scale, 0, 'g', 6)
                                      : QStringLiteral("1:%1").arg(1 / scale, 0, 'g', 6);
        cell({170, 20, 15, 10}, ratio, 3);
        cell({145, 30, 15, 5}, QStringLiteral("Лист %1").arg(sheet), 2);
        cell({160, 30, 25, 5}, QStringLiteral("Листов %1").arg(total), 2);
        cell({145, 35, 40, 20}, draft.paper.organization);
    } else {
        table(nextColumns, 15, 15);
        cell({65, 0, 100, 15}, draftTitleField(draft, TitleField::Designation, sheet, total), 5);
        cell({165, 0, 10, 5}, QStringLiteral("Вер."), 2);
        cell({175, 0, 10, 5}, QStringLiteral("Лист"), 2);
        cell({175, 5, 10, 10}, QString::number(sheet));
    }
    horizontal(height + 4, 0, 185);
    cell({155, height, 30, 4}, QStringLiteral("Формат %1").arg(format), 2);
    p.restore();
}

void titleBlock(QPainter& p, const Draft& draft, const QRectF& frame, int sheet, int total)
{
    if (const auto& custom = draft.paper.titleBlock) {
        DraftTable table{2, custom->width, custom->rowHeight, {}};
        for (const auto& row : custom->rows) {
            table.cells << row.label
                << (row.field == TitleField::Text ? row.text : draftTitleField(draft, row.field, sheet, total));
        }
        const auto height = custom->rows.size() * custom->rowHeight;
        const QRectF bounds(frame.right() - custom->width, frame.bottom() - height, custom->width, height);
        p.fillRect(bounds, Qt::white);
        paintDraftTable(p, bounds.topLeft(), table, Qt::black, .34);
        return;
    }
    if (draft.paper.eskd) {
        paintEskdBlock(p, draft, frame, sheet, total);
        return;
    }
    const QRectF block(frame.right() - 145, frame.bottom() - 24, 145, 24);
    const double x = block.left();
    const double y = block.top();
    p.fillRect(block, Qt::white);
    p.setPen(QPen(Qt::black, 0.25));
    p.drawRect(block);
    p.drawLine(QPointF(x, y + 9), QPointF(block.right(), y + 9));
    p.drawLine(QPointF(x, y + 17), QPointF(block.right(), y + 17));
    p.drawLine(QPointF(x + 106, y + 9), QPointF(x + 106, block.bottom()));
    p.setFont(lettering(4));
    const auto cell = [&](QRectF area, const QString& value) {
        const auto text = QFontMetricsF(p.font()).elidedText(value, Qt::ElideRight, area.width());
        p.drawText(area, Qt::AlignLeft | Qt::AlignVCenter, text);
    };
    cell({x + 2, y, 141, 9}, draftTitleField(draft, TitleField::Title, sheet, total));
    cell({x + 2, y + 9, 102, 8}, draftTitleField(draft, TitleField::Designation, sheet, total));
    const auto mass = draftTitleField(draft, TitleField::Mass, sheet, total);
    cell({x + 108, y + 9, 35, 8}, mass.isEmpty()
        ? QCoreApplication::translate("DraftSheet", draft.modelViews ? "mm" : "1:1   mm")
        : mass + QCoreApplication::translate("DraftSheet", " kg"));
    cell({x + 2, y + 17, 102, 7}, draft.author);
    cell({x + 108, y + 17, 35, 7}, QStringLiteral("%1   %2/%3")
        .arg(draft.a3 ? QStringLiteral("A3") : QStringLiteral("A4")).arg(sheet).arg(total));
}

}  // namespace

QString draftTitleField(const Draft& draft, TitleField field, int page, int pages)
{
    const auto* data = draft.paper.modelTitle && draft.modelViews ? &draft.modelViews->titleData : nullptr;
    switch (field) {
    case TitleField::Text: return {};
    case TitleField::Title: return data && !data->name.isEmpty() ? data->name : draft.title;
    case TitleField::Designation: return data && !data->designation.isEmpty() ? data->designation : draft.number;
    case TitleField::Author: return draft.author;
    case TitleField::Material: return draft.paper.material;
    case TitleField::Organization: return draft.paper.organization;
    case TitleField::Mass: return data && data->massKg ? QString::number(*data->massKg, 'g', 6) : QString{};
    case TitleField::Page: return QString::number(page);
    case TitleField::Pages: return QString::number(pages);
    }
    return {};
}

bool validTitleBlock(const TitleBlockLayout& layout)
{
    return std::isfinite(layout.width) && layout.width >= 40 && layout.width <= 185
        && std::isfinite(layout.rowHeight) && layout.rowHeight >= 4 && layout.rowHeight <= 12
        && !layout.rows.empty() && layout.rows.size() <= 12
        && std::ranges::all_of(layout.rows, [](const auto& row) {
            return row.field >= TitleField::Text && row.field <= TitleField::Pages
                && row.label.size() <= 80 && row.text.size() <= 200;
        });
}

bool isLinearDimension(DraftKind kind)
{
    return kind == DraftKind::Horizontal || kind == DraftKind::Vertical || kind == DraftKind::Aligned;
}

bool isDimension(DraftKind kind)
{
    return isLinearDimension(kind) || kind == DraftKind::Angular || kind == DraftKind::Radius
        || kind == DraftKind::Diameter;
}

static QString dimensionValue(const DraftItem& item)
{
    if (item.lostReference) {
        return QCoreApplication::translate("DraftSheet", "Lost link");
    }
    const auto number = [](double value) { return QString::number(std::abs(value), 'g', 8); };
    const double distance = QLineF(item.a, item.b).length();
    switch (item.kind) {
    case DraftKind::Horizontal: return number((item.a.x() - item.b.x()) / item.measurementScale);
    case DraftKind::Vertical: return number((item.a.y() - item.b.y()) / item.measurementScale);
    case DraftKind::Aligned: return number(distance / item.measurementScale);
    case DraftKind::Angular: return number(sweepOf(item) * 180 / std::numbers::pi) + QStringLiteral("°");
    case DraftKind::Radius: return QStringLiteral("R") + number(distance / item.measurementScale);
    case DraftKind::Diameter: return QStringLiteral("⌀") + number(2 * distance / item.measurementScale);
    default: return {};
    }
}

QString draftDimensionText(const DraftItem& item)
{
    auto value = dimensionValue(item);
    if (value.isEmpty() || item.lostReference) {
        return value;
    }
    const auto& notation = item.dimensionNote;
    if (!notation.fit.isEmpty()) {
        value += QLatin1Char(' ') + notation.fit;
    }
    if (!notation.upper.isEmpty() || !notation.lower.isEmpty()) {
        const auto upper = notation.upper.isEmpty() ? QStringLiteral("0") : notation.upper;
        const auto lower = notation.lower.isEmpty() ? QStringLiteral("0") : notation.lower;
        value += upper == lower ? QLatin1Char(' ') + upper
            : QLatin1Char(' ') + upper + QStringLiteral(" / ") + lower;
    }
    return value;
}

std::vector<QLineF> dimensionSegments(const DraftItem& item)
{
    std::vector<QLineF> result;
    for(const auto& polygon:itemPath(item).toSubpathPolygons()){
        for(qsizetype i=1;i<polygon.size();++i)
            if(polygon[i-1]!=polygon[i]) result.emplace_back(polygon[i-1],polygon[i]);
    }
    return result;
}

QPointF dimensionTextPoint(const DraftItem& item) { return dimensionText(item); }

std::span<const DraftAnchor> draftAnchors(DraftKind kind)
{
    using enum DraftAnchor;
    static constexpr std::array line{Start, End};
    static constexpr std::array rectangle{Start, End, CornerAB, CornerBA};
    static constexpr std::array circle{Center, Right, Left, Top, Bottom};
    static constexpr std::array arc{Start, End, Center};
    switch (kind) {
    case DraftKind::Line:
    case DraftKind::ConstructionLine:
    case DraftKind::Spline: return line;
    case DraftKind::Rectangle: return rectangle;
    case DraftKind::Circle:
    case DraftKind::Ellipse: return circle;
    case DraftKind::Polyline: return line;
    case DraftKind::Arc: return arc;
    default: return {};
    }
}

std::optional<QPointF> draftAnchorPoint(const DraftItem& item, DraftAnchor anchor)
{
    const auto anchors = draftAnchors(item.kind);
    if (std::ranges::find(anchors, anchor) == anchors.end()) {
        return {};
    }
    const auto ellipse = item.kind == DraftKind::Ellipse;
    const auto center = ellipse ? (item.a + item.b) / 2 : item.a;
    const auto radii = ellipse ? QPointF(std::abs(item.a.x() - item.b.x()), std::abs(item.a.y() - item.b.y())) / 2
                               : QPointF(QLineF(item.a, item.b).length(), QLineF(item.a, item.b).length());
    const bool pointChain = item.kind == DraftKind::Polyline || item.kind == DraftKind::Spline;
    switch (anchor) {
    case DraftAnchor::Start: return pointChain ? item.points.front() : item.a;
    case DraftAnchor::Center: return item.kind == DraftKind::Arc ? draftArc(item.a, item.b, item.label).a : center;
    case DraftAnchor::End:
        return pointChain ? item.points.back() : item.kind == DraftKind::Arc ? item.label : item.b;
    case DraftAnchor::CornerAB: return QPointF(item.a.x(), item.b.y());
    case DraftAnchor::CornerBA: return QPointF(item.b.x(), item.a.y());
    case DraftAnchor::Right: return center + QPointF(radii.x(), 0);
    case DraftAnchor::Left: return center - QPointF(radii.x(), 0);
    case DraftAnchor::Top: return center - QPointF(0, radii.y());
    case DraftAnchor::Bottom: return center + QPointF(0, radii.y());
    case DraftAnchor::ModelCurve:
    case DraftAnchor::ModelCenter:
    case DraftAnchor::Curve:
    case DraftAnchor::Intersection: return {};
    }
    return {};
}

DraftGeometry::DraftGeometry(const Draft& draft, const DraftItem* preview)
    : model_(draft.modelViews ? &*draft.modelViews : nullptr)
{
    for (const auto& item : draft.items) {
        if (!item.id.isNull() && !draftAnchors(item.kind).empty()) {
            sources_.insert(item.id, preview && item.id == preview->id ? preview : &item);
        }
    }
}

std::optional<QPointF> DraftGeometry::point(const DraftReference& reference) const
{
    if (isModelReference(reference)) {
        return model_ ? modelReferencePoint(*model_, reference) : std::nullopt;
    }
    const auto* source = sources_.value(reference.item);
    if (!source) {
        return {};
    }
    if (reference.anchor != DraftAnchor::Curve && reference.anchor != DraftAnchor::Intersection) {
        return draftAnchorPoint(*source, reference.anchor);
    }
    const auto curves = draftCurves(*source);
    if (curves.empty() || !std::isfinite(reference.parameter) || reference.parameter < 0
        || reference.parameter > curves.size()) {
        return {};
    }
    if (reference.anchor == DraftAnchor::Curve) {
        const auto index = std::min(static_cast<std::size_t>(reference.parameter), curves.size() - 1);
        return curves[index].at(reference.parameter - index);
    }
    const auto* crossing = sources_.value(reference.crossing);
    if (!crossing || crossing == source) {
        return {};
    }
    const auto other = draftCurves(*crossing);
    std::optional<QPointF> result;
    double distance = 2 * curves.size();
    for (std::size_t i = 0; i < curves.size(); ++i) {
        for (const auto& edge : other) {
            for (const auto& hit : draftIntersections(curves[i], edge)) {
                const double gap = std::abs(i + hit.parameter - reference.parameter);
                if (gap < distance) {
                    distance = gap;
                    result = hit.point;
                }
            }
        }
    }
    return result;
}

bool DraftGeometry::validReference(const DraftReference& reference) const
{
    if (isModelReference(reference)) {
        return !reference.item.isNull() && !reference.view.isNull() && std::isfinite(reference.parameter)
            && reference.parameter >= 0 && reference.parameter <= 1;
    }
    if (reference.anchor != DraftAnchor::Intersection) {
        return point(reference).has_value();
    }
    const auto* source = sources_.value(reference.item);
    return source && reference.item != reference.crossing && sources_.contains(reference.crossing)
        && std::isfinite(reference.parameter) && reference.parameter >= 0
        && reference.parameter <= draftCurves(*source).size();
}

DraftItem DraftGeometry::resolve(const DraftItem& item) const
{
    auto copy = item;
    if (item.kind == DraftKind::Hatch && !item.hatchBoundaries.empty()) {
        copy.loops.clear();
        QPainterPath outer;
        for (const auto& ids : item.hatchBoundaries) {
            std::vector<const DraftItem*> pieces;
            for (const auto& id : ids) {
                pieces.push_back(sources_.value(id));
            }
            const auto loop = draftBoundary(pieces);
            if (!loop) {
                copy.lostReference = true;
                break;
            }
            QPainterPath boundary;
            boundary.addPolygon(QPolygonF(loop->begin(), loop->end()));
            boundary.closeSubpath();
            if (copy.loops.empty()) {
                outer = boundary;
            } else if (!outer.contains(boundary)) {
                copy.lostReference = true;
                break;
            }
            copy.loops.push_back(*loop);
        }
        if (copy.lostReference) {
            copy.loops.clear();
        }
        return copy;
    }
    if (item.anchorA) {
        const auto resolved = point(*item.anchorA);
        copy.a = resolved.value_or(item.a);
        if (isModelReference(*item.anchorA)) {
            copy.lostReference = !resolved;
        } else {
            // A dimension on hand-drawn geometry keeps its offset from the measured point.
            copy.label += copy.a - item.a;
        }
    }
    if (item.anchorB) {
        const auto resolved = point(*item.anchorB);
        copy.b = resolved.value_or(item.b);
        copy.lostReference |= isModelReference(*item.anchorB) && !resolved;
    }
    if (item.anchorC && !item.points.empty()) {
        const auto resolved = point(*item.anchorC);
        copy.points.front() = resolved.value_or(item.points.front());
        copy.lostReference |= isModelReference(*item.anchorC) && !resolved;
    }
    const bool modelA = item.anchorA && isModelReference(*item.anchorA);
    const bool modelB = item.anchorB && isModelReference(*item.anchorB);
    if (modelA || modelB) {
        const auto scale = model_ && modelA && modelB && item.anchorA->view == item.anchorB->view
            ? modelReferenceScale(*model_, *item.anchorA) : std::nullopt;
        copy.lostReference |= !scale;
        copy.measurementScale = scale.value_or(1);
    }
    return copy;
}

QSizeF draftSize(const Draft& draft)
{
    auto size = draft.a3 ? QSizeF(420, 297) : QSizeF(297, 210);
    if (draft.paper.portrait) {
        size.transpose();
    }
    return size;
}

bool isAnnotation(DraftKind kind)
{
    return (kind >= DraftKind::Leader && kind <= DraftKind::Roughness)
        || kind == DraftKind::Thread || kind == DraftKind::Weld;
}

bool layerVisible(const Draft& draft, int layer)
{
    return layer >= 0 && layer < static_cast<int>(draft.layers.size()) && draft.layers[layer].visible;
}

bool layerEditable(const Draft& draft, int layer)
{
    return layerVisible(draft, layer) && !draft.layers[layer].locked;
}

bool validItem(const DraftItem& item)
{
    const auto inRange = [](QPointF point) {
        return std::isfinite(point.x()) && std::isfinite(point.y())
            && std::abs(point.x()) <= coordinateLimit && std::abs(point.y()) <= coordinateLimit;
    };
    if (!inRange(item.a) || !inRange(item.b) || !inRange(item.label)
        || item.points.size() > 20000 || !std::ranges::all_of(item.points, inRange)) {
        return false;
    }
    const auto& note = item.dimensionNote;
    if ((!isDimension(item.kind) && note != DimensionNote{})
        || note.fit.size() > 24 || note.upper.size() > 16 || note.lower.size() > 16
        || (item.kind != DraftKind::Hatch && !item.hatchBoundaries.empty())
        || (item.kind != DraftKind::Fragment && !item.fragment.empty())
        || (item.kind != DraftKind::Table && item.table)
        || (item.kind != DraftKind::Weld && item.weldReverse)) {
        return false;
    }
    const auto dx = std::abs(item.a.x() - item.b.x());
    const auto dy = std::abs(item.a.y() - item.b.y());
    switch (item.kind) {
    case DraftKind::Line:
    case DraftKind::ConstructionLine:
    case DraftKind::Circle: return std::hypot(dx, dy) >= epsilon;
    case DraftKind::Rectangle:
    case DraftKind::Ellipse: return dx >= epsilon && dy >= epsilon;
    case DraftKind::Tolerance: {
        const auto fields = item.text.split(u'|');
        return item.text.size() <= 200 && fields.size() >= 2 && fields.size() <= 6
            && std::ranges::none_of(fields, [](const auto& field) { return field.trimmed().isEmpty(); });
    }
    case DraftKind::Leader:
    case DraftKind::Datum:
    case DraftKind::Roughness:
    case DraftKind::Thread:
    case DraftKind::Weld:
    case DraftKind::Text: return !item.text.trimmed().isEmpty() && item.text.size() <= 200;
    case DraftKind::Horizontal:
    case DraftKind::Vertical:
    case DraftKind::Aligned:
    case DraftKind::Radius:
    case DraftKind::Diameter: return true;
    case DraftKind::Angular: return item.points.size() == 1;
    case DraftKind::Arc: return draftArc(item.a, item.b, item.label).radius >= epsilon;
    case DraftKind::Spline:
        if (item.points.size() > 512) {
            return false;
        }
        [[fallthrough]];
    case DraftKind::Polyline:
        return item.points.size() >= 2 && std::adjacent_find(item.points.begin(), item.points.end(),
            [](auto a, auto b) { return QLineF(a, b).length() < epsilon; }) == item.points.end();
    case DraftKind::Fragment:
        return !item.fragment.empty() && item.fragment.size() <= 1024
            && std::ranges::all_of(item.fragment, [](const DraftItem& child) {
                return child.kind != DraftKind::Fragment && child.kind != DraftKind::ConstructionLine
                    && !child.anchorA && !child.anchorB && !child.anchorC
                    && child.hatchBoundaries.empty() && validItem(child);
            });
    case DraftKind::Table: {
        if (!item.table) {
            return false;
        }
        const auto& table = *item.table;
        return table.columns >= 1 && table.columns <= 10 && !table.cells.isEmpty()
            && table.cells.size() % table.columns == 0 && table.cells.size() <= 30 * table.columns
            && std::isfinite(table.width) && table.width >= 10 && table.width <= 360
            && std::isfinite(table.rowHeight) && table.rowHeight >= 3 && table.rowHeight <= 30
            && std::ranges::all_of(table.cells, [](const auto& cell) { return cell.size() <= 200; });
    }
    case DraftKind::Hatch:
        return (item.hatchBoundaries.empty() || item.hatchBoundaries.size() == item.loops.size())
            && std::ranges::all_of(item.hatchBoundaries, [](const auto& loop) {
                return !loop.empty() && loop.size() <= 1024 && std::ranges::none_of(loop, [](const QUuid& id) { return id.isNull(); });
            })
            && !item.loops.empty() && std::ranges::all_of(item.loops, [&](const auto& loop) {
            return loop.size() >= 4 && loop.size() <= 50000 && QLineF(loop.front(), loop.back()).length() < epsilon
                && std::ranges::all_of(loop, inRange);
        });
    }
    return false;
}

QRectF draftItemBounds(const DraftItem& item)
{
    if (item.kind == DraftKind::Fragment) {
        QRectF bounds;
        for (const auto& child : item.fragment) {
            bounds |= draftItemBounds(child);
        }
        return bounds.translated(item.a);
    }
    if (item.kind == DraftKind::Table && item.table) {
        const auto& table = *item.table;
        return {item.a, QSizeF(table.width, table.rowHeight * (table.cells.size() / table.columns))};
    }
    if (isAnnotation(item.kind)) {
        const auto shape = annotationShape(item);
        return shape.box | shape.lines.boundingRect();
    }
    return itemPath(item).boundingRect();
}

DraftItem translateDraftItem(DraftItem item, QPointF delta)
{
    if (item.anchorA || item.anchorB || item.anchorC) {
        item.label += delta;
        return item;
    }
    if (item.kind == DraftKind::Hatch && !item.hatchBoundaries.empty()) {
        return item;
    }
    item.a += delta;
    if (item.kind != DraftKind::Text && item.kind != DraftKind::Fragment && item.kind != DraftKind::Table) {
        item.b += delta;
    }
    if (isDimension(item.kind) || item.kind == DraftKind::Arc || isAnnotation(item.kind)) {
        item.label += delta;
    }
    for (auto& point : item.points) {
        point += delta;
    }
    for (auto& loop : item.loops) {
        for (auto& point : loop) {
            point += delta;
        }
    }
    return item;
}

static void paintDraftTable(QPainter& p, QPointF origin, const DraftTable& table, QColor color,
                            double labelFraction)
{
    p.save();
    p.setPen(QPen(color, .25));
    p.setFont(lettering(3));
    const auto rows = table.cells.size() / table.columns;
    const auto columnWidth = [&](int column) {
        if (labelFraction > 0) {
            return table.width * (column == 0 ? labelFraction : 1 - labelFraction);
        }
        return table.width / table.columns;
    };
    for (qsizetype row = 0; row < rows; ++row) {
        double x = origin.x();
        for (int column = 0; column < table.columns; ++column) {
            const auto width = columnWidth(column);
            const QRectF cell(x, origin.y() + row * table.rowHeight, width, table.rowHeight);
            p.drawRect(cell);
            const auto value = QFontMetricsF(p.font()).elidedText(table.cells[row * table.columns + column].simplified(),
                                                                  Qt::ElideRight, width - 2);
            p.drawText(cell.adjusted(1, 0, -1, 0), Qt::AlignVCenter | Qt::AlignLeft, value);
            x += width;
        }
    }
    p.restore();
}

void paintDraftItem(QPainter& p, const DraftItem& item, bool selected)
{
    const QColor color = item.lostReference ? QColor("#a62020") : selected ? QColor("#1a72b8")
        : item.kind == DraftKind::ConstructionLine ? QColor("#788fa5") : QColor("#151b22");
    if (item.kind == DraftKind::Fragment) {
        p.save();
        p.translate(item.a);
        for (const auto& child : item.fragment) {
            paintDraftItem(p, child, selected);
        }
        p.restore();
        return;
    }
    if (item.kind == DraftKind::Table && item.table) {
        paintDraftTable(p, item.a, *item.table, color);
        return;
    }
    const bool dimension = isDimension(item.kind);
    QPen pen(color, selected ? 0.48 : dimension ? 0.18 : 0.35, Qt::SolidLine, Qt::FlatCap);
    if (item.stroke == DraftStroke::Hidden || item.kind == DraftKind::ConstructionLine) {
        pen.setDashPattern({7, 4});
    } else if (item.stroke == DraftStroke::Center) {
        pen.setDashPattern({15, 4, 3, 4});
    }
    if (isAnnotation(item.kind)) {
        const auto annotation = annotationShape(item);
        p.setPen(QPen(color, .22, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawPath(annotation.lines);
        p.fillPath(annotation.ink, color);
        return;
    }
    if (item.kind == DraftKind::Text) {
        p.fillPath(itemPath(item), color);
        return;
    }
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const auto path = itemPath(item);
    if (item.kind == DraftKind::Hatch) {
        if (item.lostReference) {
            p.setFont(lettering(3));
            p.drawText(item.a, QCoreApplication::translate("DraftSheet", "Lost hatch boundary"));
            return;
        }
        p.save();
        p.setClipPath(path, Qt::IntersectClip);
        const auto box = p.clipBoundingRect();
        p.setPen(QPen(color, .14));
        const double start = std::floor((box.left() + box.top()) / 3) * 3 - box.bottom();
        for (double x = start; x <= box.right(); x += 3) {
            p.drawLine(QPointF(x, box.bottom()), QPointF(x + box.height(), box.top()));
        }
        p.restore();
        if (selected) {
            p.drawPath(path);
        }
        return;
    }
    p.drawPath(path);
    if (dimension) {
        p.setFont(lettering(3));
        const auto anchor = dimensionText(item);
        const auto& note = item.dimensionNote;
        const bool deviations = !item.lostReference && (!note.upper.isEmpty() || !note.lower.isEmpty())
            && note.upper != note.lower;
        auto text = deviations ? dimensionValue(item) : draftDimensionText(item);
        if (deviations && !note.fit.isEmpty()) {
            text += QLatin1Char(' ') + note.fit;
        }
        const QFontMetricsF metrics(p.font());
        if (deviations) {
            auto upper = note.upper.isEmpty() ? QStringLiteral("0") : note.upper;
            auto lower = note.lower.isEmpty() ? QStringLiteral("0") : note.lower;
            const QFontMetricsF small(lettering(2));
            const auto baseWidth = metrics.horizontalAdvance(text);
            const auto offset = baseWidth + .6;
            const auto total = offset + std::max(small.horizontalAdvance(upper), small.horizontalAdvance(lower));
            const auto baseline = anchor - QPointF(item.kind == DraftKind::Vertical ? 0 : total / 2, 0);
            p.fillRect(QRectF(baseline.x() - .4, baseline.y() - 3, total + .8, 4), Qt::white);
            p.drawText(baseline, text);
            p.setFont(lettering(2));
            p.drawText(baseline + QPointF(offset, -1.25), upper);
            p.drawText(baseline + QPointF(offset, .85), lower);
            return;
        }
        const auto baseline = anchor - QPointF(item.kind == DraftKind::Vertical ? 0 : metrics.horizontalAdvance(text) / 2, 0);
        p.fillRect(metrics.tightBoundingRect(text).translated(baseline).adjusted(-.45, -.35, .45, .35), Qt::white);
        p.drawText(baseline, text);
    }
}

bool draftHit(const DraftItem& item, QPointF point, double tolerance)
{
    if (isAnnotation(item.kind)) {
        const auto annotation = annotationShape(item);
        QPainterPathStroker stroke;
        stroke.setWidth(tolerance * 2);
        return annotation.box.contains(point) || stroke.createStroke(annotation.lines).contains(point);
    }
    const auto path = itemPath(item);
    if (item.kind == DraftKind::Text) {
        return path.boundingRect().adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(point);
    }
    if (item.kind == DraftKind::Fragment || item.kind == DraftKind::Table) {
        return draftItemBounds(item).adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(point);
    }
    if (item.kind == DraftKind::Hatch) {
        return item.lostReference ? QLineF(item.a, point).length() < tolerance * 2 : path.contains(point);
    }
    QPainterPathStroker stroke;
    stroke.setWidth(tolerance * 2);
    return stroke.createStroke(path).contains(point);
}

static void paintSheetDraft(QPainter& p, const Draft& draft, bool grid, int selected,
                            const DraftItem* preview, int sheet, int total, bool construction)
{
    const auto size = draftSize(draft);
    const QRectF paper(QPointF{}, size);
    const QMarginsF margin(20, 5, 5, 5);
    const auto frame = paper.marginsRemoved(margin);
    p.save();
    p.setClipRect(paper, Qt::IntersectClip);
    p.fillRect(paper, Qt::white);
    p.setRenderHint(QPainter::Antialiasing);
    if (grid) {
        p.setPen(QPen(QColor("#e2eaf2"), 0.12));
        for (int x = 5; x < size.width(); x += 5) {
            p.drawLine(QPointF(x, 0), QPointF(x, size.height()));
        }
        for (int y = 5; y < size.height(); y += 5) {
            p.drawLine(QPointF(0, y), QPointF(size.width(), y));
        }
    }
    p.setPen(QPen(Qt::black, 0.45));
    p.drawRect(frame);
    p.save();
    p.setClipRect(frame, Qt::IntersectClip);
    if (draft.modelViews) {
        paintModelViews(p, *draft.modelViews, draft.paper.eskd);
        paintPositionCallouts(p, *draft.modelViews);
    }
    const DraftGeometry geometry(draft, preview);
    for (const bool hatches : {true, false}) {
        for (std::size_t i = 0; i < draft.items.size(); ++i) {
            const bool active = static_cast<int>(i) == selected;
            const auto& item = active && preview ? *preview : draft.items[i];
            if (layerVisible(draft, item.layer) && (item.kind == DraftKind::Hatch) == hatches
                && (construction || item.kind != DraftKind::ConstructionLine)) {
                if (isDimension(item.kind) || item.kind == DraftKind::Hatch) {
                    paintDraftItem(p, geometry.resolve(item), active);
                } else {
                    paintDraftItem(p, item, active);
                }
            }
        }
    }
    p.restore();
    titleBlock(p, draft, frame, sheet, total);
    paintRequirements(p, draft, frame, sheet);
    p.restore();
}

void paintDraft(QPainter& p, const Draft& draft, bool grid, int selected, const DraftItem* preview,
                int sheet, int total)
{
    paintSheetDraft(p, draft, grid, selected, preview, sheet, total, true);
}

void exportDraftPdf(const QString& path, const Draft& draft)
{
    const auto pages = static_cast<int>(draft.additionalSheets.size()) + 1;
    const QFontMetricsF metrics(lettering(3.5));
    for (int i = 0; i < pages; ++i) {
        const auto& sheet = i == 0 ? draft : draft.additionalSheets[i - 1];
        const auto frame = QRectF(QPointF{}, draftSize(sheet)).marginsRemoved(QMarginsF(20, 5, 5, 5));
        const auto area = requirementsArea(sheet, frame, i + 1);
        if (metrics.boundingRect(area, Qt::TextWrapAnywhere, numberedRequirements(sheet)).height() > area.height()) {
            throw std::runtime_error("Technical requirements do not fit on the drawing sheet.");
        }
    }
    QSaveFile file(path);
    const auto fileFailure = [&] { return std::runtime_error(file.errorString().toStdString()); };
    if (!file.open(QIODevice::WriteOnly)) {
        throw fileFailure();
    }
    {
        QPdfWriter writer(&file);
        const auto setPage = [&](bool a3, bool portrait) {
            if (!writer.setPageLayout(QPageLayout(QPageSize(a3 ? QPageSize::A3 : QPageSize::A4),
                portrait ? QPageLayout::Portrait : QPageLayout::Landscape, {}))) {
                throw std::runtime_error("Could not set the drawing PDF page layout.");
            }
        };
        setPage(draft.a3, draft.paper.portrait);
        writer.setResolution(300);
        writer.setCreator(QStringLiteral("Graph3D"));
        writer.setTitle(draft.title);
        QPainter painter;
        if (!painter.begin(&writer)) {
            throw std::runtime_error("Could not create the drawing PDF.");
        }
        for (int i = 0; i < pages; ++i) {
            const Draft& sheet = i == 0 ? draft : draft.additionalSheets[i - 1];
            if (i != 0) {
                setPage(sheet.a3, sheet.paper.portrait);
                if (!writer.newPage()) {
                    throw std::runtime_error("Could not add a drawing PDF page.");
                }
            }
            const auto size = draftSize(sheet);
            painter.save();
            painter.scale(writer.width() / size.width(), writer.height() / size.height());
            paintSheetDraft(painter, sheet, false, -1, nullptr, i + 1, pages, false);
            painter.restore();
        }
        for (int i = 0; i < pages; ++i) {
            const auto& sheet = i == 0 ? draft : draft.additionalSheets[i - 1];
            if (!sheet.modelViews || !sheet.modelViews->assembly || !sheet.modelViews->assembly->includeSpecification) {
                continue;
            }
            const auto& assembly = *sheet.modelViews->assembly;
            const auto specification = paginateSpecification(assembly);
            for (std::size_t page = 0; page < specification.size(); ++page) {
                setPage(false, true);
                if (!writer.newPage()) {
                    throw std::runtime_error("Could not add a drawing PDF page.");
                }
                painter.save();
                painter.scale(writer.width() / 210.0, writer.height() / 297.0);
                paintSpecificationPage(painter, assembly, specification[page], i + 1,
                    static_cast<int>(page) + 1, static_cast<int>(specification.size()));
                painter.restore();
            }
        }
        if (!painter.end()) {
            throw std::runtime_error("Could not finish the drawing PDF.");
        }
    }
    if (file.error() != QFileDevice::NoError || !file.commit()) {
        throw fileFailure();
    }
}

}  // namespace graph3d::drawing
