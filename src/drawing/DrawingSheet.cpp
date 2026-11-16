#include "drawing/DrawingSheet.h"

#include <QFontMetrics>
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

void paintSheet(QPainter& painter, const QRectF& area, const Projections& views, const SheetOptions& options)
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
    paintView(painter, views.front, front, scale.factor, options.hiddenLines);
    paintView(painter, views.right, right, scale.factor, options.hiddenLines);
    paintView(painter, views.top, top, scale.factor, options.hiddenLines);
    if (options.dimensions) {
        const double halfX = views.front.width * scale.factor / 2;
        const double halfY = views.right.width * scale.factor / 2;
        const double halfZ = views.front.height * scale.factor / 2;
        const double lower = front.y() + halfZ;
        dimension(painter, front.x() - halfX, front.x() + halfX, lower, lower + 8, views.front.width);
        dimension(painter, right.x() - halfY, right.x() + halfY, lower, lower + 8, views.right.width);
        // Rotate the dimension frame so its extension lines reach to the right of the front view.
        painter.save();
        painter.translate(front + QPointF(halfX, 0));
        painter.rotate(-90);
        dimension(painter, -halfZ, halfZ, 0, 8, views.front.height);
        painter.restore();
    }
    const double frontLabels = front.y() + views.front.height * scale.factor / 2
                             + (options.dimensions ? 11 : 4);
    const double topLabel = top.y() + views.top.height * scale.factor / 2 + 4;
    text(painter, {right.x() - 25, frontLabels, 50, 6}, QStringLiteral("Right"), 4, Qt::AlignCenter);
    text(painter, {front.x() - 25, frontLabels, 50, 6}, QStringLiteral("Front"), 4, Qt::AlignCenter);
    text(painter, {top.x() - 25, topLabel, 50, 6}, QStringLiteral("Top"), 4, Qt::AlignCenter);
    titleBlock(painter, options, scale);
    painter.restore();
}

void savePdf(const QString& path, const Projections& views, const SheetOptions& options)
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
        paintSheet(painter, QRectF(0, 0, writer.width(), writer.height()), views, options);
        if (!painter.end()) {
            throw std::runtime_error("Could not finish the PDF drawing.");
        }
    }
    if (file.error() != QFileDevice::NoError || !file.commit()) {
        throw std::runtime_error(file.errorString().toStdString());
    }
}

}  // namespace graph3d::drawing
