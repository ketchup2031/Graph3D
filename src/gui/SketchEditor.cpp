#include "gui/SketchEditor.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <type_traits>

namespace graph3d {
namespace {

enum class Tool { Line, Rectangle, Circle };
constexpr double snap = 5.0;

}  // namespace

class SketchEditor::Canvas final : public QWidget {
public:
    explicit Canvas(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("sketchCanvas"));
        setAccessibleName(SketchEditor::tr("Sketch drawing area"));
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(320, 240);
    }

    void begin(geometry::Plane plane)
    {
        sketch = {plane, {}};
        pending.reset();
        cursor.reset();
        pan = {};
        pixelsPerMillimetre = 3.0;
        update();
    }

    void setTool(Tool tool)
    {
        activeTool = tool;
        pending.reset();
        update();
    }

    void undo()
    {
        if (pending) {
            pending.reset();
        } else if (!sketch.entities.empty()) {
            sketch.entities.pop_back();
        }
        update();
    }

    geometry::Sketch sketch;
    std::function<void(const QString&)> feedback;

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
        painter.setPen(QPen(QColor(90, 209, 250), 2));
        for (const auto& entity : sketch.entities) {
            draw(painter, entity);
        }
        if (pending && cursor) {
            painter.setPen(QPen(QColor(230, 240, 250), 1, Qt::DashLine));
            draw(painter, makeEntity(*pending, *cursor));
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
            pending.reset();
            update();
            return;
        }
        if (event->button() != Qt::LeftButton) {
            return;
        }
        cursor = toSnappedModel(event->position());
        if (!pending) {
            pending = cursor;
            feedback(SketchEditor::tr("Choose the second point."));
        } else {
            const double du = std::abs(pending->u - cursor->u);
            const double dv = std::abs(pending->v - cursor->v);
            if ((du == 0 && dv == 0) || (activeTool == Tool::Rectangle && (du == 0 || dv == 0))) {
                feedback(SketchEditor::tr("Choose a distinct point with a nonzero size."));
            } else {
                sketch.entities.push_back(makeEntity(*pending, *cursor));
                pending.reset();
                reportPosition();
            }
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
        reportPosition();
        update();
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
    QPointF screenOrigin() const
    {
        return QPointF(width() / 2.0, height() / 2.0) + pan;
    }

    geometry::Point2 toSnappedModel(QPointF p) const
    {
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
            feedback(SketchEditor::tr("U: %1 mm   V: %2 mm   |   Snap: 5 mm")
                .arg(cursor->u, 0, 'f', 0).arg(cursor->v, 0, 'f', 0));
        }
    }

    Tool activeTool = Tool::Line;
    std::optional<geometry::Point2> pending;
    std::optional<geometry::Point2> cursor;
    QPointF previous;
    QPointF pan;
    double pixelsPerMillimetre = 3.0;
};

SketchEditor::SketchEditor(QWidget* parent)
    : QWidget(parent), canvas_(new Canvas(this)), planeLabel_(new QLabel(this)), feedback_(new QLabel(this))
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
    connect(button(tr("Undo"), "sketchUndo"), &QPushButton::clicked, this, [this] {
        canvas_->undo();
        showError(tr("Last step undone."));
    });
    tools->addStretch();
    connect(button(tr("Finish"), "sketchFinish"), &QPushButton::clicked, this, &SketchEditor::finishRequested);
    connect(button(tr("Cancel"), "sketchCancel"), &QPushButton::clicked, this, &SketchEditor::cancelRequested);
    layout->addWidget(canvas_, 1);
    feedback_->setObjectName(QStringLiteral("sketchFeedback"));
    feedback_->setWordWrap(true);
    layout->addWidget(feedback_);
    canvas_->feedback = [this](const QString& message) { feedback_->setText(message); };
}

void SketchEditor::begin(geometry::Plane plane)
{
    canvas_->begin(plane);
    findChild<QPushButton*>(QStringLiteral("sketchLine"))->click();
    switch (plane) {
    case geometry::Plane::XY: planeLabel_->setText(tr("XY  (U = X, V = Y)")); break;
    case geometry::Plane::XZ: planeLabel_->setText(tr("XZ  (U = X, V = Z)")); break;
    case geometry::Plane::YZ: planeLabel_->setText(tr("YZ  (U = Y, V = Z)")); break;
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
