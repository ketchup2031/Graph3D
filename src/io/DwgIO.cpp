#include "io/DxfIO.h"
#include "io/FileIO.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace graph3d::io {
namespace {
QString utility(QString name)
{
#ifdef Q_OS_WIN
    name+=QStringLiteral(".exe");
#endif
    const auto directory=qEnvironmentVariable("GRAPH3D_LIBREDWG_DIR");
    const auto path=directory.isEmpty()?QStandardPaths::findExecutable(name):QDir(directory).absoluteFilePath(name);
    if(path.isEmpty()||!QFileInfo(path).isExecutable())
        fileError(QStringLiteral("Install GNU LibreDWG and set GRAPH3D_LIBREDWG_DIR to its tools folder."));
    return path;
}
void convert(const QString& program,const QTemporaryDir& dir,const QStringList& options,const QString& output)
{
    QProcess converter;
    converter.setWorkingDirectory(dir.path());
    converter.setProcessChannelMode(QProcess::MergedChannels);
    converter.setStandardOutputFile(QProcess::nullDevice());
    converter.start(utility(program),options);
    if(!converter.waitForStarted(5000)||!converter.waitForFinished(45000)){
        converter.kill(); converter.waitForFinished(1000);
        fileError(QStringLiteral("The DWG converter failed to start or timed out."));
    }
    if(converter.exitStatus()!=QProcess::NormalExit || converter.exitCode()!=0 || !QFileInfo::exists(dir.filePath(output)))
        fileError(QStringLiteral("LibreDWG could not convert the file."));
}
}
DxfDrawing importDwg(const QString& path)
{
    QTemporaryDir temp;
    if(!temp.isValid()) fileError(QStringLiteral("Could not prepare DWG conversion."));
    writeFile(temp.filePath("input.dwg"),readFile(path));
    convert("dwg2dxf",temp,{"--as","r2000","-o","output.dxf","input.dwg"},"output.dxf");
    auto result=importDxf(temp.filePath("output.dxf"));
    result.drawing.title=QFileInfo(path).completeBaseName().left(200);
    return result;
}
QMap<QString,int> exportDwg(const QString& path,const drawing::Draft& draft)
{
    QTemporaryDir temp;
    if(!temp.isValid()) fileError(QStringLiteral("Could not prepare DWG conversion."));
    auto report=exportDxfReport(temp.filePath("input.dxf"),draft);
    convert("dxf2dwg",temp,{"--as","r2000","-o","output.dwg","input.dxf"},"output.dwg");
    const auto bytes=readFile(temp.filePath("output.dwg"));
    if(!bytes.startsWith("AC1015")) fileError(QStringLiteral("Invalid DWG from the converter."));
    // Some converter versions return success even when the file is unreadable.
    convert("dwg2dxf",temp,{"--as","r2000","-o","check.dxf","output.dwg"},"check.dxf");
    const auto checked=importDxf(temp.filePath("check.dxf"));
    for(auto loss=checked.losses.cbegin();loss!=checked.losses.cend();++loss) {
        report[QStringLiteral("DWG/")+loss.key()] += loss.value();
    }
    if(!draft.additionalSheets.empty()) report[QStringLiteral("DWG additional sheets")]+=static_cast<int>(draft.additionalSheets.size());
    writeFile(path,bytes);
    return report;
}
}  // namespace graph3d::io
