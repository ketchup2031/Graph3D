#include "io/Exchange.h"
#include "io/FileIO.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <DESTEP_Parameters.hxx>
#include <IGESControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <RWStl.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_Reader.hxx>
#include <TDataStd_Name.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDocStd_Document.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopExp_Explorer.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <Quantity_Color.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepLib.hxx>
#include <TopoDS.hxx>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QDir>
#include <cmath>
#include <sstream>

namespace graph3d::io {
namespace {
Handle(TDocStd_Document) newXde()
{
    Handle(TDocStd_Document) doc=new TDocStd_Document("BinXCAF");
    XCAFDoc_DocumentTool::SetLengthUnit(doc,1.,UnitsMethods_LengthUnit_Millimeter);
    return doc;
}
using Node=document::ExchangeNode;
Node exportNode(const TopoDS_Shape& shape,const document::ComponentRecord& metadata,int depth)
{
    if(depth>24) fileError(QStringLiteral("STEP assembly nesting is too deep."));
    Node node{shape,metadata.properties,{}};
    if(!metadata.children.empty()){
        std::size_t i=0;
        for(TopoDS_Iterator child(shape);child.More();child.Next()){
            if(i>=metadata.children.size()) fileError(QStringLiteral("STEP assembly structure is inconsistent."));
            node.children.push_back(exportNode(child.Value(),metadata.children[i++],depth+1));
        }
        if(i!=metadata.children.size()) fileError(QStringLiteral("STEP assembly structure is inconsistent."));
    }
    return node;
}
void metadata(const TDF_Label& label,const Handle(XCAFDoc_ColorTool)& colors,const document::PartProperties& p)
{
    if(!p.name.empty()) TDataStd_Name::Set(label,TCollection_ExtendedString(p.name.c_str(),true));
    if(p.color) colors->SetColor(label,Quantity_Color((*p.color)[0],(*p.color)[1],(*p.color)[2],Quantity_TOC_RGB),XCAFDoc_ColorGen);
}
QString labelName(const TDF_Label& label)
{
    Handle(TDataStd_Name) name;
    if(!label.FindAttribute(TDataStd_Name::GetID(),name)) return {};
    const auto& text=name->Get();
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(text.ToExtString()),text.Length());
}
}

void writeStructuredStep(const QString& path,const document::Document& document)
{
    auto doc=newXde();
    const auto shapes=XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    const auto colors=XCAFDoc_DocumentTool::ColorTool(doc->Main());
    const auto add=[&](auto&& self,const Node& node)->TDF_Label{
        const auto label=node.children.empty()?shapes->AddShape(node.shape,false):shapes->NewShape();
        metadata(label,colors,node.properties);
        for(const auto& child:node.children){
            const auto childLabel=self(self,child);
            const auto occurrence=shapes->AddComponent(label,childLabel,TopLoc_Location{});
            metadata(occurrence,colors,child.properties);
        }
        return label;
    };
    for(const auto& object:document.objects()){
        if(!object.visible||!document::isBody(object.parameters)) continue;
        auto node=exportNode(object.shape,document.componentRecord(object.id),0);
        if(node.properties.name.empty()) node.properties.name="Graph3D #"+std::to_string(object.id);
        add(add,node);
    }
    shapes->UpdateAssemblies();
    STEPCAFControl_Writer writer;
    DESTEP_Parameters parameters;
    parameters.WriteUnit=UnitsMethods_LengthUnit_Millimeter;
    writer.SetColorMode(true);writer.SetNameMode(true);
    std::ostringstream stream;
    if(!writer.Transfer(doc,parameters) || writer.WriteStream(stream)!=IFSelect_RetDone || !stream)
        fileError(QStringLiteral("Open CASCADE could not write the structured STEP file."));
    writeFile(path,QByteArray::fromStdString(stream.str()));
}

std::vector<Node> readStepTree(const QString& path)
{
    std::istringstream bytes(readFile(path).toStdString());
    try {
        auto doc=newXde();
        STEPCAFControl_Reader reader;
        if(reader.ReadStream("graph3d.step",bytes)!=IFSelect_RetDone)
            fileError(QStringLiteral("The file could not be read as STEP."));
        reader.ChangeReader().SetSystemLengthUnit(1.);
        reader.SetColorMode(true);reader.SetNameMode(true);
        if(!reader.Transfer(doc)) fileError(QStringLiteral("STEP did not transfer any roots."));
        const auto shapes=XCAFDoc_DocumentTool::ShapeTool(doc->Main());
        const auto colors=XCAFDoc_DocumentTool::ColorTool(doc->Main());
        int budget=10000;
        const auto read=[&](auto&& self,TDF_Label occurrence,const TopLoc_Location& parent,int depth)->Node {
            if(depth>24 || --budget<0) fileError(QStringLiteral("STEP assembly exceeds the import limits."));
            TDF_Label product=occurrence;
            const auto loc=parent*XCAFDoc_ShapeTool::GetLocation(occurrence);
            const bool reference=XCAFDoc_ShapeTool::GetReferredShape(occurrence,product);
            if(!reference) product=occurrence;
            Node result;
            auto name=labelName(occurrence);
            if(name.isEmpty() || (reference && name.startsWith("=>"))) name=labelName(product);
            result.properties.name=name.toStdString();
            Quantity_Color color;
            const auto findColor=[&](const TDF_Label& label){
                return colors->GetColor(label,XCAFDoc_ColorGen,color)||colors->GetColor(label,XCAFDoc_ColorSurf,color);
            };
            if(findColor(occurrence)||findColor(product)) result.properties.color=geometry::Point3{color.Red(),color.Green(),color.Blue()};
            TDF_LabelSequence children;
            if(XCAFDoc_ShapeTool::GetComponents(product,children)){
                for(int i=1;i<=children.Length();++i) result.children.push_back(self(self,children.Value(i),loc,depth+1));
            } else {
                auto shape=XCAFDoc_ShapeTool::GetShape(product);
                if(!shape.IsNull()) {
                    if(reference) shape=shape.Moved(loc);
                    else shape=shape.Moved(parent);
                }
                if(shape.IsNull()||!TopExp_Explorer(shape,TopAbs_SOLID).More()||!BRepCheck_Analyzer(shape).IsValid())
                    fileError(QStringLiteral("A STEP component is not a valid solid."));
                result.shape=std::move(shape);
            }
            return result;
        };
        TDF_LabelSequence roots;
        shapes->GetFreeShapes(roots);
        std::vector<Node> result;
        for(int i=1;i<=roots.Length();++i) result.push_back(read(read,roots.Value(i),{},0));
        if(result.empty()) fileError(QStringLiteral("This STEP file contains no solid bodies."));
        return result;
    } catch(const Standard_Failure& e) {
        fileError(QStringLiteral("STEP import failed: %1").arg(QString::fromUtf8(e.GetMessageString())));
    }
}

void writeIges(const QString& path,const TopoDS_Shape& shape)
{
    IGESControl_Writer writer("MM",1);
    std::ostringstream stream;
    if(!writer.AddShape(shape)||!writer.Write(stream)||!stream) fileError(QStringLiteral("Open CASCADE could not write the IGES file."));
    writeFile(path,QByteArray::fromStdString(stream.str()));
}

TopoDS_Shape readIges(const QString& path)
{
    const auto bytes = readFile(path);
    QTemporaryFile input(QDir::temp().filePath("graph3d-iges-XXXXXX.igs"));
    if (!input.open() || input.write(bytes) != bytes.size() || !input.flush()) {
        fileError(QStringLiteral("Could not stage IGES input."));
    }
    input.close();
    try {
        IGESControl_Reader reader;
        if(reader.ReadFile(QFile::encodeName(input.fileName()).constData())!=IFSelect_RetDone || !reader.TransferRoots())
            fileError(QStringLiteral("The file could not be read as IGES."));
        auto shape=reader.OneShape();
        if(!shape.IsNull() && TopExp_Explorer(shape,TopAbs_SOLID).More() && BRepCheck_Analyzer(shape).IsValid()) return shape;
        BRepBuilderAPI_Sewing sewing(1.e-5);
        sewing.Add(shape);sewing.Perform();
        const auto sewed=sewing.SewedShape();
        TopoDS_Compound group;
        BRep_Builder builder; builder.MakeCompound(group);
        int count=0;
        for(TopExp_Explorer shells(sewed,TopAbs_SHELL);shells.More();shells.Next()){
            auto solid=BRepBuilderAPI_MakeSolid(TopoDS::Shell(shells.Current())).Solid();
            if(!BRepLib::OrientClosedSolid(solid) || !BRepCheck_Analyzer(solid).IsValid())
                fileError(QStringLiteral("IGES contains open or invalid shells."));
            builder.Add(group,solid); ++count;
        }
        if(!count) fileError(QStringLiteral("IGES contains no closed solid."));
        return group;
    } catch(const Standard_Failure& e) {
        fileError(QStringLiteral("IGES import failed: %1").arg(QString::fromUtf8(e.GetMessageString())));
    }
}

document::ImportedMesh readStl(const QString& path,double scale)
{
    if(!std::isfinite(scale)||scale<=0||scale>1000000) fileError(QStringLiteral("Enter a valid STL unit scale."));
    const auto bytes=readFile(path);
    QTemporaryFile file(QDir::temp().filePath("graph3d-stl-XXXXXX.stl"));
    if(!file.open()||file.write(bytes)!=bytes.size()||!file.flush()) fileError(QStringLiteral("Could not stage STL input."));
    file.close();
    const auto mesh=RWStl::ReadFile(QFile::encodeName(file.fileName()).constData());
    if(mesh.IsNull()||mesh->NbTriangles()<1||mesh->NbTriangles()>500000) fileError(QStringLiteral("Invalid or oversized STL mesh."));
    document::ImportedMesh result{QFileInfo(path).fileName().toStdString()};
    result.vertices.reserve(mesh->NbTriangles()*3);
    for(int i=1;i<=mesh->NbTriangles();++i){
        int a,b,c;mesh->Triangle(i).Get(a,b,c);
        for(int j:{a,b,c}){
            const auto vertex=mesh->Node(j);
            result.vertices.push_back({vertex.X()*scale,vertex.Y()*scale,vertex.Z()*scale});
        }
    }
    return result;
}
}  // namespace graph3d::io
