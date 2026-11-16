#pragma once

#include "drawing/Draft.h"

#include <QMainWindow>
#include <QUndoStack>

#include <optional>

class QAction;
class QFileSystemWatcher;
class QLabel;

namespace graph3d {

class DraftCanvas;

class DraftWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit DraftWindow(QWidget* parent);
    bool newDrawing();
    bool openDrawing(const QString& path);

signals:
    void fileUsed(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void browseDrawing();
    bool saveDrawing(bool saveAs);
    void exportPdf();
    void editSheet();
    void editSelected();
    void chooseModel();
    void refreshModel();
    void editModelViews();
    void updateModelStatus(const QString& failure = {});
    void setModelViews(std::optional<drawing::ModelViews> model, const QString& description);
    void removeItem(int index);
    void detachLostPoints(int index, const drawing::DraftItem* replacement);
    void replaceCurve(int index, const std::vector<drawing::DraftItem>& items, const QString& description);
    bool confirmSave();
    void replaceDrawing(drawing::Draft draft, const QString& path = {});
    void updateTitle();
    void editItem(int index, const std::optional<drawing::DraftItem>& after, const QString& description);

    drawing::Draft draft_;
    QUndoStack history_;
    QString path_;
    DraftCanvas* canvas_;
    QFileSystemWatcher* sourceWatch_;
    QLabel* modelStatus_;
    QAction* modelRefresh_;
    QAction* modelSettings_;
    QAction* modelRemove_;
};

}  // namespace graph3d
