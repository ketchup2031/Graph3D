#include "gui/DraftCanvas.h"
#include "gui/LineInput.h"

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
    if (item.anchorA || item.anchorB) {
        item.label += delta;
        return item;
    }
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

DraftCanvas::DraftCanvas(const drawing::Draft& draft, LineInput& lineInput, QWidget* parent)
    : QWidget(parent), draft_(draft), lineInput_(lineInput)
{
    setObjectName(QStringLiteral("draftCanvas"));
    setAccessibleName(tr("2D drawing sheet"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 220);
    connect(&lineInput_, &LineInput::previewChanged, this, qOverload<>(&QWidget::update));
    connect(&lineInput_, &LineInput::lineRequested, this, [this](QPointF delta) {
        if (tool_ == DraftKind::Line && !points_.empty()) {
            addLine(points_.front().position + QPointF(delta.x(), -delta.y()));
        }
    });
}

void DraftCanvas::setTool(std::optional<DraftKind> tool)
{
    cancel();
    tool_ = tool;
    lineInput_.setVisible(tool_ == DraftKind::Line);
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
    lineInput_.reset();
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

DraftCanvas::Pick DraftCanvas::snapped(QPointF page) const
{
    double best = 8 / scale();
    Pick result{{std::round(page.x()), std::round(page.y())}, {}};
    for (const auto& item : draft_.items) {
        for (const auto anchor : drawing::draftAnchors(item.kind)) {
            const auto point = drawing::draftAnchorPoint(item, anchor);
            if (!point) {
                continue;
            }
            const auto distance = QLineF(page, *point).length();
            if (distance < best) {
                best = distance;
                result = {*point, item.id.isNull() ? std::nullopt
                                                    : std::optional(drawing::DraftReference{item.id, anchor})};
            }
        }
    }
    return result;
}

std::optional<DraftItem> DraftCanvas::pendingItem(QPointF end) const
{
    if (!tool_ || points_.empty()) {
        return {};
    }
    if (*tool_ == DraftKind::Line) {
        if (const auto delta = lineInput_.exactDelta()) {
            end = points_.front().position + QPointF(delta->x(), -delta->y());
        }
    }
    const bool dimension = drawing::isLinearDimension(*tool_);
    if (dimension && points_.size() == 1) {
        return DraftItem{DraftKind::Line, points_.front().position, end};
    }
    DraftItem item{*tool_, points_.front().position, dimension ? points_[1].position : end,
                   dimension ? end : QPointF{}, {}, dimension ? drawing::DraftStroke::Solid : stroke_};
    if (dimension) {
        item.anchorA = points_[0].reference;
        item.anchorB = points_[1].reference;
    }
    return item;
}

void DraftCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().dark());
    painter.translate(origin());
    painter.scale(scale(), scale());
    drawing::paintDraft(painter, draft_, grid_, selected_, preview_ ? &*preview_ : nullptr);
    const auto size = drawing::draftSize(draft_);
    painter.setClipRect(QRectF(QPointF{}, size).marginsRemoved(QMarginsF(20, 5, 5, 5)));
    const auto ghost = pendingItem(cursor_);
    if (ghost && drawing::validItem(*ghost)) {
        drawing::paintDraftItem(painter, *ghost, true);
    }
    if (tool_ && underMouse()) {
        painter.setPen(QPen(QColor("#2469a5"), 0.18));
        painter.drawLine(cursor_ - QPointF(1, 0), cursor_ + QPointF(1, 0));
        painter.drawLine(cursor_ - QPointF(0, 1), cursor_ + QPointF(0, 1));
        if (drawing::isLinearDimension(*tool_) && points_.size() < 2 && cursorAttached_) {
            const double side = 8 / scale();
            painter.drawRect(QRectF(cursor_ - QPointF(side / 2, side / 2), QSizeF(side, side)));
        }
    }
}

bool DraftCanvas::canPlace(QPointF point) const
{
    const auto frame = QRectF(QPointF{}, drawing::draftSize(draft_)).marginsRemoved(QMarginsF(20, 5, 5, 5));
    const QRectF title(frame.right() - 145, frame.bottom() - 24, 145, 24);
    return frame.contains(point) && !title.contains(point);
}

void DraftCanvas::addLine(QPointF end)
{
    if (!canPlace(end)) {
        emit lineOutsideSheet();
        return;
    }
    const DraftItem item{DraftKind::Line, points_.front().position, end, {}, {}, stroke_};
    if (drawing::validItem(item)) {
        cancel();
        setFocus();
        emit itemCreated(item);
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
    if (!tool_) {
        int index = -1;
        if (canPlace(raw)) {
            const drawing::DraftGeometry geometry(draft_);
            for (int i = static_cast<int>(draft_.items.size()) - 1; i >= 0; --i) {
                if (drawing::draftHit(geometry.resolve(draft_.items[i]), raw, 6 / scale())) {
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
    if (!canPlace(raw)) {
        return;
    }
    const auto point = snapped(raw);
    if (*tool_ == DraftKind::Text) {
        bool accepted = false;
        auto text = QInputDialog::getText(this, tr("Text"), tr("Drawing note"), QLineEdit::Normal, {}, &accepted);
        if (accepted && !text.trimmed().isEmpty()) {
            emit itemCreated(DraftItem{DraftKind::Text, point.position, {}, {}, text.left(200)});
        }
        return;
    }
    const bool dimension = drawing::isLinearDimension(*tool_);
    if (points_.empty()) {
        points_.push_back(point);
        cursor_ = point.position;
        if (*tool_ == DraftKind::Line) {
            lineInput_.begin();
        }
    } else if (*tool_ == DraftKind::Line) {
        addLine(pendingItem(point.position)->b);
    } else if (dimension && points_.size() == 1) {
        if (*tool_ == DraftKind::Horizontal ? point.position.x() != points_[0].position.x() : point.position.y() != points_[0].position.y()) {
            points_.push_back(point);
        }
    } else if (const auto item = pendingItem(point.position); item && drawing::validItem(*item)) {
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
        const auto pick = tool_ ? snapped(point) : Pick{point, {}};
        cursor_ = pick.position;
        cursorAttached_ = pick.reference.has_value();
        emit positionChanged(cursor_);
        if (tool_ == DraftKind::Line && !points_.empty()) {
            const auto offset = cursor_ - points_.front().position;
            lineInput_.track({offset.x(), -offset.y()});
        }
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
