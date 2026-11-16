#pragma once

#include "document/Document.h"

#include <QHash>
#include <QMainWindow>

#include <functional>

class QAction;
class QMenu;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace graph3d {

class PropertyPanel;
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
    void refreshDocument(int selection);
    void updateSelection();
    void updateActions();
    void beginSketch(geometry::Plane plane);
    void finishSketch();
    void leaveSketch();
    int addFeature(const QString& operation, const std::function<int()>& action, bool select = true);
    void editFeature(int id, const std::function<bool()>& action);
    void restoreHistory(bool undo);
    int selectedId() const;
    QString objectName(const document::Object& object) const;
    QString objectDetails(const document::Object& object) const;
    void extrudeCurrent();
    void revolveCurrent();
    void combineCurrent(geometry::BooleanOperation operation);
    void report(const QString& message);
    void writeLog(const QString& message);
    void showSceneError(const QString& message);

    document::Document document_;
    Viewport* viewport_;
    QStackedWidget* pages_;
    SketchEditor* sketchEditor_;
    QTreeWidget* tree_;
    PropertyPanel* properties_;
    QHash<int, QTreeWidgetItem*> items_;
    QMenu* createMenu_ = nullptr;
    QMenu* editMenu_ = nullptr;
    QMenu* viewMenu_ = nullptr;
    QMenu* sketchMenu_ = nullptr;
    QAction* newScene_ = nullptr;
    QAction* undo_ = nullptr;
    QAction* redo_ = nullptr;
    QAction* extrude_ = nullptr;
    QAction* revolve_ = nullptr;
    QMenu* booleanMenu_ = nullptr;
    QString logPath_;
};

}  // namespace graph3d
