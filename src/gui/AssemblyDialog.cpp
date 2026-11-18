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

enum Column { Body, Anchor, OffsetX, OffsetY, OffsetZ, RotateX, RotateY, RotateZ, Fixed, Simplified, ColumnCount };

}  // namespace

AssemblyDialog::AssemblyDialog(const document::Document& document, Sources sources,
                               const document::Assembly* existing, QWidget* parent)
    : QDialog(parent), document_(document), sources_(std::move(sources)), mates_(existing ? existing->mates : std::vector<geometry::Mate>{}),
      table_(new QTableWidget(0, ColumnCount, this)),
      editing_(existing != nullptr)
{
    setWindowTitle(editing_ ? tr("Edit components and mates") : tr("Create assembly"));
    setObjectName(QStringLiteral("assemblyDialog"));
    resize(1120, 470);
    auto* window = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    window->addWidget(tabs, 1);
    auto* placement = new QWidget(tabs);
    tabs->addTab(placement, tr("Components"));
    auto* layout = new QVBoxLayout(placement);
    {
        auto* form = new QFormLayout;
        auto* count = new QSpinBox(this);
        count->setObjectName(QStringLiteral("assemblyCount"));
        count->setRange(geometry::minAssemblyComponents, geometry::maxAssemblyComponents);
        count->setValue(existing ? static_cast<int>(existing->components.size()) : geometry::minAssemblyComponents);
        form->addRow(tr("Components"), count);
        layout->addLayout(form);
        connect(count, &QSpinBox::valueChanged, this, [this](int size) {
            if (size < table_->rowCount()) {
                table_->setRowCount(size);
                contexts_.resize(size);
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
                                       tr("X (°)"), tr("Y (°)"), tr("Z (°)"), tr("Fixed"), tr("Simplified")});
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    auto* header = table_->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Stretch);
    header->setMinimumSectionSize(72);
    header->setSectionResizeMode(Body, QHeaderView::Interactive);
    header->setSectionResizeMode(Anchor, QHeaderView::Interactive);
    table_->setColumnWidth(Body, 185);
    table_->setColumnWidth(Anchor, 150);
    layout->addWidget(table_);
    auto* replaceSelected = new QPushButton(tr("Replace selected instances…"), placement);
    layout->addWidget(replaceSelected);
    connect(replaceSelected, &QPushButton::clicked, this, [this] {
        const auto rows = table_->selectionModel()->selectedRows();
        if (rows.isEmpty()) { return; }
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Replace selected instances"));
        QFormLayout form(&dialog);
        QComboBox source;
        for (const auto& [id, label] : sources_) { source.addItem(label, id); }
        form.addRow(tr("Source body"), &source);
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form.addRow(&buttons);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) { return; }
        for (const auto& row : rows) {
            auto* combo = qobject_cast<QComboBox*>(table_->cellWidget(row.row(), Body));
            if (combo) { combo->setCurrentIndex(combo->findData(source.currentData())); }
        }
    });
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
    auto* edit = new QPushButton(tr("Edit mate…"), mates);
    auto* remove = new QPushButton(tr("Remove mate"), mates);
    auto* solve = new QPushButton(tr("Check mates"), mates);
    add->setObjectName(QStringLiteral("addAssemblyMate"));
    solve->setObjectName(QStringLiteral("checkAssemblyMates"));
    tools->addWidget(add);
    tools->addWidget(edit);
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
    auto* motion = new QWidget(tabs);
    tabs->addTab(motion, tr("Context and motion"));
    auto* motionLayout = new QFormLayout(motion);
    drive_ = new QDoubleSpinBox(motion);
    drive_->setRange(-geometry::maxComponentOffset, geometry::maxComponentOffset);
    drive_->setDecimals(3);
    drive_->setValue(existing ? existing->driveAngle : 0);
    motionLayout->addRow(tr("Requested screw rotation (°)"), drive_);
    auto* context = new QPushButton(tr("Edit component in context…"), motion);
    context->setObjectName(QStringLiteral("editAssemblyContext"));
    motionLayout->addRow(context);
    auto* guide = new QLabel(tr("A screw drive needs a rotatable screw, for example a screw with a coaxial mate to a fixed support. Use the component offsets and angles to set free positions."), motion);
    guide->setWordWrap(true);
    motionLayout->addRow(guide);
    connect(add, &QPushButton::clicked, this, [this] { addMate(); });
    connect(edit, &QPushButton::clicked, this, [this] {
        if (matesList_->currentRow() >= 0) { addMate(matesList_->currentRow()); }
    });
    connect(matesList_, &QListWidget::itemDoubleClicked, this, [this] { addMate(matesList_->currentRow()); });
    connect(solve, &QPushButton::clicked, this, &AssemblyDialog::refreshMates);
    connect(remove, &QPushButton::clicked, this, [this] {
        if (const int row = matesList_->currentRow(); row >= 0) {
            mates_.erase(mates_.begin() + row);
            if (std::ranges::none_of(mates_, [](const auto& mate) { return mate.mode == geometry::MateMode::Screw; })) {
                drive_->setValue(0);
            }
            refreshMates();
        }
    });
    auto* motionResult = new QLabel(motion);
    motionResult->setWordWrap(true);
    motionResult->setTextFormat(Qt::PlainText);
    motionLayout->addRow(motionResult);
    auto* contextList = new QLabel(motion);
    contextList->setWordWrap(true);
    motionLayout->addRow(tr("Contextual relationships"), contextList);
    const auto refreshContext = [this, contextList] {
        QStringList entries;
        for (std::size_t i = 0; i < contexts_.size(); ++i) {
            if (const auto& relation = contexts_[i]) {
                entries << tr("Component %1 → %2 · %3 mm").arg(i + 1).arg(relation->reference).arg(relation->clearance);
            }
        }
        contextList->setText(entries.isEmpty() ? tr("No contextual edits.") : entries.join(u'\n'));
    };
    refreshContext();
    connect(context, &QPushButton::clicked, this, [this, refreshContext] { editContext(); refreshContext(); });
    auto* solveMotion = new QPushButton(tr("Check current position"), motion);
    motionLayout->addRow(solveMotion);
    connect(solveMotion, &QPushButton::clicked, this, [this, motionResult] {
        refreshMates();
        motionResult->setText(diagnosis_->text());
    });
    connect(tabs, &QTabWidget::currentChanged, this, [this, refreshContext](int index) {
        if (index == 1) { refreshMates(); }
        if (index == 2) { refreshContext(); }
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    window->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void AssemblyDialog::appendComponent(const document::Component& component)
{
    const int row = table_->rowCount();
    contexts_.push_back(component.context);
    table_->insertRow(row);
    auto* body = new QComboBox(table_);
    for (const auto& [id, name] : sources_) {
        body->addItem(name, id);
    }
    body->setCurrentIndex(body->findData(component.body));
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
    auto* simple = new QCheckBox(table_);
    simple->setChecked(component.simplified);
    simple->setToolTip(tr("Show only the component envelope in the viewport"));
    table_->setCellWidget(row, Simplified, simple);
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
            qobject_cast<QCheckBox*>(table_->cellWidget(row, Fixed))->isChecked(),
            qobject_cast<QCheckBox*>(table_->cellWidget(row, Simplified))->isChecked(), contexts_[row]});
    }
    result.mates = mates_;
    if (drive_ && std::ranges::any_of(mates_, [](const auto& mate) { return mate.mode == geometry::MateMode::Screw; })) {
        result.driveAngle = drive_->value();
    }
    return result;
}

void AssemblyDialog::refreshMates()
{
    const auto describe = [this](const auto& mate) {
        const auto kind = mate.mode == geometry::MateMode::Distance ? tr("Plane distance")
            : mate.mode == geometry::MateMode::Angle ? tr("Plane angle")
            : mate.mode == geometry::MateMode::Travel ? tr("Travel limits")
            : mate.mode == geometry::MateMode::Screw ? tr("Screw drive")
            : mate.moving.kind == geometry::MateKind::Plane ? tr("Coincident planes") : tr("Coaxial cylinders");
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
        if (std::ranges::any_of(mates_, [](const auto& mate) { return mate.mode == geometry::MateMode::Screw; })
            && solution.error.empty()) {
            lines << tr("Solved rotation: %1° · carriage travel: %2 mm").arg(solution.driveAngle).arg(solution.screwTravel);
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

void AssemblyDialog::addMate(int index)
{
    if (mates_.size() >= 512 && index < 0) {
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
    kind->addItems({tr("Coincident planes"), tr("Coaxial cylinders"), tr("Plane distance"), tr("Plane angle"),
        tr("Travel limits"), tr("Screw drive")});
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
    const auto field = [&](const QString& label, double initial) {
        auto* value = new QDoubleSpinBox(&dialog);
        value->setRange(-geometry::maxComponentOffset, geometry::maxComponentOffset);
        value->setDecimals(4);
        value->setValue(initial);
        form->addRow(label, value);
        return value;
    };
    auto* value = field(tr("Distance (mm), angle (°) or lead (mm/turn)"), 0);
    auto* lower = field(tr("Minimum travel (mm)"), 0);
    auto* upper = field(tr("Maximum travel (mm)"), 20);
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
                const auto expected = kind->currentIndex() == 1 || kind->currentIndex() == 5
                    ? geometry::MateKind::Cylinder : geometry::MateKind::Plane;
                if (surface.kind != expected) {
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
        const int type = kind->currentIndex();
        opposite->setChecked(type == 0 || type == 2);
        opposite->setEnabled(type != 4 && type != 5);
        value->setEnabled(type == 2 || type == 3 || type == 5);
        lower->setEnabled(type == 4 || type == 5);
        upper->setEnabled(type == 4 || type == 5);
        if (type == 3) { value->setValue(90); }
        if (type == 5) { value->setValue(5); }
        refresh();
    });
    value->setEnabled(false);
    lower->setEnabled(false);
    upper->setEnabled(false);
    updateReference();
    if (index >= 0 && index < static_cast<int>(mates_.size())) {
        const auto& old = mates_[index];
        kind->setCurrentIndex(old.mode == geometry::MateMode::Align ? (old.moving.kind == geometry::MateKind::Plane ? 0 : 1)
            : static_cast<int>(old.mode) + 1);
        moving->setCurrentIndex(moving->findData(old.component));
        reference->setCurrentIndex(reference->findData(old.target));
        const auto select = [](QComboBox* field, const auto& list, const auto& prior) {
            const auto found = std::ranges::find(list, prior);
            if (found != list.end()) { field->setCurrentIndex(static_cast<int>(found - list.begin())); }
        };
        select(sourceFace, sourceSurfaces, old.moving);
        select(targetFace, targetSurfaces, old.reference);
        opposite->setChecked(old.opposite);
        value->setValue(old.value);
        lower->setValue(old.minimum);
        upper->setValue(old.maximum);
    }
    if (dialog.exec() == QDialog::Accepted && sourceFace->currentIndex() >= 0 && targetFace->currentIndex() >= 0) {
        const int type = kind->currentIndex();
        auto mate = geometry::Mate{moving->currentData().toInt(), reference->currentData().toInt(),
                          sourceSurfaces[sourceFace->currentIndex()], targetSurfaces[targetFace->currentIndex()],
                          opposite->isChecked(), type <= 1 ? geometry::MateMode::Align : static_cast<geometry::MateMode>(type - 1),
                          type == 2 || type == 3 || type == 5 ? value->value() : 0,
                          type >= 4 ? lower->value() : 0, type >= 4 ? upper->value() : 0};
        if (index < 0) { mates_.push_back(mate); }
        else { mates_[index] = mate; }
        refreshMates();
    }
}

void AssemblyDialog::editContext()
{
    const auto components = assembly().components;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Edit component in context"));
    dialog.setObjectName(QStringLiteral("assemblyContextDialog"));
    auto* form = new QFormLayout(&dialog);
    auto* moving = new QComboBox(&dialog);
    auto* reference = new QComboBox(&dialog);
    auto* face = new QComboBox(&dialog);
    auto* target = new QComboBox(&dialog);
    auto* offset = new QDoubleSpinBox(&dialog);
    offset->setRange(-geometry::maxComponentOffset, geometry::maxComponentOffset);
    offset->setDecimals(4);
    for (int i = 2; i <= static_cast<int>(components.size()); ++i) {
        moving->addItem(tr("Component %1").arg(i), i);
    }
    form->addRow(tr("Moving component"), moving);
    form->addRow(tr("Moving surface"), face);
    form->addRow(tr("Reference component"), reference);
    form->addRow(tr("Reference surface"), target);
    form->addRow(tr("Clearance along the moving normal (mm)"), offset);
    auto* note = new QLabel(tr("The selected planar face follows the parallel reference plane in the assembled position."), &dialog);
    note->setWordWrap(true);
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto* remove = buttons->addButton(tr("Remove contextual edit"), QDialogButtonBox::ResetRole);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(remove, &QPushButton::clicked, &dialog, [&] { dialog.done(2); });
    std::vector<geometry::FaceReference> local, support;
    const auto fill = [&](QComboBox* field, int index, auto& faces, const std::optional<geometry::FaceReference>& saved) {
        field->clear();
        faces.clear();
        if (index < 1) { return; }
        try {
            if (const auto* object = document_.find(components.at(index - 1).body); object && !object->shape.IsNull()) {
                faces = geometry::planarFaces(object->shape);
            }
        } catch (const std::exception&) {
            // A context edit needs one solid in each instance.
        } catch (const Standard_Failure&) {}
        for (const auto& candidate : faces) {
            const auto xyz = [](const auto& point) {
                return QStringLiteral("(%1, %2, %3)").arg(point[0], 0, 'g', 6)
                    .arg(point[1], 0, 'g', 6).arg(point[2], 0, 'g', 6);
            };
            field->addItem(tr("Plane %1 · point %2 · direction %3").arg(field->count() + 1)
                .arg(xyz(candidate.center), xyz(candidate.outward)));
        }
        if (saved) {
            const auto found = std::ranges::find(faces, *saved);
            field->setCurrentIndex(found == faces.end() ? -1 : static_cast<int>(found - faces.begin()));
        }
    };
    const auto updateEnabled = [&] {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(face->currentIndex() >= 0 && target->currentIndex() >= 0);
    };
    const auto refreshTarget = [&] {
        const auto& prior = contexts_[moving->currentData().toInt() - 1];
        const auto index = reference->currentData().toInt();
        fill(target, index, support, prior && prior->reference == index ? std::optional(prior->targetFace) : std::nullopt);
        updateEnabled();
    };
    const auto refreshMoving = [&] {
        const int index = moving->currentData().toInt();
        const auto& prior = contexts_[index - 1];
        const QSignalBlocker block(reference);
        reference->clear();
        for (int i = 1; i < index; ++i) { reference->addItem(tr("Component %1").arg(i), i); }
        if (prior) { reference->setCurrentIndex(reference->findData(prior->reference)); }
        fill(face, index, local, prior ? std::optional(prior->face) : std::nullopt);
        offset->setValue(prior ? prior->clearance : 0);
        remove->setEnabled(prior.has_value());
        refreshTarget();
    };
    connect(moving, &QComboBox::currentIndexChanged, &dialog, refreshMoving);
    connect(reference, &QComboBox::currentIndexChanged, &dialog, refreshTarget);
    connect(face, &QComboBox::currentIndexChanged, &dialog, updateEnabled);
    connect(target, &QComboBox::currentIndexChanged, &dialog, updateEnabled);
    refreshMoving();
    const auto action = dialog.exec();
    if (action == 2) { contexts_[moving->currentData().toInt() - 1].reset(); }
    if (action == QDialog::Accepted && face->currentIndex() >= 0 && target->currentIndex() >= 0) {
        contexts_[moving->currentData().toInt() - 1] = geometry::ContextFaceEdit{reference->currentData().toInt(),
            local.at(face->currentIndex()), support.at(target->currentIndex()), offset->value()};
    }
}

}  // namespace graph3d
