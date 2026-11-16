#pragma once

#include "geometry/Sketch.h"

#include <QHash>
#include <QMainWindow>

class QAction;
class QMenu;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace graph3d {

class SketchEditor;
class Viewport;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    bool initializeScene();

private:
    void createMenus();
    void createModelTree();
    void updateActions();
    void beginSketch(geometry::Plane plane);
    void finishSketch();
    void leaveSketch();
    void selectTreeObject(int id);
    void extrudeCurrent();
    void revolveCurrent();
    void combineCurrent(geometry::BooleanOperation operation);
    void writeLog(const QString& message);
    void showSceneError(const QString& message);

    Viewport* viewport_;
    QStackedWidget* pages_;
    SketchEditor* sketchEditor_;
    QTreeWidget* tree_;
    QHash<int, QTreeWidgetItem*> items_;
    QMenu* createMenu_ = nullptr;
    QMenu* editMenu_ = nullptr;
    QMenu* viewMenu_ = nullptr;
    QMenu* sketchMenu_ = nullptr;
    QAction* newScene_ = nullptr;
    QAction* extrude_ = nullptr;
    QAction* revolve_ = nullptr;
    QMenu* booleanMenu_ = nullptr;
    QString logPath_;
};

}  // namespace graph3d
