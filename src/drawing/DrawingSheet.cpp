#include "drawing/DrawingSheet.h"

#include <QFontMetrics>
#include <QLineF>
#include <QPageLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace graph3d::drawing {
namespace {

struct Scale { double factor; QString text; };

Scale chooseScale(const Projections& views)
{
    const auto& front = views.front;
    const auto& top = views.top;
    const double ratio = std::min(102 / std::max(front.width, top.height),
                                  51 / std::max(front.height, top.height));
    constexpr std::array scales{10.0, 5.0, 2.0, 1.0, 0.5, 0.2, 0.1, 0.05, 0.02, 0.01,
                                0.005, 0.002, 0.001};
    for (const double factor : scales) {
        if (factor <= ratio) {
            return factor >= 1 ? Scale{factor, QStringLiteral("%1:1").arg(factor)}
                               : Scale{factor, QStringLiteral("1:%1").arg(std::round(1 / factor))};
        }
    }
    const double denominator = std::ceil(1 / ratio);
    return {1 / denominator, QStringLiteral("1:%1").arg(denominator, 0, 'g', 8)};
}

void pen(QPainter& p, double width, Qt::PenStyle style = Qt::SolidLine)
{
    QPen line(Qt::black, width, style, Qt::FlatCap, Qt::MiterJoin);
    if (style == Qt::DashLine) {
        line.setDashPattern({9, 5});
    }
    p.setPen(line);
}

void text(QPainter& p, QRectF rect, const QString& value, int size, int alignment, bool bold = false)
{
    auto font = p.font();
    font.setPixelSize(size);
    font.setBold(bold);
    p.setFont(font);
    const auto label = QFontMetrics(font).elidedText(value.simplified(), Qt::ElideRight,
                                                    static_cast<int>(rect.width()));
    p.drawText(rect, alignment | Qt::TextSingleLine, label);
}

void paintEdges(QPainter& p, const Linework& edges, QPointF center, double scale)
{
    QPainterPath path;
    for (const auto& line : edges) {
        const auto convert = [scale, center](Point point) {
            return center + QPointF(point.x * scale, -point.y * scale);
        };
        path.moveTo(convert(line.front()));
        for (std::size_t i = 1; i < line.size(); ++i) {
            path.lineTo(convert(line[i]));
        }
    }
    p.drawPath(path);
}

void paintView(QPainter& p, const View& view, QPointF center, double scale, bool hidden)
{
    if (hidden) {
        pen(p, 0.19, Qt::DashLine);
        paintEdges(p, view.hidden, center, scale);
    }
    pen(p, 0.40);
    paintEdges(p, view.visible, center, scale);
}

QString digits(double length)
{
    return QString::number(length, 'g', 7);
}

void arrow(QPainter& p, QPointF tip, double direction)
{
    p.setBrush(Qt::black);
    p.drawPolygon(QPolygonF{tip, tip + QPointF(direction * 2.3, -0.57),
                            tip + QPointF(direction * 2.3, 0.57)});
    p.setBrush(Qt::NoBrush);
}

// In this coordinate frame the measured span lies horizontally, with the dimension below it.
void dimension(QPainter& p, double from, double to, double modelEdge, double line, double value)
{
    pen(p, 0.16);
    for (const auto x : {from, to}) {
        p.drawLine(QPointF(x, modelEdge + 1), QPointF(x, line + 1.5));
    }
    const bool exterior = to - from < 11;
    p.drawLine(QPointF(from - (exterior ? 4 : 0), line), QPointF(to + (exterior ? 4 : 0), line));
    arrow(p, {from, line}, exterior ? -1 : 1);
    arrow(p, {to, line}, exterior ? 1 : -1);
    const auto label = QRectF((from + to) / 2 - 16, line - 5, 32, 4.5);
    text(p, label, digits(value), 3, Qt::AlignCenter);
}

void roundDimensions(QPainter& p, const View& view, QPointF origin, double scale)
{
    if (view.circles.empty()) {
        return;
    }
    struct Group {
        RoundContour representative;
        int quantity = 1;
    };
    const auto paperPoint = [origin, scale](Point point) {
        return origin + QPointF(point.x * scale, -point.y * scale);
    };
    const double tolerance = std::max(view.width, view.height) * 1.e-7;
    std::vector<Group> groups;
    std::vector<RoundContour> marked;
    for (const auto& contour : view.circles) {
        const auto same = [radius = contour.radius, tolerance](const Group& group) {
            return std::abs(group.representative.radius - radius) <= tolerance;
        };
        auto group = std::find_if(groups.begin(), groups.end(), same);
        if (group == groups.end()) {
            groups.push_back({contour});
        } else {
            ++group->quantity;
            if (contour.center.x < group->representative.center.x) {
                group->representative = contour;
            }
        }
        auto mark = std::find_if(marked.begin(), marked.end(), [c = contour.center, tolerance](const auto& item) {
            return std::hypot(item.center.x - c.x, item.center.y - c.y) <= tolerance;
        });
        if (mark == marked.end()) {
            marked.push_back(contour);
        } else {
            mark->radius = std::min(mark->radius, contour.radius);
        }
    }
    pen(p, 0.16);
    for (const auto& mark : marked) {
        const auto point = paperPoint(mark.center);
        const double halfMark = std::min(1.6, mark.radius * scale * 0.5);
        if (halfMark >= 0.3) {
            p.drawLine(point - QPointF(halfMark, 0), point + QPointF(halfMark, 0));
            p.drawLine(point - QPointF(0, halfMark), point + QPointF(0, halfMark));
        }
    }

    // Reserve the lower-left quadrant for callouts. Largest circles take the
    // upper rows; stagger the touch points so concentric leaders do not coincide.
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        return a.representative.radius > b.representative.radius;
    });
    constexpr std::size_t maxRows = 5;
    const auto rows = std::min(maxRows, groups.size());
    for (std::size_t i = 0; i < rows; ++i) {
        const auto& entry = groups[i];
        const auto center = paperPoint(entry.representative.center);
        const auto radius = entry.representative.radius * scale;
        const double relative = static_cast<double>(i) - (rows - 1) / 2.0;
        const double angle = rows == 1 ? 0 : relative * 0.82 / (rows - 1);
        const QPointF bend(136, origin.y() + 10 * relative);
        const auto onCircle = [center, radius](double direction) {
            return center + QPointF(-std::cos(direction), std::sin(direction)) * radius;
        };
        auto tip = onCircle(angle);
        for (const double turn : {0.0, -0.22, 0.22, -0.44, 0.44, -0.66, 0.66}) {
            const auto candidate = onCircle(std::clamp(angle + turn, -1.0, 1.0));
            const auto path = candidate - bend;
            const bool clear = std::none_of(view.circles.begin(), view.circles.end(),
                [&](const RoundContour& other) {
                    const auto at = paperPoint(other.center);
                    const auto limit = other.radius * scale;
                    if (QLineF(center, at).length() <= tolerance * scale
                        || QLineF(candidate, at).length() <= limit + tolerance * scale) {
                        return false;
                    }
                    const double t = std::clamp(QPointF::dotProduct(at - bend, path)
                        / QPointF::dotProduct(path, path), 0.0, 1.0);
                    return QLineF(at, bend + path * t).length() <= limit + 0.25;
                });
            if (clear) {
                tip = candidate;
                break;
            }
        }
        pen(p, 0.16);
        p.drawLine(QPointF(104, bend.y()), bend);
        p.drawLine(bend, tip);

        const auto ray = tip - bend;
        const auto direction = ray / std::hypot(ray.x(), ray.y());
        const auto normal = QPointF(-direction.y(), direction.x());
        const auto wing = tip - direction * 2.3;
        p.setBrush(Qt::black);
        p.drawPolygon(QPolygonF{tip, wing + normal * 0.55, wing - normal * 0.55});
        p.setBrush(Qt::NoBrush);

        const auto diameter = QStringLiteral("⌀%1").arg(digits(entry.representative.radius * 2));
        const auto label = entry.quantity > 1 ? QStringLiteral("%1× %2").arg(entry.quantity).arg(diameter) : diameter;
        text(p, {104, bend.y() - 6, 32, 5.5}, label, 4, Qt::AlignHCenter | Qt::AlignVCenter);
    }
    if (groups.size() > maxRows) {
        text(p, {104, origin.y() + 25, 46, 5}, QStringLiteral("More diameters omitted"), 3, Qt::AlignLeft);
    }
}

void paintHatch(QPainter& p, const Section& section, QPointF center, double scale)
{
    for (const auto& face : section.material) {
        QPainterPath mask;
        mask.setFillRule(Qt::OddEvenFill);
        for (const auto& loop : face) {
            QPolygonF polygon;
            polygon.reserve(static_cast<qsizetype>(loop.size()));
            for (const auto& point : loop) {
                polygon.append(center + QPointF(point.x * scale, -point.y * scale));
            }
            mask.addPolygon(polygon);
            mask.closeSubpath();
        }
        p.save();
        p.setClipPath(mask, Qt::IntersectClip);
        pen(p, 0.15);
        const auto box = mask.boundingRect();
        constexpr double pitch = 3;
        const double first = std::floor((box.left() + box.top()) / pitch) * pitch - box.bottom();
        for (double x = first; x <= box.right(); x += pitch) {
            p.drawLine(QPointF(x, box.bottom()), QPointF(x + box.height(), box.top()));
        }
        p.restore();
    }
}

void paintCutLine(QPainter& p, const Section& section, const Projections& views,
                  QPointF front, QPointF top, double scale)
{
    using geometry::Plane;
    const bool vertical = section.plane == Plane::YZ;
    const bool onTop = section.plane == Plane::XZ;
    const auto origin = onTop ? top : front;
    const auto& view = onTop ? views.top : views.front;
    const QPointF center = origin + (vertical ? QPointF(section.offset * scale, 0)
                                               : QPointF(0, -section.offset * scale));
    const double extent = (vertical ? view.height : view.width) * scale / 2 + 5;
    const QPointF along = vertical ? QPointF(0, 1) : QPointF(1, 0);
    const QPointF direction = vertical ? QPointF(-1, 0) : QPointF(0, onTop ? -1 : 1);
    pen(p, 0.22, Qt::DashDotLine);
    p.drawLine(center - along * extent, center + along * extent);
    for (const double side : {-extent, extent}) {
        const auto start = center + along * side;
        const auto tip = start + direction * 5;
        pen(p, 0.28);
        p.drawLine(start, tip);
        const auto base = tip - direction * 2.4;
        p.setBrush(Qt::black);
        p.drawPolygon(QPolygonF{tip, base + along * 0.75, base - along * 0.75});
        p.setBrush(Qt::NoBrush);
        const auto labelAt = tip + (vertical ? QPointF(-4, 0) : QPointF(0, onTop ? -3 : 3));
        text(p, {labelAt.x() - 2, labelAt.y() - 2.5, 4, 5}, QStringLiteral("A"), 3, Qt::AlignCenter, true);
    }
}

void titleBlock(QPainter& p, const SheetOptions& options, const Scale& scale)
{
    pen(p, 0.26);
    p.drawRect(QRectF(143, 174, 144, 26));
    p.drawLine(QPointF(143, 185), QPointF(287, 185));
    p.drawLine(QPointF(143, 192.5), QPointF(287, 192.5));
    for (const auto x : {210.0, 238.0, 266.0}) {
        p.drawLine(QPointF(x, 185), QPointF(x, 192.5));
    }
    text(p, {146, 175, 138, 9}, options.title, 5, Qt::AlignVCenter | Qt::AlignLeft, true);
    text(p, {146, 185, 61, 7.5}, options.designation, 4, Qt::AlignVCenter | Qt::AlignLeft);
    text(p, {210, 185, 28, 7.5}, scale.text, 4, Qt::AlignCenter);
    text(p, {238, 185, 28, 7.5}, QStringLiteral("mm"), 3, Qt::AlignCenter);
    text(p, {266, 185, 21, 7.5}, QStringLiteral("1 / 1"), 3, Qt::AlignCenter);
    text(p, {146, 192.5, 118, 7}, QStringLiteral("Graph3D  |  First-angle projection"), 3,
         Qt::AlignVCenter | Qt::AlignLeft);
    text(p, {266, 192.5, 21, 7}, QStringLiteral("A4"), 3, Qt::AlignCenter);
}

}  // namespace

void paintSheet(QPainter& painter, const QRectF& area, const Projections& views, const SheetOptions& options,
                const Section* section)
{
    painter.save();
    painter.translate(area.topLeft());
    painter.scale(area.width() / sheetWidth, area.height() / sheetHeight);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(QRectF(0, 0, sheetWidth, sheetHeight), Qt::white);
    pen(painter, 0.5);
    painter.drawRect(QRectF(10, 10, 277, 190));

    const auto scale = chooseScale(views);
    constexpr QPointF front{211, 51};
    constexpr QPointF right{77, 51};
    constexpr QPointF top{211, 126};
    using geometry::Plane;
    const bool cutFront = section && section->plane == Plane::XZ;
    const bool cutRight = section && section->plane == Plane::YZ;
    const bool cutTop = section && section->plane == Plane::XY;
    const auto paint = [&](const View& view, QPointF origin, bool cut) {
        if (cut) {
            paintHatch(painter, *section, origin, scale.factor);
            paintView(painter, section->view, origin, scale.factor, false);
        } else {
            paintView(painter, view, origin, scale.factor, options.hiddenLines);
        }
    };
    paint(views.front, front, cutFront);
    paint(views.right, right, cutRight);
    paint(views.top, top, cutTop);
    if (section) {
        paintCutLine(painter, *section, views, front, top, scale.factor);
    }
    if (options.dimensions) {
        const double halfX = views.front.width * scale.factor / 2;
        const double halfY = views.right.width * scale.factor / 2;
        const double halfZ = views.front.height * scale.factor / 2;
        const double lower = front.y() + halfZ;
        if (!cutRight) {
            dimension(painter, right.x() - halfY, right.x() + halfY, lower, lower + 8, views.right.width);
        }
        if (!cutFront) {
            dimension(painter, front.x() - halfX, front.x() + halfX, lower, lower + 8, views.front.width);
            // Rotate the dimension frame so its extension lines reach to the right of the front view.
            painter.save();
            painter.translate(front + QPointF(halfX, 0));
            painter.rotate(-90);
            dimension(painter, -halfZ, halfZ, 0, 8, views.front.height);
            painter.restore();
        }
        if (!cutTop && !cutFront) {
            roundDimensions(painter, views.top, top, scale.factor);
        }
    }
    const auto label = [&](const View& view, QPointF origin, const QString& name, bool cut, bool dimensioned) {
        const double offset = options.dimensions && dimensioned && !cut ? 11 : 4;
        const double y = origin.y() + view.height * scale.factor / 2 + offset;
        text(painter, {origin.x() - 25, y, 50, 6}, cut ? QStringLiteral("A–A") : name,
             4, Qt::AlignCenter, cut);
    };
    label(views.right, right, QStringLiteral("Right"), cutRight, true);
    label(views.front, front, QStringLiteral("Front"), cutFront, true);
    label(views.top, top, QStringLiteral("Top"), cutTop, false);
    titleBlock(painter, options, scale);
    painter.restore();
}

void savePdf(const QString& path, const Projections& views, const SheetOptions& options,
                const Section* section)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error(file.errorString().toStdString());
    }
    {
        QPdfWriter writer(&file);
        writer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Landscape,
                                         QMarginsF{}, QPageLayout::Millimeter));
        writer.setResolution(300);
        writer.setTitle(options.title);
        writer.setCreator(QStringLiteral("Graph3D"));
        QPainter painter;
        if (!painter.begin(&writer)) {
            throw std::runtime_error("Could not open the PDF painting device.");
        }
        paintSheet(painter, QRectF(0, 0, writer.width(), writer.height()), views, options, section);
        if (!painter.end()) {
            throw std::runtime_error("Could not finish the PDF drawing.");
        }
    }
    if (file.error() != QFileDevice::NoError || !file.commit()) {
        throw std::runtime_error(file.errorString().toStdString());
    }
}

}  // namespace graph3d::drawing
