#pragma once

#include "geometry/PartFeatures.h"

#include <QImage>
#include <QWidget>

#include <functional>
#include <memory>

namespace graph3d::document { class Document; }

namespace graph3d {

enum class ViewDirection { Front, Top, Right, Isometric };
enum class SubshapePick { Face, Edge, SketchFace, EdgeSet, Vertex };

class Viewport final : public QWidget {
    Q_OBJECT

public:
    explicit Viewport(QWidget* parent = nullptr);
    ~Viewport() override;

    bool initialize();
    void syncDocument(const document::Document& document);
    QImage renderSnapshot();
    void resetCamera();
    void selectObject(int id);
    bool beginSubshapePick(int id, SubshapePick mode);
    void cancelSubshapePick();
    void clearSelection();
    void fitScene();
    void setDirection(ViewDirection direction);
    void setGridVisible(bool visible);
    void setAxesVisible(bool visible);

signals:
    void operationFailed(const QString& message);
    void objectSelected(int id);
    void facePicked(int id, const geometry::FaceReference& face);
    void edgePicked(int id, const geometry::EdgeReference& edge);
    void vertexPicked(int id, const geometry::Point3& point);
    void edgeSetFinished();
    void subshapePickCancelled();

protected:
    QPaintEngine* paintEngine() const override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    bool runOperation(const QString& operation, const std::function<void()>& action);
    void endSubshapePick();
    void pickSubshape(const QPoint& point);
    QPoint pixels(const QPointF& logicalPosition) const;

    struct Scene;
    std::unique_ptr<Scene> scene_;
};

}  // namespace graph3d
