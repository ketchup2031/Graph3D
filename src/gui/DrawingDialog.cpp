#include "gui/DrawingDialog.h"

#include "drawing/DrawingSheet.h"
#include "gui/UserPreferences.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <exception>
#include <optional>
#include <utility>

namespace graph3d {

class DrawingDialog::Preview final : public QWidget {
public:
    Preview(drawing::Projections projections, QWidget* parent)
        : QWidget(parent), views(std::move(projections))
    {
        setObjectName(QStringLiteral("drawingPreviewCanvas"));
        setAccessibleName(tr("Drawing sheet"));
        setMinimumSize(430, 310);
    }

    drawing::Projections views;
    drawing::SheetOptions options;
    std::optional<drawing::Section> section;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().mid());
        const QSizeF size = QSizeF(drawing::sheetWidth, drawing::sheetHeight)
            .scaled(QSizeF(width() - 24, height() - 24), Qt::KeepAspectRatio);
        const QRectF paper((width() - size.width()) / 2, (height() - size.height()) / 2,
                           size.width(), size.height());
        drawing::paintSheet(painter, paper, views, options, section ? &*section : nullptr);
    }
};

DrawingDialog::DrawingDialog(const TopoDS_Shape& body, drawing::Projections projections,
                             const QString& object, int id, QWidget* parent)
    : QDialog(parent), body_(body), preview_(new Preview(std::move(projections), this))
{
    setObjectName(QStringLiteral("drawingDialog"));
    setWindowTitle(tr("Drawing — %1").arg(object));
    resize(1010, 740);
    preview_->options.title = object;
    preview_->options.designation = QStringLiteral("G3D-%1").arg(id, 4, 10, QLatin1Char('0'));

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    auto* title = new QLineEdit(preview_->options.title, this);
    title->setObjectName(QStringLiteral("drawingTitle"));
    title->setMaxLength(80);
    form->addRow(tr("Part name"), title);
    auto* designation = new QLineEdit(preview_->options.designation, this);
    designation->setObjectName(QStringLiteral("drawingDesignation"));
    designation->setMaxLength(32);
    form->addRow(tr("Drawing number"), designation);
    layout->addLayout(form);

    auto* options = new QHBoxLayout;
    auto* hidden = new QCheckBox(tr("Hidden edges"), this);
    auto* dimensions = new QCheckBox(tr("Dimensions and center marks"), this);
    dimensions->setToolTip(tr("Overall dimensions plus complete circles in the top view"));
    hidden->setObjectName(QStringLiteral("drawingHidden"));
    dimensions->setObjectName(QStringLiteral("drawingDimensions"));
    hidden->setChecked(preview_->options.hiddenLines);
    dimensions->setChecked(preview_->options.dimensions);
    options->addWidget(hidden);
    options->addWidget(dimensions);
    options->addStretch();
    layout->addLayout(options);
    auto* sectionSettings = new QHBoxLayout;
    auto* plane = new QComboBox(this);
    plane->setObjectName(QStringLiteral("drawingSectionPlane"));
    plane->addItem(tr("None"));
    using geometry::Plane;
    plane->addItem(tr("XZ — front"), static_cast<int>(Plane::XZ));
    plane->addItem(tr("XY — top"), static_cast<int>(Plane::XY));
    plane->addItem(tr("YZ — right"), static_cast<int>(Plane::YZ));
    auto* offset = new QDoubleSpinBox(this);
    offset->setObjectName(QStringLiteral("drawingSectionOffset"));
    offset->setDecimals(3);
    offset->setSuffix(tr(" mm"));
    offset->setKeyboardTracking(false);
    offset->setEnabled(false);
    offset->setToolTip(tr("Signed distance from the center of the body bounds along the perpendicular world axis."));
    auto* planeLabel = new QLabel(tr("Section plane"), this);
    planeLabel->setBuddy(plane);
    auto* offsetLabel = new QLabel(tr("Offset from center"), this);
    offsetLabel->setBuddy(offset);
    sectionSettings->addWidget(planeLabel);
    sectionSettings->addWidget(plane);
    sectionSettings->addWidget(offsetLabel);
    sectionSettings->addWidget(offset);
    sectionSettings->addStretch();
    layout->addLayout(sectionSettings);
    auto* sectionError = new QLabel(this);
    sectionError->setObjectName(QStringLiteral("drawingSectionError"));
    sectionError->setTextFormat(Qt::PlainText);
    sectionError->setWordWrap(true);
    sectionError->hide();
    layout->addWidget(sectionError);
    layout->addWidget(preview_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* pdf = buttons->addButton(tr("Save PDF…"), QDialogButtonBox::ActionRole);
    pdf->setObjectName(QStringLiteral("drawingSavePdf"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(pdf, &QPushButton::clicked, this, &DrawingDialog::exportPdf);

    const auto updateSection = [this, plane, offset, sectionError, pdf] {
        preview_->section.reset();
        sectionError->hide();
        pdf->setEnabled(true);
        if (plane->currentIndex() != 0) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            try {
                const auto orientation = static_cast<Plane>(plane->currentData().toInt());
                preview_->section = drawing::section(body_, orientation, offset->value());
            } catch (const std::exception& error) {
                sectionError->setText(QString::fromUtf8(error.what()));
                sectionError->show();
                pdf->setEnabled(false);
            }
            QApplication::restoreOverrideCursor();
        }
        preview_->update();
    };
    connect(plane, &QComboBox::currentIndexChanged, this, [this, plane, offset, updateSection](int index) {
        {
            const QSignalBlocker blocked(offset);
            offset->setEnabled(index != 0);
            if (index != 0) {
                const auto orientation = static_cast<Plane>(plane->currentData().toInt());
                const auto& views = preview_->views;
                const double span = orientation == Plane::XZ ? views.top.height
                                  : orientation == Plane::XY ? views.front.height : views.front.width;
                offset->setRange(-span / 2, span / 2);
            }
            offset->setValue(0);
        }
        updateSection();
    });
    connect(offset, &QDoubleSpinBox::valueChanged, this, updateSection);
    connect(title, &QLineEdit::textChanged, this, [this](const QString& value) {
        preview_->options.title = value;
        preview_->update();
    });
    connect(designation, &QLineEdit::textChanged, this, [this](const QString& value) {
        preview_->options.designation = value;
        preview_->update();
    });
    connect(hidden, &QCheckBox::toggled, this, [this](bool checked) {
        preview_->options.hiddenLines = checked;
        preview_->update();
    });
    connect(dimensions, &QCheckBox::toggled, this, [this](bool checked) {
        preview_->options.dimensions = checked;
        preview_->update();
    });
}

void DrawingDialog::exportPdf()
{
    QFileDialog dialog(this, tr("Save drawing"), {}, tr("PDF files (*.pdf)"));
    dialog.setOption(QFileDialog::DontUseNativeDialog, darkThemeEnabled());
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(QStringLiteral("pdf"));
    dialog.selectFile(QStringLiteral("Graph3D-drawing.pdf"));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
        return;
    }
    auto path = dialog.selectedFiles().constFirst();
    if (!path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        QMessageBox::warning(this, tr("Save drawing"), tr("Use a .pdf filename."));
        return;
    }
    try {
        drawing::savePdf(path, preview_->views, preview_->options,
                         preview_->section ? &*preview_->section : nullptr);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Save drawing"), QString::fromUtf8(error.what()));
    }
}

}  // namespace graph3d
