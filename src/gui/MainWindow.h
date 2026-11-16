#pragma once

#include "document/Document.h"

#include <QHash>
#include <QList>
#include <QMainWindow>

#include <functional>
#include <optional>

class QAction;
class QCloseEvent;
class QMenu;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace graph3d {

namespace io { enum class ExchangeFormat; }

class PropertyPanel;
class SketchEditor;
class Viewport;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    bool initializeScene();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void createMenus();
    void updateFileState();
    bool saveProject(bool choosePath);
    bool confirmSave();
    void openProject();
    void importStep();
    void exportModel(io::ExchangeFormat format);
    enum class Machining { Hole, Chamfer, Fillet };
    void startMachining(Machining operation);
    void makeHole(int id, const geometry::FaceReference& face);
    void makeEdgeTreatment(int id, const geometry::EdgeReference& edge);
    void endMachining(int id);
    void transformCurrent(const QString& title, const geometry::BodyTransform& defaults);
    void configureAssembly(int id);
    void showDrawing();
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
    QAction* drawing_ = nullptr;
    QAction* createAssembly_ = nullptr;
    QList<QAction*> machiningActions_;
    std::optional<Machining> pendingMachining_;
    QMenu* booleanMenu_ = nullptr;
    QMenu* transformMenu_ = nullptr;
    QList<QAction*> fileActions_;
    QString logPath_;
    QString projectPath_;
    std::uint64_t savedRevision_ = 0;
};

}  // namespace graph3d
