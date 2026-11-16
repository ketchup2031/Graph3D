#include "gui/DraftCanvas.h"

#include <QInputDialog>
#include <QLineF>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace graph3d {
namespace {

using drawing::DraftItem;
using drawing::DraftKind;

DraftItem moved(DraftItem item, QPointF delta)
{
    item.a += delta;
    if (item.kind != DraftKind::Text) {
        item.b += delta;
    }
    if (item.kind == DraftKind::Horizontal || item.kind == DraftKind::Vertical) {
        item.label += delta;
    }
    return item;
}

}  // namespace

DraftCanvas::DraftCanvas(const drawing::Draft& draft, QWidget* parent) : QWidget(parent), draft_(draft)
{
    setObjectName(QStringLiteral("draftCanvas"));
    setAccessibleName(tr("2D drawing sheet"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 220);
}

void DraftCanvas::setTool(std::optional<DraftKind> tool)
{
    cancel();
    tool_ = tool;
    if (tool_) {
        select(-1);
    }
    setCursor(tool ? Qt::CrossCursor : Qt::ArrowCursor);
}

void DraftCanvas::setStroke(drawing::DraftStroke stroke)
{
    stroke_ = stroke;
    update();
}

void DraftCanvas::setGrid(bool enabled)
{
    grid_ = enabled;
    update();
}

void DraftCanvas::fit()
{
    zoom_ = 1;
    pan_ = {};
    update();
}

void DraftCanvas::cancel()
{
    points_.clear();
    dragging_ = false;
    preview_.reset();
    update();
}

void DraftCanvas::select(int index)
{
    if (selected_ != index) {
        selected_ = index;
        emit selectionChanged(index);
        update();
    }
}

double DraftCanvas::scale() const
{
    const auto paper = drawing::draftSize(draft_);
    return zoom_ * std::min((width() - 32) / paper.width(), (height() - 32) / paper.height());
}

QPointF DraftCanvas::origin() const
{
    const auto size = drawing::draftSize(draft_) * scale();
    return QPointF((width() - size.width()) / 2, (height() - size.height()) / 2) + pan_;
}

QPointF DraftCanvas::toSheet(QPointF screen) const
{
    return (screen - origin()) / scale();
}

QPointF DraftCanvas::snapped(QPointF page) const
{
    double best = 8 / scale();
    QPointF result;
    bool found = false;
    const auto consider = [&](QPointF point) {
        const auto distance = QLineF(page, point).length();
        if (distance < best) {
            best = distance;
            result = point;
            found = true;
        }
    };
    for (const auto& item : draft_.items) {
        if (item.kind == DraftKind::Line) {
            consider(item.a);
            consider(item.b);
        } else if (item.kind == DraftKind::Rectangle) {
            for (const auto corner : {item.a, item.b, QPointF(item.a.x(), item.b.y()), QPointF(item.b.x(), item.a.y())}) {
                consider(corner);
            }
        } else if (item.kind == DraftKind::Circle) {
            consider(item.a);
            const double radius = QLineF(item.a, item.b).length();
            for (const auto direction : {QPointF(1, 0), QPointF(-1, 0), QPointF(0, 1), QPointF(0, -1)}) {
                consider(item.a + direction * radius);
            }
        }
    }
    return found ? result : QPointF(std::round(page.x()), std::round(page.y()));
}

std::optional<DraftItem> DraftCanvas::pendingItem(QPointF end) const
{
    if (!tool_ || points_.empty()) {
        return {};
    }
    const bool dimension = *tool_ == DraftKind::Horizontal || *tool_ == DraftKind::Vertical;
    if (dimension && points_.size() == 1) {
        return DraftItem{DraftKind::Line, points_.front(), end};
    }
    return DraftItem{*tool_, points_.front(), dimension ? points_[1] : end, dimension ? end : QPointF{},
                     {}, dimension ? drawing::DraftStroke::Solid : stroke_};
}

void DraftCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().dark());
    painter.translate(origin());
    painter.scale(scale(), scale());
    drawing::paintDraft(painter, draft_, grid_, selected_);
    const auto size = drawing::draftSize(draft_);
    painter.setClipRect(QRectF(QPointF{}, size).marginsRemoved(QMarginsF(20, 5, 5, 5)));
    const auto ghost = dragging_ ? preview_ : pendingItem(cursor_);
    if (ghost && drawing::validItem(*ghost)) {
        drawing::paintDraftItem(painter, *ghost, true);
    }
    if (tool_ && underMouse()) {
        painter.setPen(QPen(QColor("#2469a5"), 0.18));
        painter.drawLine(cursor_ - QPointF(1, 0), cursor_ + QPointF(1, 0));
        painter.drawLine(cursor_ - QPointF(0, 1), cursor_ + QPointF(0, 1));
    }
}

void DraftCanvas::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        panStart_ = event->position() - pan_;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    setFocus();
    const auto raw = toSheet(event->position());
    const auto size = drawing::draftSize(draft_);
    const auto frame = QRectF(QPointF{}, size).marginsRemoved(QMarginsF(20, 5, 5, 5));
    const QRectF title(frame.right() - 145, frame.bottom() - 24, 145, 24);
    if (!tool_) {
        int index = -1;
        if (frame.contains(raw) && !title.contains(raw)) {
            for (int i = static_cast<int>(draft_.items.size()) - 1; i >= 0; --i) {
                if (drawing::draftHit(draft_.items[i], raw, 6 / scale())) {
                    index = i;
                    break;
                }
            }
        }
        select(index);
        dragging_ = index >= 0;
        dragStart_ = raw;
        return;
    }
    if (!frame.contains(raw) || title.contains(raw)) {
        return;
    }
    const auto point = snapped(raw);
    if (*tool_ == DraftKind::Text) {
        bool accepted = false;
        auto text = QInputDialog::getText(this, tr("Text"), tr("Drawing note"), QLineEdit::Normal, {}, &accepted);
        if (accepted && !text.trimmed().isEmpty()) {
            emit itemCreated(DraftItem{DraftKind::Text, point, {}, {}, text.left(200)});
        }
        return;
    }
    const bool dimension = *tool_ == DraftKind::Horizontal || *tool_ == DraftKind::Vertical;
    if (points_.empty()) {
        points_.push_back(point);
    } else if (dimension && points_.size() == 1) {
        if (*tool_ == DraftKind::Horizontal ? point.x() != points_[0].x() : point.y() != points_[0].y()) {
            points_.push_back(point);
        }
    } else if (const auto item = pendingItem(point); item && drawing::validItem(*item)) {
        points_.clear();
        emit itemCreated(*item);
    }
    update();
}

void DraftCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (panning_) {
        pan_ = event->position() - panStart_;
    } else {
        const auto point = toSheet(event->position());
        cursor_ = tool_ ? snapped(point) : point;
        emit positionChanged(cursor_);
        if (dragging_) {
            const QPointF delta(std::round(point.x() - dragStart_.x()), std::round(point.y() - dragStart_.y()));
            preview_ = moved(draft_.items[selected_], delta);
        }
    }
    update();
}

void DraftCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton && panning_) {
        panning_ = false;
        setCursor(tool_ ? Qt::CrossCursor : Qt::ArrowCursor);
    } else if (event->button() == Qt::LeftButton && dragging_) {
        const auto item = preview_;
        dragging_ = false;
        preview_.reset();
        if (item && drawing::validItem(*item) && *item != draft_.items[selected_]) {
            emit itemMoved(selected_, *item);
        }
        update();
    }
}

void DraftCanvas::wheelEvent(QWheelEvent* event)
{
    const double scroll = event->angleDelta().y();
    if (scroll == 0) {
        return;
    }
    const auto cursor = event->position();
    const auto anchor = toSheet(cursor);
    zoom_ = std::clamp(zoom_ * std::pow(1.18, scroll / 120), 0.35, 24.0);
    pan_ += cursor - (origin() + anchor * scale());
    update();
    event->accept();
}

}  // namespace graph3d
