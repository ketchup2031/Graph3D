#include "gui/DraftWindow.h"
#include "gui/InterfaceLanguage.h"
#include "io/ModelViewsIO.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace graph3d {
namespace {

using namespace drawing;

QDoubleSpinBox* decimal(QFormLayout& form, const QString& label, const char* id, double value,
                        double min = -10000, double max = 10000)
{
    auto* field = new QDoubleSpinBox(form.parentWidget());
    field->setObjectName(QString::fromLatin1(id));
    field->setDecimals(3);
    field->setRange(min, max);
    field->setValue(value);
    form.addRow(label, field);
    return field;
}

QString shownName(const DrawingView& view)
{
    return view.name.isEmpty() ? drawingViewName(view.kind) : view.name;
}

bool usableAsSource(DrawingViewKind kind)
{
    return kind <= DrawingViewKind::Arrow;
}

std::optional<DrawingView> editView(QWidget* parent, DrawingView value, const ModelViews& model, int index)
{
    const bool base = index >= 0 && index < 3;
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("drawingViewDialog"));
    dialog.setWindowTitle(DraftWindow::tr("Drawing view"));
    auto* form = new QFormLayout(&dialog);
    auto* kind = new QComboBox(&dialog);
    kind->setObjectName(QStringLiteral("drawingViewKind"));
    for (int i = 0; i <= static_cast<int>(DrawingViewKind::Local); ++i) {
        kind->addItem(drawingViewName(static_cast<DrawingViewKind>(i)));
    }
    kind->setCurrentIndex(static_cast<int>(value.kind));
    kind->setEnabled(!base);
    form->addRow(DraftWindow::tr("Type"), kind);

    auto* name = new QLineEdit(value.name, &dialog);
    name->setMaxLength(80);
    name->setObjectName(QStringLiteral("drawingViewLabel"));
    form->addRow(DraftWindow::tr("View label"), name);

    auto* source = new QComboBox(&dialog);
    source->setObjectName(QStringLiteral("drawingViewSource"));
    for (int i = 0; i < 3; ++i) {
        source->addItem(drawingViewName(static_cast<DrawingViewKind>(i)), baseModelView(model, i).id);
    }
    for (int i = 0; i < static_cast<int>(model.additional.size()) && (index < 0 || i < index - 3); ++i) {
        const auto& entry = model.additional[i];
        if (usableAsSource(entry.kind)) {
            source->addItem(shownName(entry), entry.id);
        }
    }
    if (const auto selected = source->findData(value.source); selected >= 0) {
        source->setCurrentIndex(selected);
    }
    form->addRow(DraftWindow::tr("Source view"), source);

    const auto& place = value.placement;
    auto* x = decimal(*form, DraftWindow::tr("Centre X on sheet"), "drawingViewX", place.center.x());
    auto* y = decimal(*form, DraftWindow::tr("Centre Y on sheet"), "drawingViewY", place.center.y());
    auto* scale = decimal(*form, DraftWindow::tr("Scale factor"), "drawingViewScale", place.scale, .001, 1000);
    auto* yaw = decimal(*form, DraftWindow::tr("Yaw (°)"), "drawingViewYaw", value.angles[0], -360, 360);
    auto* pitch = decimal(*form, DraftWindow::tr("Pitch (°)"), "drawingViewPitch", value.angles[1], -90, 90);
    auto* roll = decimal(*form, DraftWindow::tr("Roll (°)"), "drawingViewRoll", value.angles[2], -360, 360);
    auto* path = new QPlainTextEdit(&dialog);
    path->setObjectName(QStringLiteral("drawingViewPath"));
    path->setMaximumHeight(115);
    path->setPlaceholderText(DraftWindow::tr("Enter X Y per line, in model mm from the source view centre."));
    QStringList lines;
    for (const auto point : value.path) {
        lines.append(QStringLiteral("%1 %2").arg(point.x(), 0, 'g', 12).arg(point.y(), 0, 'g', 12));
    }
    path->setPlainText(lines.join(u'\n'));
    form->addRow(DraftWindow::tr("Points or direction"), path);
    auto* focusX = decimal(*form, DraftWindow::tr("Region centre X in source"), "drawingViewFocusX", value.focus.x());
    auto* focusY = decimal(*form, DraftWindow::tr("Region centre Y in source"), "drawingViewFocusY", value.focus.y());
    auto* radius = decimal(*form, DraftWindow::tr("Region radius in model mm"), "drawingViewRadius", value.radius, .001);
    auto* depth = decimal(*form, DraftWindow::tr("Cut offset from model centre"), "drawingViewDepth", value.depth);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    const auto fields = [=, &dialog] {
        const auto type = static_cast<DrawingViewKind>(kind->currentIndex());
        const bool local = type == DrawingViewKind::Local;
        const bool region = type == DrawingViewKind::Detail || local;
        form->setRowVisible(name, !base);
        form->setRowVisible(source, derivedView(type));
        for (auto* field : {x, y, scale}) {
            form->setRowVisible(field, !local);
        }
        for (auto* field : {yaw, pitch, roll}) {
            form->setRowVisible(field, type == DrawingViewKind::Oblique);
        }
        form->setRowVisible(path, pathView(type));
        for (auto* field : {focusX, focusY, radius}) {
            form->setRowVisible(field, region);
        }
        form->setRowVisible(depth, local);
        dialog.adjustSize();
    };
    QObject::connect(kind, &QComboBox::currentIndexChanged, &dialog, fields);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        value.kind = static_cast<DrawingViewKind>(kind->currentIndex());
        value.name = name->text().trimmed();
        value.placement = {{x->value(), y->value()}, scale->value()};
        value.source = derivedView(value.kind) ? source->currentData().toUuid() : QUuid{};
        value.angles = {yaw->value(), pitch->value(), roll->value()};
        value.focus = {focusX->value(), focusY->value()};
        value.radius = radius->value();
        value.depth = depth->value();
        value.path.clear();
        if (pathView(value.kind)) {
            bool valid = true;
            const auto entries = path->toPlainText().split(u'\n', Qt::SkipEmptyParts);
            for (const auto& line : entries) {
                static const QRegularExpression whitespace(QStringLiteral("\\s+"));
                const auto pair = line.trimmed().split(whitespace, Qt::SkipEmptyParts);
                bool okX = false, okY = false;
                const double px = pair.value(0).toDouble(&okX);
                const double py = pair.value(1).toDouble(&okY);
                valid &= pair.size() == 2 && okX && okY && std::isfinite(px) && std::isfinite(py)
                    && std::abs(px) <= 10000 && std::abs(py) <= 10000;
                value.path.emplace_back(px, py);
            }
            const auto count = value.path.size();
            valid &= value.kind == DrawingViewKind::SteppedSection ? count >= 4 && count <= 16
                : value.kind == DrawingViewKind::AlignedSection ? count >= 3 && count <= 16 : count == 2;
            if (!valid) {
                QMessageBox::warning(&dialog, DraftWindow::tr("Drawing view"),
                    DraftWindow::tr("Enter two points for a direction, at least three for an aligned section or at least four for a stepped section."));
                return;
            }
        }
        dialog.accept();
    });
    fields();
    return dialog.exec() == QDialog::Accepted ? std::optional(std::move(value)) : std::nullopt;
}

}  // namespace

void DraftWindow::manageDrawingViews()
{
    if (!draft_.modelViews || !draft_.modelViews->geometry) {
        QMessageBox::information(this, tr("Drawing views"), tr("Insert a model before adding views."));
        return;
    }
    auto updated = *draft_.modelViews;
    bool modified = false;
    bool rebuild = false;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("draftManageViews"));
    dialog.setWindowTitle(tr("Drawing views"));
    dialog.resize(480, 350);
    auto* layout = new QVBoxLayout(&dialog);
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("drawingViewsList"));
    layout->addWidget(list);
    auto* controls = new QHBoxLayout;
    layout->addLayout(controls);
    auto* add = new QPushButton(tr("Add view…"), &dialog);
    auto* edit = new QPushButton(tr("Edit view…"), &dialog);
    auto* remove = new QPushButton(tr("Remove view"), &dialog);
    add->setObjectName(QStringLiteral("addDrawingView"));
    edit->setObjectName(QStringLiteral("editDrawingView"));
    remove->setObjectName(QStringLiteral("removeDrawingView"));
    controls->addWidget(add);
    controls->addWidget(edit);
    controls->addWidget(remove);

    const auto populate = [&](int row) {
        const QSignalBlocker block(list);
        list->clear();
        for (int i = 0; i < 3; ++i) {
            auto* item = new QListWidgetItem(drawingViewName(static_cast<DrawingViewKind>(i)), list);
            item->setCheckState(updated.showBase[i] ? Qt::Checked : Qt::Unchecked);
        }
        for (const auto& view : updated.additional) {
            list->addItem(shownName(view));
        }
        list->setCurrentRow(std::clamp(row, 0, list->count() - 1));
    };
    populate(0);
    connect(list, &QListWidget::itemChanged, &dialog, [&](QListWidgetItem* item) {
        const auto index = list->row(item);
        if (index >= 0 && index < 3) {
            updated.showBase[index] = item->checkState() == Qt::Checked;
            modified = true;
        }
    });

    connect(add, &QPushButton::clicked, &dialog, [&] {
        if (updated.additional.size() >= drawingViewLimit) {
            QMessageBox::warning(&dialog, tr("Drawing views"), tr("This sheet has reached the drawing view limit."));
            return;
        }
        if (drawing::baseModelView(updated, 0).id.isNull()) {
            try {
                const auto source = io::openModelSource(updated.sourcePath);
                updated = io::buildModelViews(source, updated.bodyId, drawing::sectionSettings(updated),
                                              drawing::draftSize(draft_), &updated);
            } catch (const std::exception& error) {
                QMessageBox::warning(&dialog, tr("Drawing views"), localizedDiagnostic(error.what()));
                return;
            }
        }
        DrawingView value;
        value.id = QUuid::createUuid();
        value.kind = DrawingViewKind::Isometric;
        value.placement = {QPointF(drawing::draftSize(draft_).width() / 2, 85), updated.layout[0].scale};
        if (auto result = editView(&dialog, value, updated, -1)) {
            updated.additional.push_back(std::move(*result));
            rebuild = modified = true;
            populate(list->count());
        }
    });
    connect(edit, &QPushButton::clicked, &dialog, [&] {
        const int row = list->currentRow();
        if (row < 0) {
            return;
        }
        DrawingView value;
        if (row < 3) {
            value.kind = static_cast<DrawingViewKind>(row);
            value.placement = updated.layout[row];
        } else {
            value = updated.additional[row - 3];
        }
        if (auto result = editView(&dialog, value, updated, row)) {
            if (row < 3) {
                updated.layout[row] = result->placement;
                updated.showBase[row] = true;
            } else {
                rebuild |= value.kind != result->kind || value.angles != result->angles
                    || value.path != result->path || value.source != result->source || value.depth != result->depth;
                updated.additional[row - 3] = std::move(*result);
            }
            modified = true;
            populate(row);
        }
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        const int row = list->currentRow();
        if (row < 0) {
            return;
        }
        if (row < 3) {
            updated.showBase[row] = false;
        } else {
            QList<QUuid> removed{updated.additional[row - 3].id};
            for (const auto& view : updated.additional) {
                if (removed.contains(view.source)) {
                    removed.append(view.id);
                }
            }
            if (removed.size() > 1 && QMessageBox::question(&dialog, tr("Remove view"),
                    tr("Remove this view and its dependent views?")) != QMessageBox::Yes) {
                return;
            }
            std::erase_if(updated.additional, [&](const auto& view) { return removed.contains(view.id); });
        }
        modified = true;
        populate(row);
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (rebuild) {
            try {
                const auto source = io::openModelSource(updated.sourcePath);
                updated = io::buildModelViews(source, updated.bodyId, drawing::sectionSettings(updated),
                                              drawing::draftSize(draft_), &updated);
            } catch (const std::exception& error) {
                QMessageBox::warning(&dialog, tr("Drawing views"), localizedDiagnostic(error.what()));
                return;
            }
        }
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted && modified) {
        setModelViews(std::move(updated), tr("Arrange drawing views"));
    }
}

}  // namespace graph3d
