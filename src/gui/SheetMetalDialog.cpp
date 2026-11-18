#include "gui/MainWindow.h"
#include "gui/DocumentWindow.h"
#include "gui/DraftWindow.h"
#include "gui/InterfaceLanguage.h"
#include "io/FlatDrawing.h"
#include "io/DxfIO.h"
#include "viewport/Viewport.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <Standard_Failure.hxx>
#include <stdexcept>

namespace graph3d {
void MainWindow::configureSheetMetal(int id)
{
    if (!hasWorkspace_ && !showWorkspace()) { return; }
    if (pages_->currentWidget()!=viewport_ || pendingMachining_) { return; }
    geometry::SheetMetal sheet;
    if (id) {
        const auto* object=document_.find(id);
        const auto* data=object ? std::get_if<geometry::SheetMetal>(&object->parameters) : nullptr;
        if (!data) { report(tr("Select a sheet metal part.")); return; }
        sheet=*data;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("sheetMetalDialog"));
    dialog.setWindowTitle(tr("Sheet metal"));
    dialog.resize(780,510);
    auto* outer=new QVBoxLayout(&dialog);
    auto* form=new QFormLayout;
    outer->addLayout(form);
    const auto size=[&](QString name,double value) {
        auto* edit=new QDoubleSpinBox(&dialog); edit->setRange(.01,1.e5); edit->setDecimals(3);
        edit->setValue(value); form->addRow(std::move(name),edit); return edit;
    };
    const auto length=size(tr("Base length (mm)"),sheet.length), width=size(tr("Width (mm)"),sheet.width),
        thickness=size(tr("Thickness (mm)"),sheet.thickness);
    auto* tabs=new QTabWidget(&dialog);
    outer->addWidget(tabs);
    const auto table=[&](QString name,QStringList labels,QStringList defaults,int limit) {
        auto* page=new QWidget(tabs); tabs->addTab(page,name);
        auto* layout=new QVBoxLayout(page);
        auto* grid=new QTableWidget(0,labels.size(),page);
        grid->setHorizontalHeaderLabels(labels);
        grid->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        grid->setSelectionBehavior(QAbstractItemView::SelectRows);
        layout->addWidget(grid);
        auto* tools=new QHBoxLayout; layout->addLayout(tools);
        auto* add=new QPushButton(tr("Add row"),page); tools->addWidget(add);
        auto* remove=new QPushButton(tr("Remove row"),page); tools->addWidget(remove);
        connect(add,&QPushButton::clicked,page,[=]{
            if (grid->rowCount()>=limit) return;
            const int index=grid->rowCount(); grid->insertRow(index);
            for(int i=0;i<defaults.size();++i) grid->setItem(index,i,new QTableWidgetItem(defaults[i]));
        });
        connect(remove,&QPushButton::clicked,page,[=]{
            if(grid->currentRow()>=0) grid->removeRow(grid->currentRow());
        });
        return grid;
    };
    auto* bends=table(tr("Bends and flanges"),{tr("Left (0/1)"),tr("Flange (mm)"),tr("Radius (mm)"),tr("Angle (°)"),
        tr("K factor"),tr("Relief (mm)")},{"0","20","2","90","0.42","1"},24);
    auto* holes=table(tr("Sheet holes"),{tr("Panel (0 = base)"),tr("Along (mm)"),tr("Across (mm)"),tr("Diameter (mm)")},
        {"0","10","10","6"},128);
    const auto row=[](QTableWidget* grid,std::initializer_list<double> values){
        const int r=grid->rowCount(); grid->insertRow(r); int i=0;
        for(double value:values) grid->setItem(r,i++,new QTableWidgetItem(QString::number(value,'g',10)));
    };
    for(const auto& b:sheet.bends) row(bends,{b.left?1.:0.,b.flange,b.radius,b.degrees,b.factor,b.relief});
    for(const auto& h:sheet.holes) row(holes,{static_cast<double>(h.panel),h.along,h.across,h.diameter});
    auto* output=new QLabel(tr("Bends on the same side continue from the previous flange. Use a negative angle to bend down."),&dialog);
    output->setWordWrap(true); output->setTextFormat(Qt::PlainText); outer->addWidget(output);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    auto* check=buttons->addButton(tr("Check development"),QDialogButtonBox::ActionRole);
    outer->addWidget(buttons);
    const auto read=[](QTableWidget* table,int r,int c) {
        bool ok=false;
        const auto* cell=table->item(r,c);
        const double value=cell ? cell->text().toDouble(&ok) : 0;
        if(!ok) throw std::invalid_argument("Use numbers with a decimal point in the sheet table.");
        return value;
    };
    const auto checkSheet=[&](bool apply) {
        try {
            geometry::SheetMetal next{length->value(),width->value(),thickness->value()};
            for(int i=0;i<bends->rowCount();++i) {
                const auto side=read(bends,i,0);
                if(side!=0 && side!=1) throw std::invalid_argument("Use 0 for right or 1 for left.");
                next.bends.push_back({side==1,read(bends,i,1),read(bends,i,2),read(bends,i,3),read(bends,i,4),read(bends,i,5)});
            }
            for(int i=0;i<holes->rowCount();++i) {
                const auto index=read(holes,i,0);
                if(!(index>=0 && index<=24) || index!=static_cast<int>(index)) throw std::invalid_argument("Invalid sheet panel number.");
                next.holes.push_back({static_cast<int>(index),read(holes,i,1),read(holes,i,2),read(holes,i,3)});
            }
            const auto flat=geometry::flattenSheet(next);
            output->setText(tr("Developed blank: %1 × %2 mm.").arg(flat.length,0,'f',3).arg(flat.width));
            if(apply){sheet=std::move(next); dialog.accept();}
        } catch(const std::exception& error) { output->setText(localizedDiagnostic(error.what())); }
        catch(const Standard_Failure& error) { output->setText(localizedDiagnostic(error.GetMessageString())); }
    };
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{checkSheet(true);});
    connect(check,&QPushButton::clicked,&dialog,[&]{checkSheet(false);});
    if(dialog.exec()!=QDialog::Accepted) return;
    if(id) editFeature(id,[&]{return document_.editSheetMetal(id,sheet);});
    else addFeature(tr("Sheet metal"),[&]{return document_.addSheetMetal(sheet);});
}

void MainWindow::showFlatSheet(bool dxf)
{
    const auto id=selectedId();
    const auto* object=document_.find(id);
    if(!object || !std::holds_alternative<geometry::SheetMetal>(object->parameters)) { report(tr("Select a sheet metal part.")); return; }
    try {
        if (!dxf && projectPath_.isEmpty() && !saveProject(false)) { return; }
        auto draft=io::flatDrawing(std::get<geometry::SheetMetal>(document_.effectiveParameters(id)));
        if (!dxf) {
            draft.flatPattern = drawing::FlatPatternReference{projectPath_,id,{}};
            for (const auto& item : draft.items) { draft.flatPattern->geometry.push_back(item.id); }
        }
        if(dxf) {
            auto path=QFileDialog::getSaveFileName(this,tr("Export flat DXF"),{},tr("DXF files (*.dxf)"));
            if(path.isEmpty()) return;
            if(!path.endsWith(".dxf",Qt::CaseInsensitive)) path+=QStringLiteral(".dxf");
            io::exportDxf(path,draft);
            report(tr("Exported sheet development."));
        } else if(host_) { host_->createDrawing(std::move(draft)); }
        else {
            auto* window=new DraftWindow(nullptr);
            window->setAttribute(Qt::WA_DeleteOnClose);
            window->replaceDrawing(std::move(draft));
            window->show();
        }
    } catch(const std::exception& error) { showSceneError(localizedDiagnostic(error.what())); }
    catch(const Standard_Failure& error) { showSceneError(localizedDiagnostic(error.GetMessageString())); }
}
}  // namespace graph3d
