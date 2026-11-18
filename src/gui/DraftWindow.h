#pragma once

#include "drawing/Draft.h"

#include <QMainWindow>
#include <QUndoStack>

#include <functional>
#include <optional>

class QAction;
class QFileSystemWatcher;
class QLabel;
class QTabBar;
class QComboBox;

namespace graph3d {

class DocumentWindow;
class DraftCanvas;

class DraftWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit DraftWindow(QWidget* parent, DocumentWindow* host = nullptr);
    ~DraftWindow() override;
    bool newDrawing();
    bool openDrawing(const QString& path);

signals:
    void fileUsed(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    friend class DocumentWindow;
    friend class MainWindow;
    void browseDrawing();
    void browseDxf();
    bool importDxf(const QString& path);
    void exportDxf();
    void exportCad(bool allSheets, bool dwg);
    bool saveDrawing(bool saveAs);
    void exportPdf();
    void editSheet();
    void editTitleBlock();
    void saveDrawingTemplate();
    void newFromDrawingTemplate();
    void saveFragment();
    void insertFragment();
    void editTable(int index = -1);
    void editLayers();
    void updateLayers();
    void changeSheetCount(bool add);
    void activateSheet(int index);
    void updateSheetTabs();
    drawing::Draft wholeDrawing() const;
    void pushSheetEdit(const QString& name, std::function<void()> redo, std::function<void()> undo);
    void editSelected();
    void reattachDimension();
    void prepareModelDimensions();
    void chooseModel();
    void refreshModel();
    void editModelViews();
    void manageDrawingViews();
    void editDetail();
    void placeSectionLine();
    void placeLocalSection();
    void updateModelStatus(const QString& failure = {});
    void showSpecification();
    void createSpecification();
    void linkSpecification();
    void placePosition();
    void removePosition();
    void setSpecificationExport(bool enabled);
    void setModelViews(std::optional<drawing::ModelViews> model, const QString& description);
    void removeItem(int index);
    void detachLostPoints(int index, const drawing::DraftItem* replacement);
    void replaceCurve(int index, const std::vector<drawing::DraftItem>& items, const QString& description);
    bool confirmSave();
    void replaceDrawing(drawing::Draft draft, const QString& path = {});
    void updateTitle();
    void editItem(int index, const std::optional<drawing::DraftItem>& after, const QString& description);

    DocumentWindow* host_;
    drawing::Draft draft_;
    std::vector<drawing::Draft> sheets_{1};
    int activeSheet_ = 0;
    QTabBar* sheetTabs_;
    QComboBox* layers_;
    QUndoStack history_;
    QString path_;
    DraftCanvas* canvas_;
    QFileSystemWatcher* sourceWatch_;
    QLabel* modelStatus_;
    QAction* modelRefresh_;
    QAction* modelSettings_;
    QAction* modelRemove_;
    QAction* modelDetail_;
    QAction* sectionLine_;
    QAction* localRegion_;
    QAction* specification_;
    QAction* createSpecification_;
    QAction* linkSpecification_;
    QAction* unlinkSpecification_;
    QAction* placePosition_;
    QAction* removePosition_;
    QAction* specificationExport_;
};

}  // namespace graph3d
