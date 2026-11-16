#include "io/Exchange.h"

#include <QCoreApplication>

#include "io/FileIO.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <DESTEP_Parameters.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include <QDataStream>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>

namespace graph3d::io {
namespace {

TopoDS_Shape visibleSolids(const document::Document& document)
{
    BRep_Builder builder;
    TopoDS_Compound group;
    builder.MakeCompound(group);
    bool hasBody = false;
    for (const auto& object : document.objects()) {
        if (!object.visible || std::holds_alternative<geometry::Sketch>(object.parameters)) {
            continue;
        }
        if (object.status != document::Status::Ready || object.shape.IsNull()) {
            fileError(QCoreApplication::translate("Diagnostic", "Visible body #%1 has an error. Fix or hide it before exporting.").arg(object.id));
        }
        builder.Add(group, object.shape);
        hasBody = true;
    }
    if (!hasBody) {
        fileError(QStringLiteral("There are no visible solids to export."));
    }
    return group;
}

void writeStep(const QString& path, const TopoDS_Shape& shape)
{
    STEPControl_Writer writer;
    DESTEP_Parameters parameters;
    parameters.WriteUnit = UnitsMethods_LengthUnit_Millimeter;
    std::ostringstream stream;
    if (writer.Transfer(shape, STEPControl_AsIs, parameters) != IFSelect_RetDone
        || writer.WriteStream(stream) != IFSelect_RetDone || !stream) {
        fileError(QStringLiteral("Open CASCADE could not write the STEP file."));
    }
    writeFile(path, QByteArray::fromStdString(stream.str()));
}

float coordinate(double value)
{
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
        fileError(QStringLiteral("A mesh coordinate exceeds the STL numeric range."));
    }
    return static_cast<float>(value);
}

gp_Pnt roundedPoint(gp_Pnt point)
{
    return {coordinate(point.X()), coordinate(point.Y()), coordinate(point.Z())};
}

void writeStl(const QString& path, const TopoDS_Shape& shape)
{
    // Mesh a copy so exporting cannot replace the viewport's tessellation.
    const auto copy = BRepBuilderAPI_Copy(shape, true, false).Shape();
    BRepMesh_IncrementalMesh mesh(copy, 0.05, false, 0.2, false);
    if (!mesh.IsDone()) {
        fileError(QStringLiteral("Could not tessellate the solids for STL."));
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        fileError(file.errorString());
    }
    auto header = QByteArrayLiteral("Graph3D binary STL; coordinates in millimetres");
    header.append(QByteArray(80 - header.size(), '\0'));
    QDataStream output(&file);
    output.setByteOrder(QDataStream::LittleEndian);
    output.setFloatingPointPrecision(QDataStream::SinglePrecision);
    output.writeRawData(header.constData(), 80);
    output << quint32{0};

    quint32 count = 0;
    for (TopExp_Explorer it(copy, TopAbs_FACE); it.More(); it.Next()) {
        const auto face = TopoDS::Face(it.Current());
        TopLoc_Location location;
        const auto poly = BRep_Tool::Triangulation(face, location);
        if (poly.IsNull() || poly->NbTriangles() == 0) {
            fileError(QStringLiteral("A surface could not be triangulated for STL."));
        }
        const auto transformation = location.Transformation();
        for (int i = 1; i <= poly->NbTriangles(); ++i) {
            int a, b, c;
            poly->Triangle(i).Get(a, b, c);
            if ((face.Orientation() == TopAbs_REVERSED) != transformation.IsNegative()) {
                std::swap(b, c);
            }
            const std::array vertices{
                roundedPoint(poly->Node(a).Transformed(transformation)),
                roundedPoint(poly->Node(b).Transformed(transformation)),
                roundedPoint(poly->Node(c).Transformed(transformation))
            };
            auto normal = gp_Vec(vertices[0], vertices[1]).Crossed(gp_Vec(vertices[0], vertices[2]));
            const auto magnitude = normal.Magnitude();
            if (magnitude == 0) {
                continue;
            }
            normal.Divide(magnitude);
            if (count == std::numeric_limits<quint32>::max()) {
                fileError(QStringLiteral("The mesh exceeds the binary STL triangle limit."));
            }
            output << static_cast<float>(normal.X()) << static_cast<float>(normal.Y())
                   << static_cast<float>(normal.Z());
            for (const auto& p : vertices) {
                output << static_cast<float>(p.X()) << static_cast<float>(p.Y()) << static_cast<float>(p.Z());
            }
            output << quint16{0};
            ++count;
        }
    }
    if (!count) {
        fileError(QStringLiteral("No triangles were produced."));
    }
    if (output.status() != QDataStream::Ok || !file.seek(80)) {
        fileError(file.errorString());
    }
    output << count;
    if (output.status() != QDataStream::Ok || !file.commit()) {
        fileError(file.errorString());
    }
}

}  // namespace

void exportVisible(const QString& path, const document::Document& document, ExchangeFormat format)
{
    try {
        const auto shape = visibleSolids(document);
        switch (format) {
        case ExchangeFormat::Step: writeStep(path, shape); break;
        case ExchangeFormat::Stl: writeStl(path, shape); break;
        }
    } catch (const Standard_Failure& error) {
        fileError(QCoreApplication::translate("Diagnostic", "Geometry export failed: %1").arg(QString::fromUtf8(error.GetMessageString())));
    }
}

TopoDS_Shape readStep(const QString& path)
{
    std::istringstream stream(readFile(path).toStdString());
    try {
        STEPControl_Reader reader;
        if (reader.ReadStream("graph3d.step", stream) != IFSelect_RetDone) {
            fileError(QStringLiteral("The file could not be read as STEP."));
        }
        reader.SetSystemLengthUnit(1.0);
        if (reader.TransferRoots() == 0) {
            fileError(QStringLiteral("No geometry was transferred from the STEP file."));
        }
        TopTools_IndexedMapOfShape solids;
        TopExp::MapShapes(reader.OneShape(), TopAbs_SOLID, solids);
        if (solids.IsEmpty()) {
            fileError(QStringLiteral("This STEP file contains no solid bodies."));
        }
        BRep_Builder builder;
        TopoDS_Compound result;
        builder.MakeCompound(result);
        for (int i = 1; i <= solids.Extent(); ++i) {
            const auto& solid = solids(i);
            if (!BRepCheck_Analyzer(solid, true).IsValid()) {
                fileError(QStringLiteral("The STEP file contains an invalid solid."));
            }
            builder.Add(result, solid);
        }
        return result;
    } catch (const Standard_Failure& error) {
        fileError(QCoreApplication::translate("Diagnostic", "STEP import failed: %1").arg(QString::fromUtf8(error.GetMessageString())));
    }
}

}  // namespace graph3d::io
