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

Read sweepEditor(OperationDialog& dialog, const document::Sweep& sweep)
{
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
    return [edit, profile = sweep.profile]() -> document::Parameters {
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
            if (object.status == document::Status::Ready && std::holds_alternative<geometry::Sketch>(object.parameters)
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
    const document::Document& document, const document::Parameters& initial, bool editing)
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
        return dialog.run(sweepEditor(dialog, *p));
    }
    if (const auto* p = std::get_if<document::Loft>(&initial)) {
        return dialog.run(loftEditor(dialog, document, *p, editing));
    }
    if (const auto* p = std::get_if<document::Shell>(&initial)) {
        return dialog.run(facesEditor(dialog, document.find(p->body)->shape, p->body, p->openings, {}, p->thickness));
    }
    const auto& p = std::get<document::DraftAngle>(initial);
    return dialog.run(facesEditor(dialog, document.find(p.body)->shape, p.body, p.walls, p.neutral, p.degrees));
}

std::optional<double> bodyPropertiesDialog(QWidget* parent, const document::Object& object)
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
    QLabel volume, mass, center;
    const auto values = geometry::massProperties(object.shape, 0);
    volume.setText(QString::number(values.volumeMm3, 'g', 10));
    center.setText(xyz(values.centerMm));
    const auto update = [&] {
        mass.setText(density.value() > 0 ? QString::number(values.volumeMm3 * density.value() / 1.e9, 'g', 10)
                                         : tr("Set a density to calculate mass"));
    };
    update();
    QObject::connect(&density, &QDoubleSpinBox::valueChanged, &dialog, update);
    form.addRow(tr("Volume (mm³)"), &volume);
    form.addRow(tr("Center of mass (mm)"), &center);
    form.addRow(tr("Material density (kg/m³)"), &density);
    form.addRow(tr("Mass (kg)"), &mass);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form.addRow(&buttons);
    QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    return dialog.exec() == QDialog::Accepted ? std::optional(density.value()) : std::nullopt;
}

}  // namespace graph3d
