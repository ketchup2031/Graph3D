#include "drawing/Draft.h"

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
#include <cmath>
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

QPainterPath itemPath(const DraftItem& item)
{
    QPainterPath path;
    const auto segment = [&](QPointF a, QPointF b) {
        path.moveTo(a);
        path.lineTo(b);
    };
    const QRectF rectangle(item.a, item.b);
    switch (item.kind) {
    case DraftKind::Line:
        segment(item.a, item.b);
        break;
    case DraftKind::Rectangle:
        path.addRect(rectangle.normalized());
        break;
    case DraftKind::Circle: {
        const auto radius = QLineF(item.a, item.b).length();
        path.addEllipse(item.a, radius, radius);
        break;
    }
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
    }
    return path;
}

QString measurement(const DraftItem& item)
{
    const double distance = item.kind == DraftKind::Horizontal ? item.a.x() - item.b.x() : item.a.y() - item.b.y();
    return QString::number(std::abs(distance), 'g', 8);
}

QPointF dimensionText(const DraftItem& item)
{
    return item.kind == DraftKind::Horizontal ? QPointF((item.a.x() + item.b.x()) / 2, item.label.y() - 1)
                                             : QPointF(item.label.x() + 1, (item.a.y() + item.b.y()) / 2);
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
    cell({x + 108, y + 9, 35, 8}, QCoreApplication::translate("DraftSheet", "1:1   mm"));
    cell({x + 2, y + 17, 102, 7}, draft.author);
    cell({x + 108, y + 17, 35, 7}, draft.a3 ? QStringLiteral("A3   1/1") : QStringLiteral("A4   1/1"));
}

}  // namespace

QSizeF draftSize(const Draft& draft)
{
    return draft.a3 ? QSizeF(420, 297) : QSizeF(297, 210);
}

bool validItem(const DraftItem& item)
{
    for (const auto point : {item.a, item.b, item.label}) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())
            || std::abs(point.x()) > coordinateLimit || std::abs(point.y()) > coordinateLimit) {
            return false;
        }
    }
    const auto dx = std::abs(item.a.x() - item.b.x());
    const auto dy = std::abs(item.a.y() - item.b.y());
    switch (item.kind) {
    case DraftKind::Line:
    case DraftKind::Circle: return std::hypot(dx, dy) >= epsilon;
    case DraftKind::Rectangle: return dx >= epsilon && dy >= epsilon;
    case DraftKind::Text: return !item.text.trimmed().isEmpty() && item.text.size() <= 200;
    case DraftKind::Horizontal: return dx >= epsilon;
    case DraftKind::Vertical: return dy >= epsilon;
    }
    return false;
}

void paintDraftItem(QPainter& p, const DraftItem& item, bool selected)
{
    const QColor color = selected ? QColor("#1a72b8") : QColor("#151b22");
    const bool dimension = item.kind == DraftKind::Horizontal || item.kind == DraftKind::Vertical;
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
    p.drawPath(itemPath(item));
    if (dimension) {
        p.setFont(lettering(3));
        const auto anchor = dimensionText(item);
        const auto text = measurement(item);
        const auto width = QFontMetricsF(p.font()).horizontalAdvance(text);
        p.drawText(anchor - QPointF(item.kind == DraftKind::Horizontal ? width / 2 : 0, 0), text);
    }
}

bool draftHit(const DraftItem& item, QPointF point, double tolerance)
{
    const auto path = itemPath(item);
    if (item.kind == DraftKind::Text) {
        return path.boundingRect().adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(point);
    }
    QPainterPathStroker stroke;
    stroke.setWidth(tolerance * 2);
    return stroke.createStroke(path).contains(point);
}

void paintDraft(QPainter& p, const Draft& draft, bool grid, int selected)
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
    for (std::size_t i = 0; i < draft.items.size(); ++i) {
        paintDraftItem(p, draft.items[i], static_cast<int>(i) == selected);
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
