#pragma once

#include "document/Document.h"

#include <QDialog>
#include <QList>

#include <utility>

class QTableWidget;

namespace graph3d {

class AssemblyDialog final : public QDialog {
public:
    using Sources = QList<std::pair<document::ObjectId, QString>>;

    AssemblyDialog(Sources sources, const document::Assembly* existing, QWidget* parent = nullptr);
    document::Assembly assembly() const;

private:
    void appendComponent(const document::Component& component);

    Sources sources_;
    QTableWidget* table_;
    bool editing_;
};

}  // namespace graph3d
