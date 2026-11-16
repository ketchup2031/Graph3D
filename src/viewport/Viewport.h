#pragma once

#include "geometry/Primitive.h"
#include "geometry/Sketch.h"

#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

class TopoDS_Shape;

namespace graph3d {

enum class ViewDirection { Front, Top, Right, Isometric };

class Viewport final : public QWidget {
    Q_OBJECT

public:
    explicit Viewport(QWidget* parent = nullptr);
    ~Viewport() override;

    bool initialize();
    void newScene();
    void addPrimitive(geometry::Primitive primitive);
    int addSketch(const geometry::Sketch& sketch);
    int extrudeSketch(int id, double distance);
    int revolveSketch(int id, geometry::Axis axis, double offset, double angle);
    int combineBodies(int first, int second, geometry::BooleanOperation operation);
    void selectObject(int id);
    bool setObjectVisible(int id, bool visible);
    void clearSelection();
    void fitScene();
    void setDirection(ViewDirection direction);
    void setGridVisible(bool visible);
    void setAxesVisible(bool visible);

signals:
    void sceneMessage(const QString& message);
    void operationFailed(const QString& message);
    void sceneCleared();
    void objectAdded(int id, const QString& name, bool sketch, const QString& detail, int parentId);
    void objectSelected(int id);
    void objectVisibilityChanged(int id, bool visible);

protected:
    QPaintEngine* paintEngine() const override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    bool runOperation(const QString& operation, const std::function<void()>& action);
    QPoint pixels(const QPointF& logicalPosition) const;
    int addObject(const TopoDS_Shape& shape, const QString& baseName,
                  std::optional<geometry::Plane> plane = {}, const QString& detail = {}, int parentId = 0);

    struct Scene;
    std::unique_ptr<Scene> scene_;
};

}  // namespace graph3d
