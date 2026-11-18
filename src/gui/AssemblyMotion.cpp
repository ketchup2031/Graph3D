#include "gui/MainWindow.h"
#include "gui/InterfaceLanguage.h"
#include "viewport/Viewport.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace graph3d {
void MainWindow::moveAssembly()
{
    const int id = selectedId();
    const auto* object = document_.find(id);
    if (!object || object->status != document::Status::Ready) { return; }
    const auto* original = std::get_if<document::Assembly>(&object->parameters);
    if (!original) { return; }
    const bool drive = std::ranges::any_of(original->mates, [](const auto& mate) { return mate.mode == geometry::MateMode::Screw; });
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Move and check assembly"));
    dialog.setObjectName(QStringLiteral("assemblyMotionDialog"));
    dialog.resize(540, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* tabs = new QTabWidget(&dialog);
    layout->addWidget(tabs);
    auto* pose = new QWidget(tabs);
    auto* movement = new QFormLayout(pose);
    tabs->addTab(pose, tr("Position"));
    auto* driver = new QDoubleSpinBox(pose);
    driver->setObjectName(QStringLiteral("assemblyDriveAngle"));
    driver->setDecimals(3);
    driver->setRange(-geometry::maxComponentOffset, geometry::maxComponentOffset);
    driver->setValue(original->driveAngle);
    if (drive) { movement->addRow(tr("Requested screw rotation (°)"), driver); }
    else { driver->hide(); }
    auto* components = new QComboBox(pose);
    components->addItem(tr("Select a component to move"), 0);
    for (std::size_t i = 0; i < original->components.size(); ++i) {
        if (!original->components[i].fixed) { components->addItem(tr("Component %1").arg(i + 1), static_cast<int>(i + 1)); }
    }
    movement->addRow(tr("Component"), components);
    const auto driveMate = std::ranges::find(original->mates, geometry::MateMode::Screw, &geometry::Mate::mode);
    std::array<QDoubleSpinBox*, 3> delta{}, rotation{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        delta[axis] = new QDoubleSpinBox(pose);
        rotation[axis] = new QDoubleSpinBox(pose);
        delta[axis]->setDecimals(4);
        rotation[axis]->setDecimals(3);
        delta[axis]->setRange(-geometry::maxComponentOffset, geometry::maxComponentOffset);
        rotation[axis]->setRange(-720, 720);
        movement->addRow(tr("Translate %1 (mm)").arg(QChar("XYZ"[axis])), delta[axis]);
        movement->addRow(tr("Rotate %1 (°)").arg(QChar("XYZ"[axis])), rotation[axis]);
    }
    const auto enableFields = [&] {
        const auto selected = components->currentData().toInt();
        const bool screw = drive && driveMate->target == selected;
        for (std::size_t i = 0; i < delta.size(); ++i) {
            delta[i]->setEnabled(selected > 0);
            rotation[i]->setEnabled(selected > 0 && !screw);
        }
    };
    connect(components, &QComboBox::currentIndexChanged, &dialog, enableFields);
    enableFields();
    auto* description = new QLabel(tr("Translations and turns are relative to the placement frame. Mates constrain the result."), pose);
    description->setWordWrap(true);
    movement->addRow(description);
    auto* preview = new QPushButton(tr("Preview position"), pose);
    preview->setObjectName(QStringLiteral("previewAssemblyPosition"));
    movement->addRow(preview);
    auto* position = new QLabel(pose);
    position->setObjectName(QStringLiteral("assemblyMotionPosition"));
    position->setTextFormat(Qt::PlainText);
    position->setWordWrap(true);
    movement->addRow(position);

    const auto current = [&] {
        auto candidate = *original;
        candidate.driveAngle = driver->value();
        if (const int index = components->currentData().toInt(); index > 0) {
            auto& placement = candidate.components[index - 1].placement;
            for (std::size_t i = 0; i < delta.size(); ++i) {
                placement.offset[i] += delta[i]->value();
                if (!drive || driveMate->target != index) {
                    placement.angles[i] = std::remainder(placement.angles[i] + rotation[i]->value(), 360.);
                }
            }
        }
        return candidate;
    };
    const auto showResult = [&](const auto& solved) {
        QStringList lines;
        if (drive) { lines << tr("Solved rotation: %1° · carriage travel: %2 mm").arg(solved.driveAngle).arg(solved.screwTravel); }
        if (const int index = components->currentData().toInt(); index > 0) {
            const auto& comp = solved.components[index - 1];
            const auto xyz = comp.transform.TranslationPart();
            lines << tr("Solved offset: (%1, %2, %3) mm").arg(xyz.X(), 0, 'g', 7).arg(xyz.Y(), 0, 'g', 7).arg(xyz.Z(), 0, 'g', 7);
            lines << tr("Component %1: %2 translation, %3 rotation degrees of freedom")
                .arg(index).arg(comp.translations).arg(comp.rotations);
        }
        position->setText(lines.isEmpty() ? tr("Position is valid.") : lines.join(u'\n'));
    };
    bool previewed = false;
    auto refreshPreview = [&] {
        try {
            const auto candidate = current();
            const auto solved = document_.assemblySolution(candidate);
            if (!solved.error.empty()) { throw std::invalid_argument(solved.error); }
            auto scratch = document_;
            scratch.clearHistory();
            scratch.editAssembly(id, candidate);
            if (const auto* updated = scratch.find(id); updated->status != document::Status::Ready) {
                throw std::invalid_argument(updated->error);
            }
            viewport_->syncDocument(scratch);
            viewport_->selectObject(id);
            previewed = true;
            showResult(solved);
            return true;
        } catch (const std::exception& error) {
            position->setText(localizedDiagnostic(error.what()));
        } catch (const Standard_Failure& error) {
            position->setText(localizedDiagnostic(error.GetMessageString()));
        }
        return false;
    };
    connect(preview, &QPushButton::clicked, &dialog, [&] { refreshPreview(); });
    if (drive) {
        auto* scan = new QWidget(tabs);
        tabs->addTab(scan, tr("Interference along motion"));
        auto* form = new QFormLayout(scan);
        const auto angle = [&](const QString& title, double value) {
            auto* spin = new QDoubleSpinBox(scan);
            spin->setRange(-geometry::maxComponentOffset, geometry::maxComponentOffset);
            spin->setValue(value);
            form->addRow(title, spin);
            return spin;
        };
        auto* start = angle(tr("First rotation (°)"), 0);
        auto* end = angle(tr("Last rotation (°)"), 360);
        start->setObjectName(QStringLiteral("assemblyScanStart"));
        end->setObjectName(QStringLiteral("assemblyScanEnd"));
        auto* count = new QSpinBox(scan);
        count->setObjectName(QStringLiteral("assemblyScanPositions"));
        count->setRange(2, 50);
        count->setValue(5);
        form->addRow(tr("Positions to check"), count);
        auto* run = new QPushButton(tr("Check sampled positions"), scan);
        run->setObjectName(QStringLiteral("runAssemblyMotionScan"));
        form->addRow(run);
        auto* output = new QPlainTextEdit(scan);
        output->setObjectName(QStringLiteral("assemblyMotionScan"));
        output->setReadOnly(true);
        form->addRow(output);
        connect(run, &QPushButton::clicked, &dialog, [=, this] {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            try {
                const auto report = document_.scanAssembly(current(), start->value(), end->value(), count->value());
                QStringList lines;
                const auto path = [](const auto& indices) {
                    QStringList pieces;
                    for (int index : indices) { pieces << QString::number(index); }
                    return pieces.join(u'.');
                };
                for (const auto& record : report) {
                    lines << tr("%1° requested → %2° solved, %3 mm: %4 overlapping pairs")
                        .arg(record.requestedAngle).arg(record.solvedAngle).arg(record.travel).arg(record.pairs.size());
                    for (std::size_t i = 0; i < std::min<std::size_t>(record.pairs.size(), 10); ++i) {
                        const auto& pair = record.pairs[i];
                        lines << tr("Components %1 and %2: %3 mm³")
                            .arg(path(pair.first), path(pair.second)).arg(pair.volumeMm3, 0, 'g', 8);
                    }
                }
                output->setPlainText(lines.join(u'\n'));
            } catch (const std::exception& error) {
                output->setPlainText(localizedDiagnostic(error.what()));
            } catch (const Standard_Failure& error) {
                output->setPlainText(localizedDiagnostic(error.GetMessageString()));
            }
            QApplication::restoreOverrideCursor();
        });
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (refreshPreview()) { dialog.accept(); }
        else { tabs->setCurrentWidget(pose); }
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        editFeature(id, [&] { return document_.editAssembly(id, current()); });
    }
    if (previewed) {
        viewport_->syncDocument(document_);
        viewport_->selectObject(id);
    }
}
}  // namespace graph3d
