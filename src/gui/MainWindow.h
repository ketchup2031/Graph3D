#pragma once

#include "document/Document.h"

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPointer>

#include <functional>
#include <optional>

class QAction;
class QCloseEvent;
class QDockWidget;
class QMenu;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace graph3d {

namespace io { enum class ExchangeFormat; }

class DocumentWindow;
class DraftWindow;
class PropertyPanel;
class SketchEditor;
class StartPage;
class Viewport;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(DocumentWindow* host = nullptr);
    bool initializeScene();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    friend class DocumentWindow;
    bool canClose();
    void createMenus();
    void showStartPage();
    bool showWorkspace();
    bool newProject();
    void showMouseControls();
    void showDraft(const QString& path = {});
    void openProjectPath(const QString& path, bool example = false);
    void updateFileState();
    bool saveProject(bool choosePath);
    bool confirmSave();
    void openProject();
    void importStep();
    void exportModel(io::ExchangeFormat format);
    enum class Machining { Hole, Chamfer, Fillet, SketchFace, ChamferSet, FilletSet, MeasureVertices, MoveFace, RemoveBlend };
    void startMachining(Machining operation);
    void makeHole(int id, const geometry::FaceReference& face);
    void makeEdgeTreatment(int id, const geometry::EdgeReference& edge);
    void endMachining(int id);
    void finishEdgeSet();
    void applyEdgeTreatment(int id, geometry::EdgeTreatmentKind kind, std::vector<geometry::EdgeReference> edges);
    void configureAdvanced(const QString& title, const document::Parameters& defaults, int editing = 0);
    void measureBody();
    void measureBodyDistance();
    void transformCurrent(const QString& title, const geometry::BodyTransform& defaults);
    void configureAssembly(int id);
    void insertLinkedComponent(bool replace);
    void updateLinkedComponents(bool selected);
    void editPartProperties();
    void showAssemblyInterference();
    void showAssemblyStructure();
    void showDrawing();
    void createModelTree();
    void refreshDocument(int selection);
    void editVariables();
    void configureHole(int body, const geometry::FaceReference& face, geometry::HoleSize size, int editing = 0);
    void configureFeaturePattern(int seed);
    void configureEdgeTreatment(document::EdgeTreatment treatment, int editing = 0);
    void updateSelection();
    void updateActions();
    void beginSketch(geometry::Plane plane);
    void beginSketch(const geometry::Sketch& sketch, int id = 0);
    void editSketch(int id);
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

    DocumentWindow* host_;
    document::Document document_;
    Viewport* viewport_;
    QStackedWidget* pages_;
    SketchEditor* sketchEditor_;
    StartPage* startPage_;
    QTreeWidget* tree_;
    PropertyPanel* properties_;
    QHash<int, QTreeWidgetItem*> items_;
    QMenu* createMenu_ = nullptr;
    QMenu* editMenu_ = nullptr;
    QMenu* viewMenu_ = nullptr;
    QMenu* sketchMenu_ = nullptr;
    QDockWidget* modelDock_ = nullptr;
    QDockWidget* propertyDock_ = nullptr;
    QAction* startPageAction_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* newScene_ = nullptr;
    QAction* undo_ = nullptr;
    QAction* redo_ = nullptr;
    QAction* editSketch_ = nullptr;
    QAction* newFaceSketch_ = nullptr;
    QAction* newDatumSketch_ = nullptr;
    int editingSketch_ = 0;
    QAction* extrude_ = nullptr;
    QAction* revolve_ = nullptr;
    QAction* drawing_ = nullptr;
    QAction* createAssembly_ = nullptr;
    QAction* insertLinked_ = nullptr;
    QAction* updateLinked_ = nullptr;
    QAction* updateSelectedLink_ = nullptr;
    QAction* relink_ = nullptr;
    QAction* partProperties_ = nullptr;
    QAction* interference_ = nullptr;
    QAction* assemblyStructure_ = nullptr;
    QList<QAction*> machiningActions_;
    std::optional<Machining> pendingMachining_;
    QAction* finishEdgeSet_ = nullptr;
    QAction* editAdvanced_ = nullptr;
    QAction* variables_ = nullptr;
    QAction* featurePattern_ = nullptr;
    QAction* sweep_ = nullptr;
    QAction* loft_ = nullptr;
    QAction* shell_ = nullptr;
    QAction* draftAngle_ = nullptr;
    QAction* measureBody_ = nullptr;
    QAction* measureDistance_ = nullptr;
    std::vector<geometry::EdgeReference> pickedEdges_;
    std::optional<geometry::Point3> pickedVertex_;
    QMenu* booleanMenu_ = nullptr;
    QMenu* transformMenu_ = nullptr;
    QList<QAction*> fileActions_;
    QString logPath_;
    QString projectPath_;
    std::uint64_t savedRevision_ = 0;
    bool hasWorkspace_ = false;
    QPointer<DraftWindow> draftWindow_;
};

}  // namespace graph3d
