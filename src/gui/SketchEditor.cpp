#include "gui/SketchEditor.h"
#include "geometry/SketchConstraints.h"
#include "gui/InterfaceLanguage.h"
#include "gui/LineInput.h"

#include <GeomAbs_CurveType.hxx>

#include <QAction>
#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QListWidget>
#include <QMenu>
#include <QKeyEvent>
#include <QSignalBlocker>
#include <QToolButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <type_traits>
#include <exception>

namespace graph3d {
namespace {

enum class Tool { Line, Rectangle, Circle, Select };
using geometry::ConstraintKind;
using geometry::SketchRef;
constexpr double snap = 5.0;

QString constraintName(ConstraintKind kind)
{
    switch (kind) {
    case ConstraintKind::Coincident: return SketchEditor::tr("Coincident");
    case ConstraintKind::Horizontal: return SketchEditor::tr("Horizontal");
    case ConstraintKind::Vertical: return SketchEditor::tr("Vertical");
    case ConstraintKind::Perpendicular: return SketchEditor::tr("Perpendicular");
    case ConstraintKind::Length: return SketchEditor::tr("Driving length");
    case ConstraintKind::Diameter: return SketchEditor::tr("Driving diameter");
    case ConstraintKind::OnProjection: return SketchEditor::tr("Link to projected edge");
    }
    return {};
}

bool dimension(ConstraintKind kind)
{
    return kind == ConstraintKind::Length || kind == ConstraintKind::Diameter;
}

bool twoPicks(ConstraintKind kind)
{
    return kind == ConstraintKind::Coincident || kind == ConstraintKind::Perpendicular || kind == ConstraintKind::OnProjection;
}

double pointDistance(QPointF a, QPointF b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

}  // namespace

class SketchEditor::Canvas final : public QWidget {
public:
    explicit Canvas(LineInput& input, QWidget* parent) : QWidget(parent), lineInput(input)
    {
        setObjectName(QStringLiteral("sketchCanvas"));
        setAccessibleName(SketchEditor::tr("Sketch drawing area"));
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(320, 240);
        connect(&lineInput, &LineInput::previewChanged, this, qOverload<>(&QWidget::update));
        connect(&lineInput, &LineInput::lineRequested, this, [this](QPointF delta) {
            if (activeTool == Tool::Line && pending) {
                finishShape({pending->u + delta.x(), pending->v + delta.y()});
            }
        });
    }

    void begin(const geometry::Sketch& initial, std::vector<geometry::SketchProjection> edges)
    {
        sketch = initial;
        projections = std::move(edges);
        history.clear();
        selection = -1;
        relation.reset();
        firstPick.reset();
        clearPending();
        cursor.reset();
        pan = {};
        pixelsPerMillimetre = 3.0;
        if (!projections.empty()) {
            double minU = 0, maxU = 0, minV = 0, maxV = 0;
            const auto first = projections.front().points.front();
            minU = maxU = first.u;
            minV = maxV = first.v;
            for (const auto& edge : projections) {
                for (const auto point : edge.points) {
                    minU = std::min(minU, point.u);
                    maxU = std::max(maxU, point.u);
                    minV = std::min(minV, point.v);
                    maxV = std::max(maxV, point.v);
                }
            }
            pixelsPerMillimetre = std::clamp(300.0 / std::max({maxU - minU, maxV - minV, 1.0}), .4, 3.0);
            pan = QPointF(-(minU + maxU), minV + maxV) * pixelsPerMillimetre / 2;
        }
        resolve();
    }

    void setTool(Tool tool)
    {
        activeTool = tool;
        relation.reset();
        firstPick.reset();
        selection = -1;
        clearPending();
        changed();
        lineInput.setVisible(activeTool == Tool::Line);
        update();
    }

    void undo()
    {
        if (pending || relation) {
            relation.reset();
            firstPick.reset();
            clearPending();
        } else if (!history.empty()) {
            sketch = std::move(history.back());
            history.pop_back();
            selection = -1;
            resolve();
        }
        update();
    }

    void startConstraint(ConstraintKind kind)
    {
        setTool(Tool::Select);
        relation = kind;
        if (kind == ConstraintKind::Coincident || kind == ConstraintKind::OnProjection) {
            feedback(SketchEditor::tr("Pick a sketch point."));
        } else if (kind == ConstraintKind::Diameter) {
            feedback(SketchEditor::tr("Pick a circle."));
        } else {
            feedback(SketchEditor::tr("Pick a straight edge."));
        }
        setFocus();
    }

    void editConstraint(int index)
    {
        if (index < 0 || index >= static_cast<int>(sketch.constraints.size())) {
            return;
        }
        auto item = sketch.constraints[index];
        if (dimension(item.kind) && editDimension(item) && item != sketch.constraints[index]) {
            saveStep();
            sketch.constraints[index] = item;
            resolve();
        }
    }

    void removeConstraint(int index)
    {
        if (index >= 0 && index < static_cast<int>(sketch.constraints.size())) {
            saveStep();
            sketch.constraints.erase(sketch.constraints.begin() + index);
            resolve();
        }
    }

    void removeEntity()
    {
        if (selection < 0) {
            return;
        }
        saveStep();
        const auto usesSecond = [](ConstraintKind kind) {
            return kind == ConstraintKind::Coincident || kind == ConstraintKind::Perpendicular;
        };
        std::erase_if(sketch.constraints, [&](const auto& item) {
            return item.first.entity == selection || (usesSecond(item.kind) && item.second.entity == selection);
        });
        for (auto& item : sketch.constraints) {
            if (item.first.entity > selection) {
                --item.first.entity;
            }
            if (usesSecond(item.kind) && item.second.entity > selection) {
                --item.second.entity;
            }
        }
        sketch.entities.erase(sketch.entities.begin() + selection);
        selection = -1;
        resolve();
    }

    void editEntity()
    {
        if (selection < 0) {
            return;
        }
        const auto previous = solution.entities[selection];
        auto candidate = previous;
        QDialog dialog(this);
        dialog.setWindowTitle(SketchEditor::tr("Sketch geometry"));
        auto* form = new QFormLayout(&dialog);
        const auto field = [&](const QString& label, double value, double minimum = -1.e6) {
            auto* spin = new QDoubleSpinBox(&dialog);
            spin->setDecimals(5);
            spin->setRange(minimum, 1.e6);
            spin->setValue(value);
            form->addRow(label, spin);
            return spin;
        };
        const auto fields = [&](geometry::Point2 p) {
            return std::pair{field(SketchEditor::tr("U coordinate (mm)"), p.u),
                             field(SketchEditor::tr("V coordinate (mm)"), p.v)};
        };
        const auto read = [](auto point) { return geometry::Point2{point.first->value(), point.second->value()}; };
        const auto a = fields(geometry::sketchVertex(candidate, 0));
        std::optional<decltype(fields({}))> b;
        QDoubleSpinBox* radius = nullptr;
        if (const auto* circle = std::get_if<geometry::Circle>(&candidate)) {
            radius = field(SketchEditor::tr("Radius"), circle->radius, .00001);
        } else {
            b = fields(geometry::sketchVertex(candidate, std::holds_alternative<geometry::Rectangle>(candidate) ? 2 : 1));
        }
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        if (radius) {
            candidate = geometry::Circle{read(a), radius->value()};
        } else if (std::holds_alternative<geometry::Rectangle>(candidate)) {
            candidate = geometry::Rectangle{read(a), read(*b)};
        } else {
            candidate = geometry::Line{read(a), read(*b)};
        }
        if (candidate == previous) {
            return;
        }
        saveStep();
        const auto original = sketch.entities[selection];
        sketch.entities[selection] = candidate;
        if (const auto* line = std::get_if<geometry::Line>(&original)) {
            const auto edited = std::get<geometry::Line>(candidate);
            for (int i = 0; i < static_cast<int>(sketch.entities.size()); ++i) {
                if (i == selection) {
                    continue;
                }
                if (auto* neighbor = std::get_if<geometry::Line>(&sketch.entities[i])) {
                    for (auto* endpoint : {&neighbor->start, &neighbor->end}) {
                        if (*endpoint == line->start) {
                            *endpoint = edited.start;
                        } else if (*endpoint == line->end) {
                            *endpoint = edited.end;
                        }
                    }
                }
            }
        }
        resolve();
    }

    void highlightConstraint(int index)
    {
        currentConstraint = index;
        update();
    }

    bool hasSelection() const { return selection >= 0; }
    bool hasProjections() const { return !projections.empty(); }

    geometry::Sketch sketch;
    geometry::SketchSolution solution;
    QString error;
    std::function<void(const QString&)> feedback;
    std::function<void()> changed;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(25, 32, 43));
        const double spacing = snap * pixelsPerMillimetre * (pixelsPerMillimetre < 2 ? 5 : 1);
        const QPointF origin = screenOrigin();
        painter.setPen(QColor(47, 57, 73));
        for (double x = std::fmod(origin.x(), spacing); x < width(); x += spacing) {
            painter.drawLine(QLineF(x, 0, x, height()));
        }
        for (double y = std::fmod(origin.y(), spacing); y < height(); y += spacing) {
            painter.drawLine(QLineF(0, y, width(), y));
        }
        painter.setPen(QColor(156, 92, 91));
        painter.drawLine(QLineF(0, origin.y(), width(), origin.y()));
        painter.setPen(QColor(91, 141, 101));
        painter.drawLine(QLineF(origin.x(), 0, origin.x(), height()));
        painter.drawText(origin + QPointF(7, 16), QStringLiteral("0"));

        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(145, 161, 183), 1, Qt::DashLine));
        for (const auto& edge : projections) {
            QPolygonF polyline;
            for (const auto p : edge.points) {
                polyline.append(toPixel(p));
            }
            painter.drawPolyline(polyline);
        }
        const auto* highlighted = currentConstraint >= 0 && currentConstraint < static_cast<int>(sketch.constraints.size())
            ? &sketch.constraints[currentConstraint] : nullptr;
        for (int i = 0; i < static_cast<int>(solution.entities.size()); ++i) {
            const bool related = highlighted && (i == highlighted->first.entity
                || ((highlighted->kind == ConstraintKind::Coincident || highlighted->kind == ConstraintKind::Perpendicular)
                    && i == highlighted->second.entity));
            painter.setPen(QPen(i == selection || related ? QColor(245, 193, 99) : QColor(90, 209, 250), 2));
            draw(painter, solution.entities[i]);
        }
        if (relation && (*relation == ConstraintKind::Coincident || *relation == ConstraintKind::OnProjection)) {
            painter.setPen(QPen(QColor(245, 193, 99), 1));
            for (const auto& entity : solution.entities) {
                for (int part = 0; part < geometry::sketchVertexCount(entity); ++part) {
                    painter.drawEllipse(toPixel(geometry::sketchVertex(entity, part)), 2.5, 2.5);
                }
            }
        }
        if (relation && firstPick) {
            painter.setPen(QPen(QColor(245, 193, 99), 2));
            if (*relation == ConstraintKind::Perpendicular) {
                const auto edge = geometry::sketchSegment(solution.entities[firstPick->entity], firstPick->part);
                painter.drawLine(toPixel(edge.start), toPixel(edge.end));
            } else {
                painter.drawEllipse(toPixel(geometry::sketchVertex(solution.entities[firstPick->entity], firstPick->part)), 4, 4);
            }
        }
        if (pending && cursor) {
            painter.setPen(QPen(QColor(230, 240, 250), 1, Qt::DashLine));
            draw(painter, makeEntity(*pending, endpoint(*cursor)));
        }
        if (cursor) {
            const auto p = toPixel(*cursor);
            painter.setPen(QColor(234, 239, 246));
            painter.drawLine(p - QPointF(5, 0), p + QPointF(5, 0));
            painter.drawLine(p - QPointF(0, 5), p + QPointF(0, 5));
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        previous = event->position();
        if (event->button() == Qt::RightButton) {
            relation.reset();
            firstPick.reset();
            clearPending();
            update();
            return;
        }
        if (event->button() != Qt::LeftButton) {
            return;
        }
        if (relation) {
            pickConstraint(event->position());
            return;
        }
        if (activeTool == Tool::Select) {
            const auto pick = pickGeometry(event->position(), false, true, true);
            selection = pick ? pick->entity : -1;
            changed();
            update();
            return;
        }
        cursor = toSnappedModel(event->position());
        if (!pending) {
            pending = cursor;
            if (activeTool == Tool::Line) {
                lineInput.begin();
                feedback(SketchEditor::tr("Click the endpoint or enter a length and angle."));
            } else {
                feedback(SketchEditor::tr("Choose the second point."));
            }
        } else {
            finishShape(endpoint(*cursor));
        }
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (event->buttons().testFlag(Qt::MiddleButton)) {
            pan += event->position() - previous;
        }
        previous = event->position();
        cursor = toSnappedModel(event->position());
        if (!relation) {
            reportPosition();
        }
        update();
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Escape) {
            relation.reset();
            firstPick.reset();
            clearPending();
            update();
            return;
        }
        if (event->key() == Qt::Key_Delete) {
            removeEntity();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override
    {
        const double old = pixelsPerMillimetre;
        const double steps = std::clamp(event->angleDelta().y() / 120.0, -5.0, 5.0);
        pixelsPerMillimetre = std::clamp(old * std::pow(1.2, steps), 0.4, 24.0);
        const QPointF at = event->position();
        pan += (at - screenOrigin()) * (1.0 - pixelsPerMillimetre / old);
        cursor = toSnappedModel(at);
        reportPosition();
        update();
        event->accept();
    }

private:
    struct ProjectionPick {
        int index;
        double position;
        geometry::Point2 point;
    };

    void resolve()
    {
        error.clear();
        try {
            solution = geometry::solveSketch(sketch, projections);
            if (!solution.projectionError.empty()) {
                error = localizedDiagnostic(solution.projectionError.c_str());
            } else if (!solution.conflicts.empty()) {
                error = SketchEditor::tr("Sketch constraints conflict. Check the highlighted constraints.");
            }
        } catch (const std::exception& failure) {
            solution = {sketch.entities, {}};
            error = localizedDiagnostic(failure.what());
        }
        changed();
        update();
    }

    void saveStep()
    {
        constexpr std::size_t historyLimit = 64;
        if (history.size() == historyLimit) {
            history.erase(history.begin());
        }
        history.push_back(sketch);
    }

    std::optional<ProjectionPick> pickProjection(QPointF screen, double best) const
    {
        std::optional<ProjectionPick> result;
        const double limit = best;
        for (int i = 0; i < static_cast<int>(projections.size()); ++i) {
            const auto& points = projections[i].points;
            for (std::size_t j = 0; j < points.size(); ++j) {
                const double distance = pointDistance(screen, toPixel(points[j]));
                if (distance < best) {
                    best = distance;
                    result = ProjectionPick{i, static_cast<double>(j) / (points.size() - 1), points[j]};
                }
            }
        }
        if (result) {
            return result;
        }
        best = limit;
        for (int i = 0; i < static_cast<int>(projections.size()); ++i) {
            const auto& projection = projections[i];
            if (projection.edge.curveType != GeomAbs_Line) {
                continue;
            }
            const auto a = projection.points.front();
            const auto b = projection.points.back();
            const auto start = toPixel(a);
            const auto delta = toPixel(b) - start;
            const auto norm = QPointF::dotProduct(delta, delta);
            if (norm <= 0) {
                continue;
            }
            const auto t = std::clamp(QPointF::dotProduct(screen - start, delta) / norm, 0.0, 1.0);
            const auto dist = pointDistance(screen, start + delta * t);
            if (dist < best) {
                best = dist;
                result = ProjectionPick{i, t, {std::lerp(a.u, b.u, t), std::lerp(a.v, b.v, t)}};
            }
        }
        return result;
    }

    std::optional<SketchRef> pickGeometry(QPointF screen, bool vertices, bool segments, bool circles,
                                         double best = 9) const
    {
        std::optional<SketchRef> result;
        const auto consider = [&](double distance, int index, int part) {
            if (distance < best) {
                best = distance;
                result = SketchRef{index, part};
            }
        };
        for (int i = 0; i < static_cast<int>(solution.entities.size()); ++i) {
            const auto& shape = solution.entities[i];
            if (vertices) {
                for (int part = 0; part < geometry::sketchVertexCount(shape); ++part) {
                    consider(pointDistance(screen, toPixel(geometry::sketchVertex(shape, part))), i, part);
                }
            }
            if (segments) {
                for (int part = 0; part < geometry::sketchSegmentCount(shape); ++part) {
                    const auto edge = geometry::sketchSegment(shape, part);
                    const auto start = toPixel(edge.start);
                    const auto axis = toPixel(edge.end) - start;
                    const auto norm = QPointF::dotProduct(axis, axis);
                    const auto t = norm > 0 ? std::clamp(QPointF::dotProduct(screen - start, axis) / norm, 0.0, 1.0) : 0;
                    consider(pointDistance(screen, start + axis * t), i, part);
                }
            }
            if (circles) {
                if (const auto* circle = std::get_if<geometry::Circle>(&shape)) {
                    consider(std::abs(pointDistance(screen, toPixel(circle->center)) - circle->radius * pixelsPerMillimetre), i, 0);
                }
            }
        }
        return result;
    }

    bool editDimension(geometry::SketchConstraint& item)
    {
        QInputDialog dialog(this);
        dialog.setObjectName(QStringLiteral("sketchDimensionDialog"));
        dialog.setWindowTitle(constraintName(item.kind));
        dialog.setLabelText(SketchEditor::tr("Dimension (mm)"));
        dialog.setInputMode(QInputDialog::DoubleInput);
        dialog.setDoubleDecimals(5);
        dialog.setDoubleRange(.00001, 1000000);
        dialog.setDoubleValue(item.value);
        if (dialog.exec() != QDialog::Accepted) {
            return false;
        }
        item.value = dialog.doubleValue();
        return true;
    }

    void pickConstraint(QPointF position)
    {
        const auto kind = *relation;
        geometry::SketchConstraint next{kind};
        if (kind == ConstraintKind::OnProjection && firstPick) {
            const auto projected = pickProjection(position, 14);
            if (!projected) {
                feedback(SketchEditor::tr("Pick a snap point on a projected edge."));
                return;
            }
            next.first = *firstPick;
            next.edge = projections[projected->index].edge;
            next.value = projected->position;
        } else {
            const bool vertex = kind == ConstraintKind::Coincident || kind == ConstraintKind::OnProjection;
            const auto picked = pickGeometry(position, vertex, !vertex && kind != ConstraintKind::Diameter,
                                              kind == ConstraintKind::Diameter);
            if (!picked) {
                return;
            }
            if (twoPicks(kind) && !firstPick) {
                firstPick = picked;
                feedback(kind == ConstraintKind::OnProjection ? SketchEditor::tr("Pick a snap point on a projected edge.")
                    : kind == ConstraintKind::Coincident ? SketchEditor::tr("Pick the second point.")
                                                         : SketchEditor::tr("Pick the second straight edge."));
                update();
                return;
            }
            if (firstPick == picked) {
                return;
            }
            next.first = firstPick.value_or(*picked);
            next.second = *picked;
        }
        if (dimension(kind)) {
            const auto& entity = solution.entities[next.first.entity];
            if (kind == ConstraintKind::Diameter) {
                next.value = std::get<geometry::Circle>(entity).radius * 2;
            } else {
                const auto line = geometry::sketchSegment(entity, next.first.part);
                next.value = std::hypot(line.end.u - line.start.u, line.end.v - line.start.v);
            }
            if (!editDimension(next)) {
                firstPick.reset();
                relation.reset();
                update();
                return;
            }
        }
        if (kind != ConstraintKind::Perpendicular && kind != ConstraintKind::Coincident) {
            next.second = {};
        }
        saveStep();
        sketch.constraints.push_back(std::move(next));
        firstPick.reset();
        relation.reset();
        resolve();
        feedback(SketchEditor::tr("Constraint added."));
    }

    void clearPending()
    {
        pending.reset();
        lineInput.reset();
    }

    geometry::Point2 endpoint(geometry::Point2 mouse) const
    {
        if (activeTool == Tool::Line && pending) {
            if (const auto delta = lineInput.exactDelta()) {
                return {pending->u + delta->x(), pending->v + delta->y()};
            }
        }
        return mouse;
    }

    void finishShape(geometry::Point2 end)
    {
        const double du = std::abs(pending->u - end.u);
        const double dv = std::abs(pending->v - end.v);
        if ((du == 0 && dv == 0) || (activeTool == Tool::Rectangle && (du == 0 || dv == 0))) {
            feedback(SketchEditor::tr("Choose a distinct point with a nonzero size."));
        } else {
            saveStep();
            const auto id = static_cast<int>(sketch.entities.size());
            sketch.entities.push_back(makeEntity(*pending, end));
            for (int part : {0, activeTool == Tool::Rectangle ? 2 : 1}) {
                if (activeTool == Tool::Circle && part != 0) {
                    continue;
                }
                const auto p = geometry::sketchVertex(sketch.entities.back(), part);
                if (const auto vertex = pickGeometry(toPixel(p), true, false, false, .001);
                    vertex && geometry::sketchVertex(sketch.entities[vertex->entity], vertex->part) != p) {
                    sketch.constraints.push_back({ConstraintKind::Coincident, {id, part}, *vertex});
                } else if (const auto projection = pickProjection(toPixel(p), .001)) {
                    sketch.constraints.push_back({ConstraintKind::OnProjection, {id, part}, {}, projection->position,
                                                  projections[projection->index].edge});
                }
            }
            resolve();
            clearPending();
            setFocus();
            reportPosition();
        }
        update();
    }

    QPointF screenOrigin() const
    {
        return QPointF(width() / 2.0, height() / 2.0) + pan;
    }

    geometry::Point2 toSnappedModel(QPointF p) const
    {
        if (activeTool == Tool::Select) {
            const auto offset = (p - screenOrigin()) / pixelsPerMillimetre;
            return {offset.x(), -offset.y()};
        }
        if (const auto vertex = pickGeometry(p, true, false, false)) {
            return geometry::sketchVertex(solution.entities[vertex->entity], vertex->part);
        }
        if (const auto projection = pickProjection(p, 8)) {
            return projection->point;
        }
        const auto offset = (p - screenOrigin()) / pixelsPerMillimetre;
        return {std::round(offset.x() / snap) * snap, std::round(-offset.y() / snap) * snap};
    }

    QPointF toPixel(geometry::Point2 p) const
    {
        return screenOrigin() + QPointF(p.u, -p.v) * pixelsPerMillimetre;
    }

    geometry::SketchEntity makeEntity(geometry::Point2 a, geometry::Point2 b) const
    {
        switch (activeTool) {
        case Tool::Line: return geometry::Line{a, b};
        case Tool::Rectangle: return geometry::Rectangle{a, b};
        case Tool::Circle: return geometry::Circle{a, std::hypot(b.u - a.u, b.v - a.v)};
        case Tool::Select: break;
        }
        return geometry::Line{a, b};
    }

    void draw(QPainter& painter, const geometry::SketchEntity& entity) const
    {
        std::visit([this, &painter](const auto& shape) {
            using Shape = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<Shape, geometry::Line>) {
                painter.drawLine(toPixel(shape.start), toPixel(shape.end));
            } else if constexpr (std::is_same_v<Shape, geometry::Rectangle>) {
                painter.drawRect(QRectF(toPixel(shape.first), toPixel(shape.opposite)).normalized());
            } else {
                const double radius = shape.radius * pixelsPerMillimetre;
                painter.drawEllipse(toPixel(shape.center), radius, radius);
            }
        }, entity);
    }

    void reportPosition()
    {
        if (cursor) {
            if (activeTool == Tool::Line && pending) {
                lineInput.track({cursor->u - pending->u, cursor->v - pending->v});
            }
            feedback(SketchEditor::tr("U: %1 mm   V: %2 mm   |   Snap: 5 mm")
                .arg(cursor->u, 0, 'f', 0).arg(cursor->v, 0, 'f', 0));
        }
    }

    std::vector<geometry::Sketch> history;
    std::vector<geometry::SketchProjection> projections;
    std::optional<ConstraintKind> relation;
    std::optional<SketchRef> firstPick;
    int selection = -1;
    int currentConstraint = -1;
    LineInput& lineInput;
    Tool activeTool = Tool::Line;
    std::optional<geometry::Point2> pending;
    std::optional<geometry::Point2> cursor;
    QPointF previous;
    QPointF pan;
    double pixelsPerMillimetre = 3.0;
};

SketchEditor::SketchEditor(QWidget* parent)
    : QWidget(parent), lineInput_(new LineInput(this)), canvas_(new Canvas(*lineInput_, this)),
      planeLabel_(new QLabel(this)), feedback_(new QLabel(this))
{
    setObjectName(QStringLiteral("sketchEditor"));
    auto* layout = new QVBoxLayout(this);
    auto* tools = new QHBoxLayout;
    layout->addLayout(tools);
    tools->addWidget(planeLabel_);
    auto* group = new QButtonGroup(this);
    const auto button = [this, tools](const QString& text, const char* name) {
        auto* widget = new QPushButton(text, this);
        widget->setObjectName(QString::fromLatin1(name));
        tools->addWidget(widget);
        return widget;
    };
    auto addTool = [this, group, &button](const QString& text, const char* name, Tool tool) {
        auto* action = button(text, name);
        action->setCheckable(true);
        group->addButton(action);
        connect(action, &QPushButton::clicked, this, [this, tool] { canvas_->setTool(tool); });
        return action;
    };
    addTool(tr("Line"), "sketchLine", Tool::Line)->setChecked(true);
    addTool(tr("Rectangle"), "sketchRectangle", Tool::Rectangle);
    addTool(tr("Circle"), "sketchCircle", Tool::Circle);
    auto* selectTool = addTool(tr("Select"), "sketchSelect", Tool::Select);
    connect(button(tr("Undo"), "sketchUndo"), &QPushButton::clicked, this, [this] {
        canvas_->undo();
        showError(tr("Last step undone."));
    });
    tools->addStretch();
    connect(button(tr("Finish"), "sketchFinish"), &QPushButton::clicked, this, &SketchEditor::finishRequested);
    connect(button(tr("Cancel"), "sketchCancel"), &QPushButton::clicked, this, &SketchEditor::cancelRequested);
    layout->addWidget(lineInput_);
    layout->addWidget(canvas_, 1);
    auto* relations = new QHBoxLayout;
    layout->addLayout(relations);
    auto* list = new QListWidget(this);
    list->setObjectName(QStringLiteral("sketchConstraints"));
    list->setAccessibleName(tr("Sketch constraints"));
    list->setMaximumHeight(75);
    relations->addWidget(list, 1);
    auto* controls = new QVBoxLayout;
    relations->addLayout(controls);
    auto* addConstraint = new QToolButton(this);
    addConstraint->setText(tr("Add constraint"));
    addConstraint->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(addConstraint);
    addConstraint->setMenu(menu);
    controls->addWidget(addConstraint);
    QAction* projectionAction = nullptr;
    const std::array kinds{ConstraintKind::Coincident, ConstraintKind::Horizontal, ConstraintKind::Vertical,
                           ConstraintKind::Perpendicular, ConstraintKind::Length, ConstraintKind::Diameter,
                           ConstraintKind::OnProjection};
    const std::array actionNames{"sketchCoincident", "sketchHorizontal", "sketchVertical", "sketchPerpendicular",
                                 "sketchConstraintLength", "sketchConstraintDiameter", "sketchOnProjection"};
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        const auto kind = kinds[i];
        auto* action = menu->addAction(constraintName(kind));
        action->setObjectName(QString::fromLatin1(actionNames[i]));
        if (kind == ConstraintKind::OnProjection) {
            projectionAction = action;
        }
        connect(action, &QAction::triggered, this, [this, kind, selectTool] {
            selectTool->setChecked(true);
            canvas_->startConstraint(kind);
        });
    }
    auto* editConstraint = new QPushButton(tr("Edit dimension…"), this);
    editConstraint->setObjectName(QStringLiteral("editSketchDimension"));
    auto* removeConstraint = new QPushButton(tr("Remove constraint"), this);
    controls->addWidget(editConstraint);
    controls->addWidget(removeConstraint);
    auto* geometryCommands = new QVBoxLayout;
    relations->addLayout(geometryCommands);
    auto* editEntity = new QPushButton(tr("Edit geometry…"), this);
    auto* removeEntity = new QPushButton(tr("Delete geometry"), this);
    geometryCommands->addWidget(editEntity);
    geometryCommands->addWidget(removeEntity);
    geometryCommands->addStretch();
    auto* diagnostic = new QLabel(this);
    diagnostic->setObjectName(QStringLiteral("sketchConstraintDiagnostic"));
    diagnostic->setWordWrap(true);
    layout->addWidget(diagnostic);
    const auto updateSelection = [this, list, editConstraint, removeConstraint] {
        const int row = list->currentRow();
        canvas_->highlightConstraint(row);
        removeConstraint->setEnabled(row >= 0);
        editConstraint->setEnabled(row >= 0 && dimension(canvas_->sketch.constraints[row].kind));
    };
    connect(list, &QListWidget::currentRowChanged, this, updateSelection);
    connect(editConstraint, &QPushButton::clicked, this, [this, list] { canvas_->editConstraint(list->currentRow()); });
    connect(removeConstraint, &QPushButton::clicked, this, [this, list] { canvas_->removeConstraint(list->currentRow()); });
    connect(list, &QListWidget::itemDoubleClicked, this, [this, list] { canvas_->editConstraint(list->currentRow()); });
    connect(editEntity, &QPushButton::clicked, this, [this] { canvas_->editEntity(); });
    connect(removeEntity, &QPushButton::clicked, this, [this] { canvas_->removeEntity(); });
    canvas_->changed = [this, list, updateSelection, diagnostic, editEntity, removeEntity, projectionAction] {
        const int previous = list->currentRow();
        const QSignalBlocker block(list);
        list->clear();
        for (int i = 0; i < static_cast<int>(canvas_->sketch.constraints.size()); ++i) {
            const auto& constraint = canvas_->sketch.constraints[i];
            auto label = tr("%1 · %2").arg(i + 1).arg(constraintName(constraint.kind));
            if (dimension(constraint.kind)) {
                label += tr(" · %1 mm").arg(constraint.value, 0, 'g', 10);
            }
            auto* item = new QListWidgetItem(label, list);
            const auto& solution = canvas_->solution;
            if (std::ranges::find(solution.brokenProjections, i) != solution.brokenProjections.end()) {
                item->setForeground(QColor("#c4473e"));
                item->setToolTip(tr("Broken projected edge reference"));
            } else if (std::ranges::find(solution.conflicts, i) != solution.conflicts.end()) {
                item->setForeground(QColor("#c4473e"));
                item->setToolTip(tr("Conflicting constraint"));
            }
        }
        list->setCurrentRow(std::min(previous, list->count() - 1));
        diagnostic->setText(canvas_->error);
        diagnostic->setVisible(!canvas_->error.isEmpty());
        editEntity->setEnabled(canvas_->hasSelection());
        removeEntity->setEnabled(canvas_->hasSelection());
        projectionAction->setEnabled(canvas_->hasProjections());
        updateSelection();
    };
    canvas_->changed();
    feedback_->setObjectName(QStringLiteral("sketchFeedback"));
    feedback_->setWordWrap(true);
    layout->addWidget(feedback_);
    canvas_->feedback = [this](const QString& message) { feedback_->setText(message); };
}

void SketchEditor::begin(geometry::Plane plane)
{
    begin(geometry::Sketch{plane}, {});
}

void SketchEditor::begin(const geometry::Sketch& sketch, std::vector<geometry::SketchProjection> projections)
{
    canvas_->begin(sketch, std::move(projections));
    const auto tool = sketch.entities.empty() ? QStringLiteral("sketchLine") : QStringLiteral("sketchSelect");
    findChild<QPushButton*>(tool)->click();
    switch (sketch.plane) {
    case geometry::Plane::XY: planeLabel_->setText(tr("XY  (U = X, V = Y)")); break;
    case geometry::Plane::XZ: planeLabel_->setText(tr("XZ  (U = X, V = Z)")); break;
    case geometry::Plane::YZ: planeLabel_->setText(tr("YZ  (U = Y, V = Z)")); break;
    }
    if (sketch.support) {
        planeLabel_->setText(tr("Face of body #%1").arg(sketch.support->body));
    }
    feedback_->setText(tr("Two clicks per shape. Middle drag pans and the wheel zooms."));
    canvas_->setFocus();
}

const geometry::Sketch& SketchEditor::sketch() const
{
    return canvas_->sketch;
}

void SketchEditor::showError(const QString& message)
{
    feedback_->setText(message);
}

}  // namespace graph3d
