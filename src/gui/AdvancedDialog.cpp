#include "gui/AdvancedDialog.h"

#include "gui/InterfaceLanguage.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace graph3d {
namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("AdvancedDialog", text);
}

QString xyz(const geometry::Point3& coordinates)
{
    return tr("(%1, %2, %3)").arg(coordinates[0], 0, 'g', 7)
        .arg(coordinates[1], 0, 'g', 7).arg(coordinates[2], 0, 'g', 7);
}

QString faceLabel(const geometry::FaceReference& reference)
{
    return tr("Point %1 · normal %2").arg(xyz(reference.center), xyz(reference.outward));
}

using Read = std::function<document::Parameters()>;

class OperationDialog final : public QDialog {
public:
    OperationDialog(QWidget* parent, const QString& title) : QDialog(parent), form(new QFormLayout(this))
    {
        setObjectName(QStringLiteral("advancedFeatureDialog"));
        setWindowTitle(title);
        resize(560, 360);
    }

    QDoubleSpinBox* number(const QString& label, double value, double min, double max)
    {
        auto* spin = new QDoubleSpinBox(this);
        spin->setDecimals(6);
        spin->setRange(min, max);
        spin->setValue(value);
        form->addRow(label, spin);
        return spin;
    }

    auto triple(const QString& label, const geometry::Point3& value)
    {
        std::array<QDoubleSpinBox*, 3> spins{};
        for (int i = 0; i < 3; ++i) {
            spins[i] = number(QStringLiteral("%1 %2").arg(label, QString(QLatin1Char("XYZ"[i]))),
                              value[i], -1.e8, 1.e8);
        }
        return [spins] { return geometry::Point3{spins[0]->value(), spins[1]->value(), spins[2]->value()}; };
    }

    std::optional<document::Parameters> run(const Read& read)
    {
        QLabel diagnostic;
        diagnostic.setWordWrap(true);
        form->addRow(&diagnostic);
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(&buttons);
        std::optional<document::Parameters> result;
        connect(&buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(&buttons, &QDialogButtonBox::accepted, this, [&] {
            try {
                result = read();
                accept();
            } catch (const std::exception& error) {
                diagnostic.setText(localizedDiagnostic(error.what()));
            }
        });
        exec();
        return result;
    }

    QFormLayout* form;
};

QListWidgetItem* checkable(QListWidget* list, const QString& text, bool checked)
{
    auto* item = new QListWidgetItem(text, list);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    return item;
}

Read sweepEditor(OperationDialog& dialog, const document::Document& document, const document::Sweep& sweep, int editing)
{
    auto* source = new QComboBox(&dialog);
    source->setObjectName(QStringLiteral("sweepPathSketch"));
    source->addItem(tr("Enter path points"), 0);
    for (const auto& object : document.objects()) {
        const auto* sketch = std::get_if<geometry::Sketch>(&object.parameters);
        if (sketch && sketch->trajectory && (object.status == document::Status::Ready || object.id == sweep.pathSketch)
            && (!editing || object.id < editing)) {
            source->addItem(tr("Path sketch #%1").arg(object.id), object.id);
        }
    }
    source->setCurrentIndex(std::max(0, source->findData(sweep.pathSketch)));
    source->setEnabled(source->count() > 1);
    dialog.form->addRow(tr("Path source"), source);
    auto* reverse = new QCheckBox(tr("Reverse path direction"), &dialog);
    reverse->setObjectName(QStringLiteral("sweepReversePath"));
    reverse->setChecked(sweep.reversePath);
    dialog.form->addRow(reverse);
    auto* edit = new QPlainTextEdit(&dialog);
    edit->setObjectName(QStringLiteral("sweepPath"));
    QStringList rows;
    for (const auto& p : sweep.path) {
        rows << QStringLiteral("%1 %2 %3").arg(p[0], 0, 'g', 14).arg(p[1], 0, 'g', 14).arg(p[2], 0, 'g', 14);
    }
    edit->setPlainText(rows.join(u'\n'));
    auto* hint = new QLabel(tr("Enter U V N in millimetres, one point per line. Start with N = 0 and keep U and V fixed for the first segment."));
    hint->setWordWrap(true);
    dialog.form->addRow(hint);
    dialog.form->addRow(tr("Path points"), edit);
    const auto updateFields = [form = dialog.form, source, edit, hint, reverse] {
        const bool linked = source->currentData().toInt() != 0;
        form->setRowVisible(edit, !linked);
        form->setRowVisible(hint, !linked);
        form->setRowVisible(reverse, linked);
    };
    QObject::connect(source, &QComboBox::currentIndexChanged, &dialog, updateFields);
    updateFields();
    return [source, reverse, edit, profile = sweep.profile]() -> document::Parameters {
        if (const int pathId = source->currentData().toInt()) {
            return document::Sweep{profile, {}, pathId, reverse->isChecked()};
        }
        const auto lines = edit->toPlainText().split(u'\n', Qt::SkipEmptyParts);
        const QRegularExpression spaces(QStringLiteral("\\s+"));
        std::vector<geometry::Point3> path;
        for (const auto& line : lines) {
            const auto fields = line.trimmed().split(spaces, Qt::SkipEmptyParts);
            if (fields.isEmpty()) {
                continue;
            }
            if (fields.size() != 3 || path.size() == 128) {
                throw std::invalid_argument("Enter three coordinates per line and at most 128 points.");
            }
            geometry::Point3 p{};
            for (int i = 0; i < 3; ++i) {
                bool ok = false;
                p[i] = fields[i].toDouble(&ok);
                if (!ok) {
                    p[i] = QLocale().toDouble(fields[i], &ok);
                }
                if (!ok || !std::isfinite(p[i]) || std::abs(p[i]) > 1.e8) {
                    throw std::invalid_argument("Enter finite construction coordinates.");
                }
            }
            path.push_back(p);
        }
        if (path.size() < 2) {
            throw std::invalid_argument("Enter at least two path points.");
        }
        return document::Sweep{profile, std::move(path)};
    };
}

Read loftEditor(OperationDialog& dialog, const document::Document& document, const document::Loft& loft, bool editing)
{
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("loftSections"));
    list->setDragDropMode(QAbstractItemView::InternalMove);
    const auto add = [&](int id, bool checked) {
        auto* item = checkable(list, tr("Sketch #%1").arg(id), checked);
        item->setData(Qt::UserRole, id);
        if (editing) {
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        }
    };
    for (const auto id : loft.profiles) {
        add(id, true);
    }
    if (!editing) {
        for (const auto& object : document.objects()) {
            const auto* sketch = std::get_if<geometry::Sketch>(&object.parameters);
            if (object.status == document::Status::Ready && sketch && !sketch->trajectory
                && std::ranges::find(loft.profiles, object.id) == loft.profiles.end()) {
                add(object.id, false);
            }
        }
    }
    dialog.form->addRow(tr("Check sections and drag them into order."), list);
    auto* ruled = new QCheckBox(tr("Straight transitions"), &dialog);
    ruled->setChecked(loft.ruled);
    dialog.form->addRow(ruled);
    return [list, ruled]() -> document::Parameters {
        document::Loft operation{{}, ruled->isChecked()};
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->checkState() == Qt::Checked) {
                operation.profiles.push_back(list->item(i)->data(Qt::UserRole).toInt());
            }
        }
        if (operation.profiles.size() < 2 || operation.profiles.size() > 32) {
            throw std::invalid_argument("Select two to 32 ordered loft sections.");
        }
        return operation;
    };
}

Read facesEditor(OperationDialog& dialog, const TopoDS_Shape& body, int id,
                 const std::vector<geometry::FaceReference>& selected, std::optional<geometry::FaceReference> neutral,
                 double value)
{
    auto references = geometry::planarFaces(body);
    const auto include = [&](const geometry::FaceReference& face) {
        if (std::ranges::find(references, face) == references.end()) {
            references.push_back(face);
        }
    };
    for (const auto& face : selected) {
        include(face);
    }
    if (neutral && neutral->outward != geometry::Point3{}) {
        include(*neutral);
    }
    QComboBox* base = nullptr;
    if (neutral) {
        base = new QComboBox(&dialog);
        base->setObjectName(QStringLiteral("draftNeutralFace"));
        base->addItem(tr("Choose a face"));
        dialog.form->addRow(tr("Neutral face"), base);
    }
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("operationFaces"));
    for (std::size_t i = 0; i < references.size(); ++i) {
        const auto& face = references[i];
        const auto label = tr("Face %1 · %2").arg(i + 1).arg(faceLabel(face));
        checkable(list, label, std::ranges::find(selected, face) != selected.end());
        if (base) {
            base->addItem(label);
            if (*neutral == face) {
                base->setCurrentIndex(base->count() - 1);
            }
        }
    }
    dialog.form->addRow(neutral ? tr("Faces to draft") : tr("Faces to open"), list);
    auto* number = dialog.number(neutral ? tr("Draft angle (°)") : tr("Wall thickness (mm)"),
                                 value, neutral ? -88.999 : -1.e6, neutral ? 88.999 : 1.e6);
    number->setObjectName(QStringLiteral("advancedValue"));
    if (!neutral) {
        auto* hint = new QLabel(tr("Positive thickness offsets the walls inward. Negative thickness offsets them outward."));
        hint->setWordWrap(true);
        dialog.form->addRow(hint);
    }
    return [id, list, base, references = std::move(references), number]() -> document::Parameters {
        std::vector<geometry::FaceReference> faces;
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->checkState() == Qt::Checked) {
                faces.push_back(references[i]);
            }
        }
        if (faces.empty() || faces.size() > 64) {
            throw std::invalid_argument("Select between one and 64 planar faces.");
        }
        if (base) {
            if (base->currentIndex() < 1) {
                throw std::invalid_argument("Select a neutral face.");
            }
            return document::DraftAngle{id, references[base->currentIndex() - 1], std::move(faces), number->value()};
        }
        return document::Shell{id, std::move(faces), number->value()};
    };
}

}  // namespace

std::optional<document::Parameters> advancedParameters(QWidget* parent, const QString& title,
    const document::Document& document, const document::Parameters& initial, int editing)
{
    OperationDialog dialog(parent, title);
    if (const auto* p = std::get_if<geometry::DatumPlane>(&initial)) {
        const auto origin = dialog.triple(tr("Origin (mm)"), p->origin);
        const auto normal = dialog.triple(tr("Normal"), p->normal);
        return dialog.run([=]() -> document::Parameters { return geometry::DatumPlane{origin(), normal()}; });
    }
    if (const auto* p = std::get_if<geometry::DatumAxis>(&initial)) {
        const auto origin = dialog.triple(tr("Origin (mm)"), p->origin);
        const auto direction = dialog.triple(tr("Direction"), p->direction);
        return dialog.run([=]() -> document::Parameters { return geometry::DatumAxis{origin(), direction()}; });
    }
    if (const auto* p = std::get_if<document::Sweep>(&initial)) {
        return dialog.run(sweepEditor(dialog, document, *p, editing));
    }
    if (const auto* p = std::get_if<document::Loft>(&initial)) {
        return dialog.run(loftEditor(dialog, document, *p, editing));
    }
    if (const auto* p = std::get_if<document::Shell>(&initial)) {
        return dialog.run(facesEditor(dialog, document.find(p->body)->shape, p->body, p->openings, {}, p->thickness));
    }
    if (const auto* p = std::get_if<document::FeaturePattern>(&initial)) {
        auto* source = new QComboBox(&dialog);
        source->addItem(tr("Linear step"), 0);
        for (const auto& object : document.objects()) {
            const auto* sketch = std::get_if<geometry::Sketch>(&object.parameters);
            if (sketch && sketch->trajectory && (!editing || object.id < editing)) {
                source->addItem(tr("Path sketch #%1").arg(object.id), object.id);
            }
        }
        source->setCurrentIndex(std::max(0, source->findData(p->path)));
        dialog.form->addRow(tr("Path source"), source);
        auto* count = dialog.number(tr("Instances (including seed)"), p->count, 2, geometry::maxArrayInstances);
        count->setDecimals(0);
        const auto step = dialog.triple(tr("Step (mm)"), p->step);
        auto* reverse = new QCheckBox(tr("Reverse path direction"), &dialog);
        reverse->setChecked(p->reverse);
        dialog.form->addRow(reverse);
        return dialog.run([=]() -> document::Parameters {
            const int path = source->currentData().toInt();
            return document::FeaturePattern{p->seed, static_cast<int>(count->value()),
                path ? geometry::Point3{} : step(), path, path && reverse->isChecked()};
        });
    }
    if (const auto* p = std::get_if<document::MoveFace>(&initial)) {
        auto* distance = dialog.number(tr("Outward displacement (mm)"), p->distance, -1.e6, 1.e6);
        return dialog.run([=]() -> document::Parameters { return document::MoveFace{p->body, p->face, distance->value()}; });
    }
    if (const auto* p = std::get_if<document::RemoveBlend>(&initial)) {
        const auto faces = geometry::blendFaces(document.find(p->body)->shape);
        auto* choice = new QComboBox(&dialog);
        for (const auto& face : faces) {
            choice->addItem(tr("Face near %1").arg(xyz(face.center)));
        }
        if (const auto prior = std::ranges::find(faces, p->face); prior != faces.end()) {
            choice->setCurrentIndex(static_cast<int>(prior - faces.begin()));
        }
        dialog.form->addRow(tr("Blend face"), choice);
        return dialog.run([&]() -> document::Parameters {
            if (choice->currentIndex() < 0) { throw std::invalid_argument("Select a curved blend face."); }
            return document::RemoveBlend{p->body, faces.at(choice->currentIndex())};
        });
    }
    const auto& p = std::get<document::DraftAngle>(initial);
    return dialog.run(facesEditor(dialog, document.find(p.body)->shape, p.body, p.walls, p.neutral, p.degrees));
}

std::optional<double> bodyPropertiesDialog(QWidget* parent, const document::Document& document, const document::Object& object)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("bodyMeasuresDialog"));
    dialog.setWindowTitle(tr("Body measurements"));
    QFormLayout form(&dialog);
    QDoubleSpinBox density;
    density.setObjectName(QStringLiteral("materialDensity"));
    density.setDecimals(3);
    density.setRange(0, 1.e9);
    density.setSpecialValueText(tr("Unspecified"));
    density.setValue(object.densityKgM3.value_or(0));
    QLabel volume, geometricCenter, mass, center, principal, products;
    const auto original = density.value();
    const auto initial = document.measure(object.id);
    volume.setText(QString::number(initial.geometric.volumeMm3, 'g', 10));
    geometricCenter.setText(xyz(initial.geometric.centerMm));
    const auto update = [&] {
        const auto result = density.value() == original ? initial : document.measure(object.id, density.value());
        mass.setText(result.massKg ? QString::number(*result.massKg, 'g', 10)
                                   : tr("Missing density for %1 part(s)").arg(result.missingDensities));
        center.setText(result.centerMm ? xyz(*result.centerMm) : QStringLiteral("—"));
        if (result.inertiaKgM2) {
            const auto& i = *result.inertiaKgM2;
            principal.setText(xyz({i[0], i[1], i[2]}));
            products.setText(xyz({i[3], i[4], i[5]}));
        } else {
            principal.setText(QStringLiteral("—"));
            products.setText(QStringLiteral("—"));
        }
    };
    update();
    QObject::connect(&density, &QDoubleSpinBox::valueChanged, &dialog, update);
    form.addRow(tr("Volume (mm³)"), &volume);
    form.addRow(tr("Geometric center (mm)"), &geometricCenter);
    form.addRow(tr("Center of mass (mm)"), &center);
    form.addRow(tr("Material density (kg/m³)"), &density);
    form.addRow(tr("Mass (kg)"), &mass);
    form.addRow(tr("Inertia Ixx, Iyy, Izz (kg m²)"), &principal);
    form.addRow(tr("Inertia Ixy, Ixz, Iyz (kg m²)"), &products);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form.addRow(&buttons);
    QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    return dialog.exec() == QDialog::Accepted ? std::optional(density.value()) : std::nullopt;
}

}  // namespace graph3d
