#pragma once

#include "drawing/SpecificationDocument.h"
#include <QMainWindow>
#include <QUndoStack>

class QLabel;
class QLineEdit;
class QTableWidget;

namespace graph3d {
class DocumentWindow;
class SpecificationWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit SpecificationWindow(DocumentWindow* host);
    bool open(const QString& path);
    bool confirmSave();
    bool save(bool choosePath = false);
    void setDocument(drawing::SpecificationDocument spec);
private:
    friend class DocumentWindow;
    void loadDocument(drawing::SpecificationDocument spec);
    void replace(drawing::SpecificationDocument spec);
    void render();
    void updateTitle();
    void chooseAssembly();
    void chooseDrawing();
    void refresh();
    void audit();
    void exportFile(bool pdf);
    void addManual();
    void removeRow();
    void editCell(int row, int column);
    void editTitle();
    void issue(const QString& title, const std::exception& error);
    DocumentWindow* host_;
    drawing::SpecificationDocument spec_;
    QString path_;
    QUndoStack undo_;
    QLineEdit* title_;
    QLineEdit* designation_;
    QLabel* links_;
    QTableWidget* table_;
};
}  // namespace graph3d
