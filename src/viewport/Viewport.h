#pragma once

#include "geometry/Primitive.h"

#include <QWidget>

#include <functional>
#include <memory>

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
    void clearSelection();
    void fitScene();
    void setDirection(ViewDirection direction);
    void setGridVisible(bool visible);
    void setAxesVisible(bool visible);

signals:
    void sceneMessage(const QString& message);
    void operationFailed(const QString& message);

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

    struct Scene;
    std::unique_ptr<Scene> scene_;
};

}  // namespace graph3d
