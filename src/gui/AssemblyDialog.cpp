#include "gui/AssemblyDialog.h"
#include "gui/InterfaceLanguage.h"

#include <Standard_Failure.hxx>
#include <QCheckBox>
#include <QColor>
#include <QSignalBlocker>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QHBoxLayout>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSpinBox>
#include <QTableWidget>
#include <QScrollArea>
#include <algorithm>
#include <QVBoxLayout>

namespace graph3d {
namespace {

enum Column { Body, Anchor, OffsetX, OffsetY, OffsetZ, RotateX, RotateY, RotateZ, Fixed, ColumnCount };

}  // namespace

AssemblyDialog::AssemblyDialog(const document::Document& document, Sources sources,
                               const document::Assembly* existing, QWidget* parent)
    : QDialog(parent), document_(document), sources_(std::move(sources)), mates_(existing ? existing->mates : std::vector<geometry::Mate>{}),
      table_(new QTableWidget(0, ColumnCount, this)),
      editing_(existing != nullptr)
{
    setWindowTitle(editing_ ? tr("Edit component placement") : tr("Create assembly"));
    setObjectName(QStringLiteral("assemblyDialog"));
    resize(1050, 420);
    auto* window = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    window->addWidget(tabs, 1);
    auto* placement = new QWidget(tabs);
    tabs->addTab(placement, tr("Components"));
    auto* layout = new QVBoxLayout(placement);
    if (!editing_) {
        auto* form = new QFormLayout;
        auto* count = new QSpinBox(this);
        count->setObjectName(QStringLiteral("assemblyCount"));
        count->setRange(geometry::minAssemblyComponents, geometry::maxAssemblyComponents);
        form->addRow(tr("Components"), count);
        layout->addLayout(form);
        connect(count, &QSpinBox::valueChanged, this, [this](int size) {
            if (size < table_->rowCount()) {
                table_->setRowCount(size);
                std::erase_if(mates_, [size](const auto& mate) { return mate.component > size; });
            }
            while (table_->rowCount() < size) {
                appendComponent({sources_.front().first, {}});
            }
        });
    }
    auto* help = new QLabel(tr("Choose the assembly origin or an earlier component as the frame for each placement."), this);
    help->setWordWrap(true);
    layout->addWidget(help);
    table_->setObjectName(QStringLiteral("assemblyComponents"));
    table_->setHorizontalHeaderLabels({tr("Source body"), tr("Relative to"), tr("X (mm)"), tr("Y (mm)"), tr("Z (mm)"),
                                       tr("X (°)"), tr("Y (°)"), tr("Z (°)"), tr("Fixed")});
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    auto* header = table_->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Stretch);
    header->setMinimumSectionSize(72);
    header->setSectionResizeMode(Body, QHeaderView::Interactive);
    header->setSectionResizeMode(Anchor, QHeaderView::Interactive);
    table_->setColumnWidth(Body, 185);
    table_->setColumnWidth(Anchor, 150);
    layout->addWidget(table_);
    if (editing_) {
        for (const auto& component : existing->components) {
            appendComponent(component);
        }
    } else {
        appendComponent({sources_.front().first, {}});
        const bool repeated = sources_.size() == 1;
        appendComponent({repeated ? sources_.front().first : sources_.at(1).first,
                         {0, {repeated ? 120.0 : 0.0, 0, 0}, {}}});
    }
    auto* mates = new QWidget(tabs);
    tabs->addTab(mates, tr("Mates"));
    auto* mateLayout = new QVBoxLayout(mates);
    matesList_ = new QListWidget(mates);
    matesList_->setObjectName(QStringLiteral("assemblyMates"));
    mateLayout->addWidget(matesList_);
    auto* tools = new QHBoxLayout;
    mateLayout->addLayout(tools);
    auto* add = new QPushButton(tr("Add mate…"), mates);
    auto* remove = new QPushButton(tr("Remove mate"), mates);
    auto* solve = new QPushButton(tr("Check mates"), mates);
    add->setObjectName(QStringLiteral("addAssemblyMate"));
    solve->setObjectName(QStringLiteral("checkAssemblyMates"));
    tools->addWidget(add);
    tools->addWidget(remove);
    tools->addWidget(solve);
    diagnosis_ = new QLabel(mates);
    diagnosis_->setObjectName(QStringLiteral("assemblyMateStatus"));
    diagnosis_->setWordWrap(true);
    diagnosis_->setTextFormat(Qt::PlainText);
    auto* scroll = new QScrollArea(mates);
    scroll->setWidgetResizable(true);
    scroll->setWidget(diagnosis_);
    mateLayout->addWidget(scroll);
    connect(add, &QPushButton::clicked, this, &AssemblyDialog::addMate);
    connect(solve, &QPushButton::clicked, this, &AssemblyDialog::refreshMates);
    connect(remove, &QPushButton::clicked, this, [this] {
        if (const int row = matesList_->currentRow(); row >= 0) {
            mates_.erase(mates_.begin() + row);
            refreshMates();
        }
    });
    connect(tabs, &QTabWidget::currentChanged, this, [this](int index) { if (index == 1) refreshMates(); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    window->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void AssemblyDialog::appendComponent(const document::Component& component)
{
    const int row = table_->rowCount();
    table_->insertRow(row);
    auto* body = new QComboBox(table_);
    for (const auto& [id, name] : sources_) {
        body->addItem(name, id);
    }
    body->setCurrentIndex(body->findData(component.body));
    body->setEnabled(!editing_);
    connect(body, &QComboBox::currentIndexChanged, this, [this, row] {
        std::erase_if(mates_, [row](const auto& mate) { return mate.component == row + 1 || mate.target == row + 1; });
    });
    table_->setCellWidget(row, Body, body);
    auto* anchor = new QComboBox(table_);
    anchor->addItem(tr("Assembly origin"));
    for (int i = 1; i <= row; ++i) {
        anchor->addItem(tr("Component %1").arg(i));
    }
    anchor->setCurrentIndex(component.placement.anchor >= 0 && component.placement.anchor <= row
                            ? component.placement.anchor : 0);
    table_->setCellWidget(row, Anchor, anchor);
    auto* fixed = new QCheckBox(table_);
    fixed->setChecked(component.fixed);
    table_->setCellWidget(row, Fixed, fixed);
    for (int axis = 0; axis < 3; ++axis) {
        auto* offset = new QDoubleSpinBox(table_);
        offset->setDecimals(4);
        offset->setRange(-geometry::maxComponentOffset, geometry::maxComponentOffset);
        offset->setValue(component.placement.offset[axis]);
        table_->setCellWidget(row, OffsetX + axis, offset);
        auto* angle = new QDoubleSpinBox(table_);
        angle->setDecimals(3);
        angle->setRange(-geometry::maxComponentAngle, geometry::maxComponentAngle);
        angle->setValue(component.placement.angles[axis]);
        table_->setCellWidget(row, RotateX + axis, angle);
    }
}

document::Assembly AssemblyDialog::assembly() const
{
    document::Assembly result;
    result.components.reserve(table_->rowCount());
    for (int row = 0; row < table_->rowCount(); ++row) {
        const auto* body = qobject_cast<QComboBox*>(table_->cellWidget(row, Body));
        const auto* anchor = qobject_cast<QComboBox*>(table_->cellWidget(row, Anchor));
        geometry::Placement placement;
        placement.anchor = anchor->currentIndex();
        for (int axis = 0; axis < 3; ++axis) {
            placement.offset[axis] = qobject_cast<QDoubleSpinBox*>(table_->cellWidget(row, OffsetX + axis))->value();
            placement.angles[axis] = qobject_cast<QDoubleSpinBox*>(table_->cellWidget(row, RotateX + axis))->value();
        }
        result.components.push_back({body->currentData().toInt(), placement,
            qobject_cast<QCheckBox*>(table_->cellWidget(row, Fixed))->isChecked()});
    }
    result.mates = mates_;
    return result;
}

void AssemblyDialog::refreshMates()
{
    const auto describe = [this](const auto& mate) {
        const auto kind = mate.moving.kind == geometry::MateKind::Plane ? tr("Coincident planes") : tr("Coaxial cylinders");
        return tr("%1 · Component %2 → %3").arg(kind).arg(mate.component).arg(mate.target);
    };
    matesList_->clear();
    for (const auto& mate : mates_) {
        matesList_->addItem(describe(mate));
    }
    QStringList lines;
    try {
        const auto solution = document_.assemblySolution(assembly());
        if (!solution.error.empty()) {
            const auto text = localizedDiagnostic(solution.error.c_str());
            if (solution.failedMate >= 0) {
                matesList_->item(solution.failedMate)->setForeground(QColor("#b93232"));
                lines << tr("Mate %1: %2").arg(solution.failedMate + 1).arg(text);
            } else {
                lines << text;
            }
        }
        for (int i = 0; i < static_cast<int>(solution.components.size()); ++i) {
            const auto& pose = solution.components[i];
            lines << tr("Component %1: %2 translation, %3 rotation degrees of freedom")
                .arg(i + 1).arg(pose.translations).arg(pose.rotations);
        }
    } catch (const std::exception& failure) {
        lines << localizedDiagnostic(failure.what());
    } catch (const Standard_Failure& failure) {
        lines << localizedDiagnostic(failure.GetMessageString());
    }
    diagnosis_->setText(lines.join(u'\n'));
}

void AssemblyDialog::addMate()
{
    if (mates_.size() >= 512) {
        diagnosis_->setText(tr("An assembly can contain at most 512 mates."));
        return;
    }
    const auto components = assembly().components;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("assemblyMateDialog"));
    dialog.setWindowTitle(tr("Assembly mate"));
    auto* form = new QFormLayout(&dialog);
    auto* kind = new QComboBox(&dialog);
    auto* moving = new QComboBox(&dialog);
    auto* reference = new QComboBox(&dialog);
    auto* sourceFace = new QComboBox(&dialog);
    auto* targetFace = new QComboBox(&dialog);
    auto* opposite = new QCheckBox(tr("Opposite directions"), &dialog);
    kind->setObjectName(QStringLiteral("mateKind"));
    moving->setObjectName(QStringLiteral("mateMoving"));
    reference->setObjectName(QStringLiteral("mateReference"));
    sourceFace->setObjectName(QStringLiteral("mateMovingSurface"));
    targetFace->setObjectName(QStringLiteral("mateReferenceSurface"));
    kind->addItems({tr("Coincident planes"), tr("Coaxial cylinders")});
    opposite->setChecked(true);
    for (int i = 2; i <= static_cast<int>(components.size()); ++i) {
        moving->addItem(tr("Component %1").arg(i), i);
    }
    form->addRow(tr("Type"), kind);
    form->addRow(tr("Moving component"), moving);
    form->addRow(tr("Moving surface"), sourceFace);
    form->addRow(tr("Reference component"), reference);
    form->addRow(tr("Reference surface"), targetFace);
    form->addRow(opposite);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const auto xyz = [](const auto& p) {
        return QStringLiteral("(%1, %2, %3)").arg(p[0], 0, 'g', 6).arg(p[1], 0, 'g', 6).arg(p[2], 0, 'g', 6);
    };
    std::vector<geometry::MateSurface> sourceSurfaces, targetSurfaces;
    const auto refresh = [&] {
        const auto fill = [&](QComboBox& list, int row, auto& surfaces) {
            list.clear();
            surfaces.clear();
            if (row <= 0) {
                return;
            }
            const auto* object = document_.find(components[row - 1].body);
            if (!object || object->status != document::Status::Ready) {
                return;
            }
            for (const auto& surface : geometry::assemblySurfaces(object->shape)) {
                if (surface.kind != static_cast<geometry::MateKind>(kind->currentIndex())) {
                    continue;
                }
                surfaces.push_back(surface);
                const auto prefix = surface.kind == geometry::MateKind::Plane
                    ? tr("Plane %1").arg(surfaces.size())
                    : tr("Cylinder %1 · Ø%2").arg(surfaces.size()).arg(surface.radius * 2, 0, 'g', 6);
                list.addItem(tr("%1 · point %2 · direction %3").arg(prefix, xyz(surface.point), xyz(surface.direction)));
            }
        };
        fill(*sourceFace, moving->currentData().toInt(), sourceSurfaces);
        fill(*targetFace, reference->currentData().toInt(), targetSurfaces);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!sourceSurfaces.empty() && !targetSurfaces.empty());
    };
    const auto updateReference = [&] {
        const QSignalBlocker blocker(reference);
        reference->clear();
        for (int i = 1; i < moving->currentData().toInt(); ++i) {
            reference->addItem(tr("Component %1").arg(i), i);
        }
        refresh();
    };
    connect(moving, &QComboBox::currentIndexChanged, &dialog, updateReference);
    connect(reference, &QComboBox::currentIndexChanged, &dialog, refresh);
    connect(kind, &QComboBox::currentIndexChanged, &dialog, [&] {
        opposite->setChecked(kind->currentIndex() == 0);
        refresh();
    });
    updateReference();
    if (dialog.exec() == QDialog::Accepted) {
        mates_.push_back({moving->currentData().toInt(), reference->currentData().toInt(),
                          sourceSurfaces[sourceFace->currentIndex()], targetSurfaces[targetFace->currentIndex()],
                          opposite->isChecked()});
        refreshMates();
    }
}

}  // namespace graph3d
