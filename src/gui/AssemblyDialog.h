#pragma once

#include "document/Document.h"

#include <QDialog>
#include <QList>

#include <utility>

class QTableWidget;
class QListWidget;
class QLabel;
class QDoubleSpinBox;

namespace graph3d {

class AssemblyDialog final : public QDialog {
public:
    using Sources = QList<std::pair<document::ObjectId, QString>>;

    AssemblyDialog(const document::Document& document, Sources sources, const document::Assembly* existing,
                   QWidget* parent = nullptr);
    document::Assembly assembly() const;

private:
    void appendComponent(const document::Component& component);

    void addMate(int index = -1);
    void refreshMates();
    void editContext();
    const document::Document& document_;
    Sources sources_;
    QListWidget* matesList_;
    QLabel* diagnosis_;
    std::vector<geometry::Mate> mates_;
    QTableWidget* table_;
    QDoubleSpinBox* drive_ = nullptr;
    std::vector<std::optional<geometry::ContextFaceEdit>> contexts_;
    bool editing_;
};

}  // namespace graph3d
