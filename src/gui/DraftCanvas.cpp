#include "gui/DraftCanvas.h"
#include "drawing/DraftCurves.h"
#include "gui/LineInput.h"

#include <QInputDialog>
#include <QKeyEvent>
#include <QLineF>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace graph3d {
namespace {

using drawing::DraftItem;
using drawing::DraftKind;

bool chainTool(std::optional<DraftKind> kind)
{
    return kind == DraftKind::Polyline || kind == DraftKind::Spline;
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
    modify_.reset();
    lineInput_.setVisible(tool_ == DraftKind::Line);
    if (tool_) {
        select(-1);
    }
    setCursor(tool ? Qt::CrossCursor : Qt::ArrowCursor);
}

void DraftCanvas::setModify(DraftModify mode)
{
    setTool({});
    select(-1);
    modify_ = mode;
    setCursor(Qt::CrossCursor);
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

void DraftCanvas::pickPoint(std::function<void(QPointF)> callback)
{
    cancel();
    pointPick_ = std::move(callback);
    setCursor(Qt::CrossCursor);
}

void DraftCanvas::cancel()
{
    if (pointPick_) {
        pointPick_ = {};
        unsetCursor();
    }
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
    using drawing::DraftAnchor;
    using drawing::DraftReference;
    const double tolerance = 8 / scale();
    double best = tolerance;
    double bestCurve = tolerance;
    Pick result{{std::round(page.x()), std::round(page.y())}, {}};
    auto onCurve = result;
    struct Nearby { QUuid id; drawing::DraftCurve curve; std::size_t index; };
    std::vector<Nearby> nearby;
    const auto consider = [&](QPointF point, DraftReference reference) {
        const auto distance = QLineF(page, point).length();
        if (distance < best) {
            best = distance;
            result = {point, reference.item.isNull() ? std::nullopt : std::optional(reference)};
        }
    };
    if (chainTool(tool_) && points_.size() > 2) {
        consider(points_.front().position, {});
    }
    for (const auto& item : draft_.items) {
        if (!drawing::layerVisible(draft_, item.layer)) {
            continue;
        }
        for (const auto anchor : drawing::draftAnchors(item.kind)) {
            if (const auto point = drawing::draftAnchorPoint(item, anchor)) {
                consider(*point, {item.id, anchor});
            }
        }
        const auto curves = drawing::draftCurves(item);
        for (std::size_t i = 0; i < curves.size(); ++i) {
            const auto& curve = curves[i];
            const double t = curve.nearest(page);
            const auto point = curve.at(t);
            const double distance = QLineF(point, page).length();
            if (distance < tolerance) {
                nearby.push_back({item.id, curve, i});
                if (distance < bestCurve) {
                    bestCurve = distance;
                    onCurve = {point, DraftReference{item.id, DraftAnchor::Curve, i + t}};
                }
            }
            consider(curve.at(.5), {item.id, DraftAnchor::Curve, i + .5});
            if (item.kind == DraftKind::Polyline) {
                consider(curve.at(0), {item.id, DraftAnchor::Curve, static_cast<double>(i)});
            }
        }
    }
    // Only curves within snapping distance can participate in a nearby crossing.
    if (best > drawing::draftTolerance) {
        for (std::size_t i = 0; i < nearby.size(); ++i) {
            for (std::size_t j = i + 1; j < nearby.size(); ++j) {
                const auto& a = nearby[i];
                const auto& b = nearby[j];
                if (a.id != b.id) {
                    for (const auto& hit : drawing::draftIntersections(a.curve, b.curve)) {
                        consider(hit.point, {a.id, DraftAnchor::Intersection, a.index + hit.parameter, b.id});
                    }
                }
            }
        }
    }
    if (tool_ && drawing::isLinearDimension(*tool_)) {
        if (const auto attached = modelPick(page)) {
            const double distance = QLineF(page, attached->position).length();
            if (distance < std::min(best, bestCurve)) {
                return {attached->position, attached->reference};
            }
        }
    }
    return best < tolerance ? result : onCurve;
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
    const auto kind = *tool_;
    const bool dimension = drawing::isDimension(kind);
    if (drawing::isAnnotation(kind) || (dimension && points_.size() == 1)
        || (kind == DraftKind::Arc && points_.size() == 1)) {
        return DraftItem{DraftKind::Line, points_.front().position, end};
    }
    DraftItem item{kind, points_.front().position, end, {}, {}, dimension ? drawing::DraftStroke::Solid : stroke_};
    if (chainTool(kind)) {
        for (const auto& point : points_) {
            item.points.push_back(point.position);
        }
        if (QLineF(item.points.back(), end).length() >= .001) {
            item.points.push_back(end);
        }
    } else if (kind == DraftKind::Arc) {
        item.b = points_[1].position;
        item.label = end;
    } else if (dimension) {
        if (kind == DraftKind::Angular && points_.size() == 2) {
            return DraftItem{DraftKind::Polyline, {}, {}, {}, {}, stroke_, {}, {}, {}, {},
                             {points_[1].position, points_[0].position, end}};
        }
        item.b = points_[1].position;
        item.label = end;
        item.anchorA = points_[0].reference;
        item.anchorB = points_[1].reference;
        if (kind == DraftKind::Angular) {
            item.anchorC = points_[2].reference;
            item.points.push_back(points_[2].position);
        }
    }
    return item;
}

void DraftCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().dark());
    painter.translate(origin());
    painter.scale(scale(), scale());
    drawing::paintDraft(painter, draft_, grid_, selected_, preview_ ? &*preview_ : nullptr, page_, total_);
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
        if (drawing::isDimension(*tool_) && points_.size() < (*tool_ == DraftKind::Angular ? 3 : 2) && cursorAttached_) {
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

void DraftCanvas::finishPolyline()
{
    if (points_.size() >= 2) {
        const auto item = pendingItem(points_.back().position);
        if (item && drawing::validItem(*item)) {
            cancel();
            emit itemCreated(*item);
        }
    }
}

int DraftCanvas::hitItem(QPointF point, bool curvesOnly) const
{
    const drawing::DraftGeometry geometry(draft_);
    int hatch = -1;
    for (int i = static_cast<int>(draft_.items.size()) - 1; i >= 0; --i) {
        const auto& item = draft_.items[i];
        if (!drawing::layerEditable(draft_, item.layer)) {
            continue;
        }
        if (curvesOnly && (drawing::draftAnchors(item.kind).empty()
            || item.kind == DraftKind::ConstructionLine || item.kind == DraftKind::Ellipse
            || item.kind == DraftKind::Spline)) {
            continue;
        }
        if (drawing::draftHit(drawing::isDimension(item.kind) || item.kind == DraftKind::Hatch
            ? geometry.resolve(item) : item, point, 6 / scale())) {
            if (item.kind != DraftKind::Hatch) {
                return i;
            }
            if (hatch < 0) {
                hatch = i;
            }
        }
    }
    return hatch;
}

void DraftCanvas::handleModify(QPointF point)
{
    const int index = hitItem(point, true);
    if (index < 0) {
        emit toolMessage(tr("Click the geometry to modify."));
        return;
    }
    const auto& source = draft_.items[index];
    switch (*modify_) {
    case DraftModify::Trim:
        if (const auto pieces = drawing::trimDraft(source, draft_, point); !pieces.empty()) {
            emit changeRequested(index, pieces);
            return;
        }
        break;
    case DraftModify::Extend:
        if (const auto extended = drawing::extendDraft(source, draft_, point)) {
            emit changeRequested(index, {*extended});
            return;
        }
        break;
    case DraftModify::Offset: {
        bool accepted = false;
        const auto distance = QInputDialog::getDouble(this, tr("Parallel offset"),
            tr("Signed offset (mm). Positive increases a circle radius or offsets left along a path."),
            5, -10000, 10000, 3, &accepted);
        if (!accepted) {
            return;
        }
        if (const auto offset = drawing::offsetDraft(source, distance)) {
            emit itemCreated(*offset);
            return;
        }
        break;
    }
    }
    emit toolMessage(tr("No valid result. Check the intersections or offset."));
}

std::optional<drawing::ModelPick> DraftCanvas::modelPick(QPointF point, bool circularOnly) const
{
    return draft_.modelViews ? drawing::pickModelGeometry(*draft_.modelViews, point, 8 / scale(), circularOnly)
                             : std::nullopt;
}

std::optional<DraftCanvas::Pick> DraftCanvas::circularPick(QPointF point) const
{
    std::optional<Pick> result;
    double best = 8 / scale();
    for (const auto& item : draft_.items) {
        if (!drawing::layerVisible(draft_, item.layer)) {
            continue;
        }
        if (item.kind != DraftKind::Circle && item.kind != DraftKind::Arc) {
            continue;
        }
        const auto curves = drawing::draftCurves(item);
        if (curves.empty()) {
            continue;
        }
        const auto& curve = curves.front();
        const double t = curve.nearest(point);
        const auto edge = curve.at(t);
        const auto distance = QLineF(point, edge).length();
        if (distance < best) {
            best = distance;
            result = Pick{edge, drawing::DraftReference{item.id, drawing::DraftAnchor::Curve, t}};
        }
    }
    if (const auto attached = modelPick(point, true);
        attached && QLineF(point, attached->position).length() < best) {
        return Pick{attached->position, attached->reference};
    }
    return result;
}

void DraftCanvas::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        panStart_ = event->position() - pan_;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() == Qt::RightButton && chainTool(tool_)) {
        finishPolyline();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    setFocus();
    const auto raw = toSheet(event->position());
    if (pointPick_) {
        if (canPlace(raw)) {
            auto callback = std::exchange(pointPick_, {});
            unsetCursor();
            callback(raw);
        }
        return;
    }
    if (modify_) {
        if (canPlace(raw)) {
            handleModify(raw);
        }
        return;
    }
    if (!tool_) {
        const int index = canPlace(raw) ? hitItem(raw) : -1;
        select(index);
        dragging_ = index >= 0;
        dragStart_ = raw;
        return;
    }
    if (!canPlace(raw)) {
        return;
    }
    if (*tool_ == DraftKind::Hatch) {
        if (const auto hatch = drawing::hatchDraft(draft_, raw)) {
            emit itemCreated(*hatch);
        } else {
            emit toolMessage(tr("Click inside a closed contour."));
        }
        return;
    }
    if ((*tool_ == DraftKind::Radius || *tool_ == DraftKind::Diameter) && points_.empty()) {
        if (const auto point = circularPick(raw)) {
            auto center = *point->reference;
            center.anchor = drawing::isModelReference(center) ? drawing::DraftAnchor::ModelCenter
                                                             : drawing::DraftAnchor::Center;
            center.parameter = 0;
            if (const auto origin = drawing::DraftGeometry(draft_).point(center)) {
                points_ = {{*origin, center}, *point};
            }
            cursor_ = raw;
            emit toolMessage(tr("Place the dimension label."));
            update();
        } else {
            emit toolMessage(tr("Click a circle or arc."));
        }
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
    if (drawing::isAnnotation(*tool_)) {
        if (points_.empty()) {
            points_.push_back(point);
            emit toolMessage(tr("Place the annotation label."));
            return;
        }
        const auto anchor = points_.front().position;
        points_.clear();
        const bool tolerance = *tool_ == DraftKind::Tolerance;
        const auto label = tolerance ? tr("Symbol | tolerance | optional datums") : tr("Annotation text");
        bool accepted = false;
        const auto defaultText = *tool_ == DraftKind::Datum ? QStringLiteral("A")
            : tolerance ? QStringLiteral("⊥ | 0.05 | A")
            : *tool_ == DraftKind::Roughness ? QStringLiteral("Ra 3.2")
            : *tool_ == DraftKind::Thread ? QStringLiteral("M10 × 1.5")
            : *tool_ == DraftKind::Weld ? QStringLiteral("△4") : QString{};
        const auto text = QInputDialog::getText(this, tr("Annotation"), label, QLineEdit::Normal, defaultText, &accepted).trimmed();
        if (accepted) {
            const DraftItem item{*tool_, anchor, {}, point.position, text};
            if (drawing::validItem(item)) {
                emit itemCreated(item);
            } else {
                emit toolMessage(tr("Enter valid annotation text."));
            }
        }
        update();
        return;
    }
    const bool dimension = drawing::isLinearDimension(*tool_);
    if (chainTool(tool_)) {
        if (points_.size() > 2 && point.position == points_.front().position) {
            points_.push_back(point);
            finishPolyline();
        } else if (points_.size() < (*tool_ == DraftKind::Spline ? 511u : 19999u)
                   && (points_.empty() || QLineF(points_.back().position, point.position).length() >= .001)) {
            points_.push_back(point);
        }
        update();
        return;
    }
    if (points_.empty()) {
        points_.push_back(point);
        cursor_ = point.position;
        if (*tool_ == DraftKind::Line) {
            lineInput_.begin();
        }
    } else if (*tool_ == DraftKind::Line) {
        addLine(pendingItem(point.position)->b);
    } else if (dimension && points_.size() == 1) {
        const auto onView = [](const auto& p) { return p.reference && drawing::isModelReference(*p.reference); };
        if (onView(points_.front()) != onView(point)
            || (onView(point) && points_.front().reference->view != point.reference->view)) {
            emit toolMessage(tr("Both measured points must belong to the same view."));
            return;
        }
        const auto delta = point.position - points_[0].position;
        const bool valid = *tool_ == DraftKind::Horizontal ? std::abs(delta.x()) >= .001
                         : *tool_ == DraftKind::Vertical ? std::abs(delta.y()) >= .001
                         : std::hypot(delta.x(), delta.y()) >= .001;
        if (valid) {
            points_.push_back(point);
        }
    } else if ((*tool_ == DraftKind::Arc && points_.size() == 1)
               || (*tool_ == DraftKind::Angular && points_.size() < 3)) {
        if (QLineF(point.position, points_.front().position).length() >= .001) {
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
            preview_ = drawing::translateDraftItem(draft_.items[selected_], delta);
        }
    }
    update();
}

void DraftCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton && panning_) {
        panning_ = false;
        setCursor(tool_ || modify_ ? Qt::CrossCursor : Qt::ArrowCursor);
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

void DraftCanvas::keyPressEvent(QKeyEvent* event)
{
    if (chainTool(tool_) && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        finishPolyline();
        return;
    }
    QWidget::keyPressEvent(event);
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
