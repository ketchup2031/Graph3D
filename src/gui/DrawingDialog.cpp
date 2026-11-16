#include "gui/DrawingDialog.h"

#include "drawing/DrawingSheet.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include <exception>
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

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().mid());
        const QSizeF size = QSizeF(drawing::sheetWidth, drawing::sheetHeight)
            .scaled(QSizeF(width() - 24, height() - 24), Qt::KeepAspectRatio);
        const QRectF paper((width() - size.width()) / 2, (height() - size.height()) / 2,
                           size.width(), size.height());
        drawing::paintSheet(painter, paper, views, options);
    }
};

DrawingDialog::DrawingDialog(drawing::Projections projections, const QString& object, int id, QWidget* parent)
    : QDialog(parent), preview_(new Preview(std::move(projections), this))
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
    auto* dimensions = new QCheckBox(tr("Overall dimensions"), this);
    hidden->setObjectName(QStringLiteral("drawingHidden"));
    dimensions->setObjectName(QStringLiteral("drawingDimensions"));
    hidden->setChecked(preview_->options.hiddenLines);
    dimensions->setChecked(preview_->options.dimensions);
    options->addWidget(hidden);
    options->addWidget(dimensions);
    options->addStretch();
    layout->addLayout(options);
    layout->addWidget(preview_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* pdf = buttons->addButton(tr("Save PDF…"), QDialogButtonBox::ActionRole);
    pdf->setObjectName(QStringLiteral("drawingSavePdf"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(pdf, &QPushButton::clicked, this, &DrawingDialog::exportPdf);
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
        drawing::savePdf(path, preview_->views, preview_->options);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Save drawing"), QString::fromUtf8(error.what()));
    }
}

}  // namespace graph3d
