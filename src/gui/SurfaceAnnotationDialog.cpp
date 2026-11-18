#include "gui/MainWindow.h"
#include "gui/InterfaceLanguage.h"
#include "viewport/Viewport.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStackedWidget>

#include <algorithm>
#include <stdexcept>

namespace graph3d {
namespace {

void buttons(QDialog& dialog, QFormLayout& form)
{
    auto* bar = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form.addRow(bar);
    QObject::connect(bar, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(bar, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
}

QString pointText(const geometry::Point3& point)
{
    return QStringLiteral("%1, %2, %3").arg(point[0], 0, 'g', 8).arg(point[1], 0, 'g', 8).arg(point[2], 0, 'g', 8);
}

}  // namespace

void MainWindow::configureSpatialCurve(int id)
{
    if ((!hasWorkspace_ && !showWorkspace()) || pages_->currentWidget() != viewport_ || pendingMachining_) { return; }
    const auto curve = id ? std::get<geometry::SpatialCurve>(document_.find(id)->parameters)
                         : geometry::SpatialCurve{{{0,0,0},{30,0,20},{40,30,35}}};
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("spatialCurveDialog"));
    dialog.setWindowTitle(tr("Spatial curve"));
    QFormLayout form(&dialog);
    QStringList lines;
    for (const auto& point : curve.points) { lines << pointText(point); }
    QPlainTextEdit points(lines.join(u'\n'));
    points.setObjectName(QStringLiteral("spatialPoints"));
    points.setPlaceholderText(QStringLiteral("0, 0, 0\n30, 0, 20"));
    QCheckBox spline(tr("Interpolate a spline")), closed(tr("Close the curve"));
    closed.setObjectName(QStringLiteral("spatialClosed"));
    spline.setChecked(curve.spline);
    closed.setChecked(curve.closed);
    form.addRow(tr("One X, Y, Z point per line (mm)"), &points);
    form.addRow(&spline);
    form.addRow(&closed);
    buttons(dialog, form);
    if (dialog.exec() != QDialog::Accepted) { return; }
    try {
        geometry::SpatialCurve next{{}, spline.isChecked(), closed.isChecked()};
        for (const auto& row : points.toPlainText().split(u'\n', Qt::SkipEmptyParts)) {
            if (row.trimmed().isEmpty()) { continue; }
            const auto columns = row.split(u',');
            if (columns.size() != 3) { throw std::invalid_argument("Enter three comma-separated coordinates per line."); }
            geometry::Point3 point;
            for (int i = 0; i < 3; ++i) {
                bool ok;
                point[i] = columns[i].trimmed().toDouble(&ok);
                if (!ok) { throw std::invalid_argument("Enter three comma-separated coordinates per line."); }
            }
            next.points.push_back(point);
        }
        if (id) { editFeature(id, [&] { return document_.editAdvanced(id, next); }); }
        else { addFeature(tr("Spatial curve"), [&] { return document_.addAdvanced(next); }); }
    } catch (const std::exception& error) {
        showSceneError(localizedDiagnostic(error.what()));
    }
}

void MainWindow::configureSurface(geometry::SurfaceKind kind, int id)
{
    using K = geometry::SurfaceKind;
    if ((!hasWorkspace_ && !showWorkspace()) || pages_->currentWidget() != viewport_ || pendingMachining_) { return; }
    auto value = id ? std::get<document::Surface>(document_.find(id)->parameters)
                    : document::Surface{kind, {}, kind == K::Extend ? 10. : .001};
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("surfaceOperationDialog"));
    const std::array titles{tr("Surface loft"), tr("Surface sweep"), tr("Curve network"), tr("Trimmed surface"),
        tr("Extended surface"), tr("Patch"), tr("Sewn shell"), tr("Solid from surfaces"), tr("Open edges")};
    dialog.setWindowTitle(titles.at(static_cast<std::size_t>(kind)));
    QFormLayout form(&dialog);
    QComboBox sources;
    for (const auto& candidate : document_.objects()) {
        if ((id && candidate.id >= id) || candidate.status != document::Status::Ready) { continue; }
        if (document::isSurface(candidate.parameters) || std::holds_alternative<geometry::SpatialCurve>(candidate.parameters)
            || std::holds_alternative<geometry::Sketch>(candidate.parameters)) {
            sources.addItem(objectName(candidate), candidate.id);
        }
    }
    if (sources.count() == 0) { report(tr("Create a curve, sketch or surface first.")); return; }
    QLineEdit ids;
    ids.setObjectName(QStringLiteral("surfaceInputs"));
    QStringList prior;
    for (const auto source : value.sources) { prior << QString::number(source); }
    if (prior.empty() && sources.findData(selectedId()) >= 0) { prior << QString::number(selectedId()); }
    ids.setText(prior.join(QStringLiteral(", ")));
    const std::array instructions{tr("Sections in order"), tr("Section, then trajectory"), tr("Boundary curves, then internal curves"),
        tr("Surface, then cutter"), tr("One surface"), tr("Closed curve or open surface"),
        tr("Faces or shells to sew"), tr("Faces forming a closed shell"), tr("One open surface")};
    form.addRow(instructions.at(static_cast<std::size_t>(kind)), &ids);
    QPushButton add(tr("Append input"));
    form.addRow(&sources, &add);
    connect(&add, &QPushButton::clicked, &dialog, [&] {
        ids.setText(ids.text().trimmed() + (ids.text().trimmed().isEmpty() ? QString{} : QStringLiteral(", "))
                    + sources.currentData().toString());
    });
    QSpinBox index;
    index.setRange(1, 32);
    index.setValue(value.index);
    QCheckBox ruled(tr("Ruled surface"));
    ruled.setChecked(value.index != 0 && id);
    QComboBox direction;
    direction.addItems({tr("U start"), tr("U end"), tr("V start"), tr("V end")});
    direction.setCurrentIndex(std::clamp(value.index - 1, 0, 3));
    QDoubleSpinBox distance;
    distance.setDecimals(6);
    distance.setRange(.000001, kind == K::Extend ? 100000. : 1.);
    distance.setValue(value.distance);
    if (kind == K::Loft) { form.addRow(&ruled); }
    if (kind == K::Network) { form.addRow(tr("Number of boundary inputs"), &index); }
    if (kind == K::Trim) { form.addRow(tr("Piece to keep"), &index); }
    if (kind == K::Patch) { form.addRow(tr("Boundary loop number"), &index); }
    if (kind == K::Extend) {
        form.addRow(tr("Surface boundary"), &direction);
        form.addRow(tr("Extension (mm)"), &distance);
    }
    if (kind == K::Sew || kind == K::Solid) { form.addRow(tr("Sewing tolerance (mm)"), &distance); }
    buttons(dialog, form);
    if (dialog.exec() != QDialog::Accepted) { return; }
    value.sources.clear();
    for (const auto& token : ids.text().split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts)) {
        bool ok;
        const int source = token.toInt(&ok);
        if (!ok || sources.findData(source) < 0) { report(tr("Enter valid input object numbers.")); return; }
        value.sources.push_back(source);
    }
    value.distance = distance.value();
    value.index = kind == K::Loft ? static_cast<int>(ruled.isChecked())
        : kind == K::Extend ? direction.currentIndex() + 1 : index.value();
    if (id) { editFeature(id, [&] { return document_.editAdvanced(id, value); }); }
    else { addFeature(dialog.windowTitle(), [&] { return document_.addAdvanced(value); }); }
}

void MainWindow::chooseCatalogMaterial()
{
    const auto* object = document_.find(selectedId());
    if (!object || pages_->currentWidget() != viewport_ || !document::isBody(object->parameters)) {
        report(tr("Select a solid body."));
        return;
    }
    QStringList names;
    const auto catalog = document::materialCatalog();
    for (const auto& material : catalog) {
        names << tr("%1 · %2 kg/m³").arg(QCoreApplication::translate("Materials", material.name)).arg(material.densityKgM3);
    }
    bool accepted = false;
    const auto choice = QInputDialog::getItem(this, tr("Material catalog"), tr("Material"), names, 0, false, &accepted);
    if (!accepted) { return; }
    const auto id = object->id;
    if (const auto index = names.indexOf(choice); index >= 0 && document_.setCatalogMaterial(id, catalog[index].name)) {
        refreshDocument(id);
        report(tr("Material assigned."));
    }
}

void MainWindow::configureAnnotation(document::AnnotationKind kind, int id)
{
    using K = document::AnnotationKind;
    if (pages_->currentWidget() != viewport_ || pendingMachining_) { return; }
    auto value = id ? std::get<document::Annotation3d>(document_.find(id)->parameters)
                   : document::Annotation3d{kind, selectedId()};
    const auto* body = document_.find(value.body);
    if (!body || !document::isBody(body->parameters) || body->status != document::Status::Ready) {
        report(tr("Select a solid body."));
        return;
    }
    if (kind == K::Diameter && !std::holds_alternative<document::Hole>(body->parameters)) {
        report(tr("Select a hole feature for a diameter annotation."));
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("annotation3dDialog"));
    const std::array titles{tr("Linear dimension"), tr("Hole diameter"), tr("Datum"), tr("Surface roughness")};
    dialog.setWindowTitle(titles.at(static_cast<std::size_t>(kind)));
    QFormLayout form(&dialog);
    form.addRow(tr("Source geometry"), new QLabel(objectName(*body), &dialog));
    QComboBox first, second;
    QLineEdit notation;
    QDoubleSpinBox roughness;
    std::vector<geometry::FaceReference> faces;
    if (kind == K::Linear) {
        const auto vertices = document::annotationVertices(body->shape);
        if (vertices.size() < 2) { report(tr("The body needs at least two vertices.")); return; }
        for (const auto& vertex : vertices) { first.addItem(pointText(vertex)); second.addItem(pointText(vertex)); }
        first.setCurrentIndex(value.first - 1);
        second.setCurrentIndex(value.second - 1);
        form.addRow(tr("First vertex"), &first);
        form.addRow(tr("Second vertex"), &second);
    } else if (kind == K::Datum || kind == K::Roughness) {
        faces = geometry::planarFaces(body->shape);
        if (faces.empty()) { report(tr("The body has no planar face.")); return; }
        for (const auto& face : faces) { first.addItem(pointText(face.center) + QStringLiteral(" / ") + pointText(face.outward)); }
        if (const auto previous = std::ranges::find(faces, value.face); previous != faces.end()) {
            first.setCurrentIndex(static_cast<int>(previous - faces.begin()));
        }
        form.addRow(tr("Face center / normal"), &first);
        if (kind == K::Datum) {
            notation.setMaxLength(12);
            notation.setText(id ? QString::fromStdString(value.text) : QStringLiteral("A"));
            form.addRow(tr("Datum designation"), &notation);
        } else {
            roughness.setDecimals(3);
            roughness.setRange(.001, 100);
            roughness.setValue(id ? QString::fromStdString(value.text).toDouble() : 3.2);
            form.addRow(tr("Ra (µm)"), &roughness);
        }
    }
    buttons(dialog, form);
    if (dialog.exec() != QDialog::Accepted) { return; }
    if (kind == K::Linear) { value.first = first.currentIndex() + 1; value.second = second.currentIndex() + 1; }
    if (kind == K::Datum || kind == K::Roughness) {
        value.face = faces.at(first.currentIndex());
        value.text = (kind == K::Datum ? notation.text().trimmed() : QString::number(roughness.value())).toStdString();
    }
    if (id) { editFeature(id, [&] { return document_.editAnnotation(id, value); }); }
    else { addFeature(dialog.windowTitle(), [&] { return document_.addAnnotation(value); }); }
}

}  // namespace graph3d
