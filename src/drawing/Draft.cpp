#include "drawing/Draft.h"
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
    case DraftKind::Line:
    case DraftKind::Rectangle:
    case DraftKind::Circle:
    case DraftKind::Arc:
    case DraftKind::Polyline:
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

void titleBlock(QPainter& p, const Draft& draft, const QRectF& frame)
{
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
    cell({x + 2, y, 141, 9}, draft.title);
    cell({x + 2, y + 9, 102, 8}, draft.number);
    cell({x + 108, y + 9, 35, 8}, QCoreApplication::translate("DraftSheet", draft.modelViews ? "mm" : "1:1   mm"));
    cell({x + 2, y + 17, 102, 7}, draft.author);
    cell({x + 108, y + 17, 35, 7}, draft.a3 ? QStringLiteral("A3   1/1") : QStringLiteral("A4   1/1"));
}

}  // namespace

bool isLinearDimension(DraftKind kind)
{
    return kind == DraftKind::Horizontal || kind == DraftKind::Vertical || kind == DraftKind::Aligned;
}

bool isDimension(DraftKind kind)
{
    return isLinearDimension(kind) || kind == DraftKind::Angular || kind == DraftKind::Radius
        || kind == DraftKind::Diameter;
}

QString draftDimensionText(const DraftItem& item)
{
    const auto number = [](double value) { return QString::number(std::abs(value), 'g', 8); };
    const double distance = QLineF(item.a, item.b).length();
    switch (item.kind) {
    case DraftKind::Horizontal: return number(item.a.x() - item.b.x());
    case DraftKind::Vertical: return number(item.a.y() - item.b.y());
    case DraftKind::Aligned: return number(distance);
    case DraftKind::Angular: return number(sweepOf(item) * 180 / std::numbers::pi) + QStringLiteral("°");
    case DraftKind::Radius: return QStringLiteral("R") + number(distance);
    case DraftKind::Diameter: return QStringLiteral("⌀") + number(2 * distance);
    default: return {};
    }
}

std::span<const DraftAnchor> draftAnchors(DraftKind kind)
{
    using enum DraftAnchor;
    static constexpr std::array line{Start, End};
    static constexpr std::array rectangle{Start, End, CornerAB, CornerBA};
    static constexpr std::array circle{Center, Right, Left, Top, Bottom};
    static constexpr std::array arc{Start, End, Center};
    switch (kind) {
    case DraftKind::Line: return line;
    case DraftKind::Rectangle: return rectangle;
    case DraftKind::Circle: return circle;
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
    const auto radius = QLineF(item.a, item.b).length();
    switch (anchor) {
    case DraftAnchor::Start: return item.kind == DraftKind::Polyline ? item.points.front() : item.a;
    case DraftAnchor::Center: return item.kind == DraftKind::Arc ? draftArc(item.a, item.b, item.label).a : item.a;
    case DraftAnchor::End:
        return item.kind == DraftKind::Polyline ? item.points.back() : item.kind == DraftKind::Arc ? item.label : item.b;
    case DraftAnchor::CornerAB: return QPointF(item.a.x(), item.b.y());
    case DraftAnchor::CornerBA: return QPointF(item.b.x(), item.a.y());
    case DraftAnchor::Right: return item.a + QPointF(radius, 0);
    case DraftAnchor::Left: return item.a - QPointF(radius, 0);
    case DraftAnchor::Top: return item.a - QPointF(0, radius);
    case DraftAnchor::Bottom: return item.a + QPointF(0, radius);
    case DraftAnchor::Curve:
    case DraftAnchor::Intersection: return {};
    }
    return {};
}

DraftGeometry::DraftGeometry(const Draft& draft, const DraftItem* preview)
{
    for (const auto& item : draft.items) {
        if (!item.id.isNull() && !draftAnchors(item.kind).empty()) {
            sources_.insert(item.id, preview && item.id == preview->id ? preview : &item);
        }
    }
}

std::optional<QPointF> DraftGeometry::point(const DraftReference& reference) const
{
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
    if (item.anchorA) {
        copy.a = point(*item.anchorA).value_or(item.a);
        // The dimension line keeps its offset from the first measured point.
        copy.label += copy.a - item.a;
    }
    if (item.anchorB) {
        copy.b = point(*item.anchorB).value_or(item.b);
    }
    if (item.anchorC && !item.points.empty()) {
        copy.points.front() = point(*item.anchorC).value_or(item.points.front());
    }
    return copy;
}

QSizeF draftSize(const Draft& draft)
{
    return draft.a3 ? QSizeF(420, 297) : QSizeF(297, 210);
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
    const auto dx = std::abs(item.a.x() - item.b.x());
    const auto dy = std::abs(item.a.y() - item.b.y());
    switch (item.kind) {
    case DraftKind::Line:
    case DraftKind::Circle: return std::hypot(dx, dy) >= epsilon;
    case DraftKind::Rectangle: return dx >= epsilon && dy >= epsilon;
    case DraftKind::Text: return !item.text.trimmed().isEmpty() && item.text.size() <= 200;
    case DraftKind::Horizontal:
    case DraftKind::Vertical:
    case DraftKind::Aligned:
    case DraftKind::Radius:
    case DraftKind::Diameter: return true;
    case DraftKind::Angular: return item.points.size() == 1;
    case DraftKind::Arc: return draftArc(item.a, item.b, item.label).radius >= epsilon;
    case DraftKind::Polyline:
        return item.points.size() >= 2 && std::adjacent_find(item.points.begin(), item.points.end(),
            [](auto a, auto b) { return QLineF(a, b).length() < epsilon; }) == item.points.end();
    case DraftKind::Hatch:
        return !item.loops.empty() && std::ranges::all_of(item.loops, [&](const auto& loop) {
            return loop.size() >= 4 && loop.size() <= 50000 && QLineF(loop.front(), loop.back()).length() < epsilon
                && std::ranges::all_of(loop, inRange);
        });
    }
    return false;
}

void paintDraftItem(QPainter& p, const DraftItem& item, bool selected)
{
    const QColor color = selected ? QColor("#1a72b8") : QColor("#151b22");
    const bool dimension = isDimension(item.kind);
    QPen pen(color, selected ? 0.48 : dimension ? 0.18 : 0.35, Qt::SolidLine, Qt::FlatCap);
    if (item.stroke == DraftStroke::Hidden) {
        pen.setDashPattern({7, 4});
    } else if (item.stroke == DraftStroke::Center) {
        pen.setDashPattern({15, 4, 3, 4});
    }
    if (item.kind == DraftKind::Text) {
        p.fillPath(itemPath(item), color);
        return;
    }
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const auto path = itemPath(item);
    if (item.kind == DraftKind::Hatch) {
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
        const auto text = draftDimensionText(item);
        const QFontMetricsF metrics(p.font());
        const auto baseline = anchor - QPointF(item.kind == DraftKind::Vertical ? 0 : metrics.horizontalAdvance(text) / 2, 0);
        p.fillRect(metrics.tightBoundingRect(text).translated(baseline).adjusted(-.45, -.35, .45, .35), Qt::white);
        p.drawText(baseline, text);
    }
}

bool draftHit(const DraftItem& item, QPointF point, double tolerance)
{
    const auto path = itemPath(item);
    if (item.kind == DraftKind::Text) {
        return path.boundingRect().adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(point);
    }
    if (item.kind == DraftKind::Hatch) {
        return path.contains(point);
    }
    QPainterPathStroker stroke;
    stroke.setWidth(tolerance * 2);
    return stroke.createStroke(path).contains(point);
}

void paintDraft(QPainter& p, const Draft& draft, bool grid, int selected, const DraftItem* preview)
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
        paintModelViews(p, *draft.modelViews);
    }
    const DraftGeometry geometry(draft, preview);
    for (const bool hatches : {true, false}) {
        for (std::size_t i = 0; i < draft.items.size(); ++i) {
            const bool active = static_cast<int>(i) == selected;
            const auto& item = active && preview ? *preview : draft.items[i];
            if ((item.kind == DraftKind::Hatch) == hatches) {
                if (isDimension(item.kind)) {
                    paintDraftItem(p, geometry.resolve(item), active);
                } else {
                    paintDraftItem(p, item, active);
                }
            }
        }
    }
    p.restore();
    titleBlock(p, draft, frame);
    p.restore();
}

void exportDraftPdf(const QString& path, const Draft& draft)
{
    QSaveFile file(path);
    const auto fileFailure = [&] { return std::runtime_error(file.errorString().toStdString()); };
    if (!file.open(QIODevice::WriteOnly)) {
        throw fileFailure();
    }
    {
        QPdfWriter writer(&file);
        writer.setPageLayout(QPageLayout(QPageSize(draft.a3 ? QPageSize::A3 : QPageSize::A4),
                                         QPageLayout::Landscape, QMarginsF{}));
        writer.setResolution(300);
        writer.setCreator(QStringLiteral("Graph3D"));
        writer.setTitle(draft.title);
        QPainter painter;
        if (!painter.begin(&writer)) {
            throw std::runtime_error("Could not create the drawing PDF.");
        }
        const auto size = draftSize(draft);
        painter.scale(writer.width() / size.width(), writer.height() / size.height());
        paintDraft(painter, draft);
        if (!painter.end()) {
            throw std::runtime_error("Could not finish the drawing PDF.");
        }
    }
    if (file.error() != QFileDevice::NoError || !file.commit()) {
        throw fileFailure();
    }
}

}  // namespace graph3d::drawing
