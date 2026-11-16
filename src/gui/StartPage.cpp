#include "gui/StartPage.h"

#include "drawing/Projection.h"
#include "gui/UserPreferences.h"
#include "io/ProjectIO.h"

#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <exception>
#include <optional>

namespace graph3d {
namespace {

struct Example {
    const char* path;
    const char* name;
    const char* description;
};

constexpr std::array examples{
    Example{":/examples/flange.graph3d", QT_TRANSLATE_NOOP("graph3d::StartPage", "Mounting flange"),
            QT_TRANSLATE_NOOP("graph3d::StartPage", "A circular plate with a central bore and four mounting holes.")},
    Example{":/examples/stepped-bushing.graph3d", QT_TRANSLATE_NOOP("graph3d::StartPage", "Stepped bushing"),
            QT_TRANSLATE_NOOP("graph3d::StartPage", "A turned part built from a revolved profile.")},
    Example{":/examples/angle-bracket.graph3d", QT_TRANSLATE_NOOP("graph3d::StartPage", "Angle bracket"),
            QT_TRANSLATE_NOOP("graph3d::StartPage", "A support bracket made from a sketch and solid operations.")},
};

constexpr auto recentKey = "recentProjects";
constexpr int maxRecents = 8;

QLabel* label(const QString& text, const char* name, QWidget* parent)
{
    auto* item = new QLabel(text, parent);
    item->setObjectName(QString::fromLatin1(name));
    item->setTextFormat(Qt::PlainText);
    item->setWordWrap(true);
    return item;
}

enum class CommandIcon { Part, Sketch, Cube, Drawing };

QIcon commandIcon(CommandIcon kind, bool dark)
{
    QPixmap tile(52, 52);
    tile.setDevicePixelRatio(2);
    tile.fill(Qt::transparent);
    QPainter painter(&tile);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(dark ? "#90caff" : "#2667a3"), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(QColor(dark ? "#2a4663" : "#e2efff"));
    switch (kind) {
    case CommandIcon::Part:
        painter.drawPolygon(QPolygonF{{5, 3}, {15, 3}, {21, 9}, {21, 23}, {5, 23}});
        painter.drawPolyline(QPolygonF{{15, 3}, {15, 9}, {21, 9}});
        painter.drawLine(QPointF(9, 16), QPointF(17, 16));
        painter.drawLine(QPointF(13, 12), QPointF(13, 20));
        break;
    case CommandIcon::Sketch:
        painter.drawRoundedRect(QRectF(3, 3, 20, 20), 2, 2);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(QPolygonF{{7, 18}, {11, 8}, {19, 11}, {18, 18}, {7, 18}});
        break;
    case CommandIcon::Drawing:
        painter.drawRect(QRectF(3, 5, 20, 16));
        painter.drawRect(QRectF(6, 8, 14, 10));
        painter.drawLine(QPointF(13, 15), QPointF(20, 15));
        break;
    case CommandIcon::Cube:
        painter.drawPolygon(QPolygonF{{13, 2}, {23, 8}, {23, 18}, {13, 24}, {3, 18}, {3, 8}});
        painter.drawPolyline(QPolygonF{{3, 8}, {13, 14}, {23, 8}});
        painter.drawLine(QPointF(13, 14), QPointF(13, 24));
        break;
    }
    painter.end();
    return QIcon(tile);
}

void drawView(QPainter& painter, const drawing::View& view, const QRectF& area, bool dark)
{
    const auto scale = std::min(area.width() / view.width, area.height() / view.height);
    const auto paintLines = [&](const drawing::Linework& lines) {
        for (const auto& line : lines) {
            QPolygonF points;
            points.reserve(static_cast<qsizetype>(line.size()));
            for (const auto& point : line) {
                points.append(area.center() + QPointF(point.x * scale, -point.y * scale));
            }
            painter.drawPolyline(points);
        }
    };
    painter.setPen(QPen(QColor(dark ? "#537999" : "#adc5e0"), 0.85, Qt::DashLine));
    paintLines(view.hidden);
    painter.setPen(QPen(QColor(dark ? "#9acdf3" : "#2c608f"), 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    paintLines(view.visible);
}

}  // namespace

class ExamplePreview final : public QWidget {
public:
    explicit ExamplePreview(QWidget* parent) : QWidget(parent)
    {
        setMinimumHeight(166);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setDarkTheme(bool dark)
    {
        dark_ = dark;
        update();
    }

    void setExample(int index)
    {
        index_ = index;
        if (!loaded_[index]) {
            loaded_[index] = true;
            try {
                const auto document = io::openProject(QString::fromLatin1(examples[index].path));
                for (const auto& object : document.objects()) {
                    if (object.visible && object.status == document::Status::Ready
                        && document::isBody(object.parameters)) {
                        projections_[index] = drawing::project(object.shape);
                        break;
                    }
                }
            } catch (const std::exception&) {
                // Opening the example still reports a detailed error if its file is unreadable.
            }
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF paper = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath clip;
        clip.addRoundedRect(paper, 8, 8);
        painter.fillPath(clip, QColor(dark_ ? "#17232f" : "#f2f7fd"));
        painter.save();
        painter.setClipPath(clip);
        painter.setPen(QColor(dark_ ? "#263b4e" : "#e1ecf7"));
        for (int x = 12; x < width(); x += 16) {
            painter.drawLine(x, 0, x, height());
        }
        for (int y = 12; y < height(); y += 16) {
            painter.drawLine(0, y, width(), y);
        }
        const auto& views = projections_[index_];
        if (views) {
            const qreal mid = width() / 2.0;
            const QRectF front(22, 27, mid - 44, height() - 59);
            const QRectF top(mid + 22, 27, mid - 44, height() - 59);
            drawView(painter, views->front, front, dark_);
            drawView(painter, views->top, top, dark_);
            painter.setPen(QColor(dark_ ? "#b4c8da" : "#4d6781"));
            painter.drawText(QRectF(0, height() - 27, mid, 20), Qt::AlignCenter, tr("Front"));
            painter.drawText(QRectF(mid, height() - 27, mid, 20), Qt::AlignCenter, tr("Top"));
        }
        painter.restore();
        painter.setPen(QColor(dark_ ? "#39526a" : "#d9e5f2"));
        painter.drawPath(clip);
    }

private:
    std::array<bool, examples.size()> loaded_{};
    std::array<std::optional<drawing::Projections>, examples.size()> projections_{};
    int index_ = 0;
    bool dark_ = false;
};

void StartPage::setDarkTheme(bool dark)
{
    auto style = QStringLiteral(R"(
        #startPage, #startScroll, #startCanvas { background: #f1f4f8; }
        #startPage QLabel { background: transparent; color: #26394e; }
        #startPage QLabel#subtitle, #startPage QLabel#detail { color: #465c73; font-size: 13px; }
        #startPage QLabel#brand { color: #14263d; font-size: 36px; font-weight: 700; }
        #startPage QLabel#section { color: #162b43; font-size: 21px; font-weight: 700; }
        #startPage QLabel#exampleName { color: #162b43; font-size: 17px; font-weight: 650; }
        #startPage QLabel#tip { color: #20364f; font-weight: 600; font-size: 13px; }
        #startPage QFrame[panel="true"] { background: #ffffff; border: 1px solid #dbe3ee; border-radius: 12px; }
        #startPage QPushButton {
            background: #ffffff; color: #1e3853; border: 1px solid #d4deea;
            border-radius: 0; padding: 8px 11px; font-size: 13px;
        }
        #startPage QPushButton[dashed="true"] { border: 2px dashed #5b7fa6; padding: 7px 10px; }
        #startPage QPushButton:hover { background: #eaf3ff; border-color: #91b7e5; }
        #startPage QPushButton:pressed { background: #dceafe; }
        #startPage QPushButton:focus { border: 2px solid #286bb4; padding: 7px 10px; }
        #startPage QPushButton[primary="true"] { background: #205b9f; color: white; border-color: #205b9f; font-weight: 600; }
        #startPage QPushButton[primary="true"]:hover { background: #194b86; }
        #startPage QPushButton[primary="true"]:focus { border-color: #79abeb; }
        #startPage QPushButton[card="true"] { text-align: left; min-height: 55px; }
        #startPage QPushButton:disabled { color: #6e7884; background: #f5f6f8; }
    )");
    if (dark) {
        style += QStringLiteral(R"(
            #startPage, #startScroll, #startCanvas { background: #161d26; }
            #startPage QLabel { color: #d7e3ef; }
            #startPage QLabel#subtitle, #startPage QLabel#detail { color: #b4c5d6; }
            #startPage QLabel#brand, #startPage QLabel#section, #startPage QLabel#exampleName { color: #eff5fc; }
            #startPage QLabel#tip { color: #deebf8; }
            #startPage QFrame[panel="true"] { background: #202a36; border-color: #344557; }
            #startPage QPushButton { background: #253342; color: #e6eff8; border-color: #465e77; }
            #startPage QPushButton[dashed="true"] { border-color: #678caf; }
            #startPage QPushButton:hover { background: #2c4053; border-color: #8fb9df; }
            #startPage QPushButton:pressed { background: #344d65; }
            #startPage QPushButton:focus { border-color: #a1cbf1; }
            #startPage QPushButton[primary="true"] { background: #2c649a; border-color: #2c649a; }
            #startPage QPushButton[primary="true"]:hover { background: #3677b2; border-color: #3677b2; }
            #startPage QPushButton[primary="true"]:focus { border-color: #a1cbf1; }
            #startPage QPushButton:disabled { color: #9aa9ba; background: #222c37; border-color: #3b4b5c; }
        )");
    }
    setStyleSheet(style);
    preview_->setDarkTheme(dark);
    const std::array commands{
        std::pair{"startNewPart", CommandIcon::Part},
        std::pair{"startNewDrawing", CommandIcon::Drawing},
        std::pair{"startSketch", CommandIcon::Sketch},
        std::pair{"startCube", CommandIcon::Cube},
    };
    for (const auto& [name, icon] : commands) {
        findChild<QPushButton*>(QString::fromLatin1(name))->setIcon(commandIcon(icon, dark));
    }
}

StartPage::StartPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("startPage"));
    QFont font = this->font();
    font.setFamilies({QStringLiteral("Inter"), QStringLiteral("Segoe UI"), font.family()});
    setFont(font);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("startScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    root->addWidget(scroll);

    auto* canvas = new QWidget(scroll);
    canvas->setObjectName(QStringLiteral("startCanvas"));
    auto* layout = new QVBoxLayout(canvas);
    layout->setContentsMargins(24, 20, 24, 24);
    layout->setSpacing(18);
    auto* header = new QHBoxLayout;
    const auto icon = QApplication::windowIcon();
    if (!icon.isNull()) {
        auto* mark = new QLabel(canvas);
        mark->setPixmap(icon.pixmap(QSize(42, 42), devicePixelRatioF()));
        header->addWidget(mark, 0, Qt::AlignVCenter);
        header->addSpacing(4);
    }
    auto* title = new QVBoxLayout;
    title->setSpacing(0);
    title->addWidget(label(QStringLiteral("Graph3D"), "brand", canvas));
    title->addWidget(label(tr("Parametric CAD"), "subtitle", canvas));
    header->addLayout(title);
    header->addStretch();
    resume_ = new QPushButton(tr("Return to model"), canvas);
    resume_->setObjectName(QStringLiteral("startResume"));
    header->addWidget(resume_);
    connect(resume_, &QPushButton::clicked, this, &StartPage::resumeRequested);
    setWorkspaceAvailable(false);
    layout->addLayout(header);

    columns_ = new QGridLayout;
    columns_->setSpacing(14);
    layout->addLayout(columns_);
    layout->addStretch();
    const auto buildPanel = [canvas](const QString& heading, QWidget*& panel) {
        auto* frame = new QFrame(canvas);
        panel = frame;
        frame->setProperty("panel", true);
        auto* content = new QVBoxLayout(frame);
        content->setContentsMargins(15, 14, 15, 15);
        content->setSpacing(9);
        content->addWidget(label(heading, "section", panel));
        return content;
    };

    auto* recent = buildPanel(tr("Open"), recentPane_);
    auto* open = new QPushButton(tr("Browse files…"), recentPane_);
    open->setObjectName(QStringLiteral("startOpen"));
    open->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    open->setProperty("primary", true);
    connect(open, &QPushButton::clicked, this, &StartPage::openRequested);
    recent->addWidget(open);
    recent->addSpacing(7);
    recent->addWidget(label(tr("Recent projects"), "tip", recentPane_));
    recentLayout_ = new QVBoxLayout;
    recentLayout_->setSpacing(5);
    recent->addLayout(recentLayout_);
    recent->addStretch();

    auto* create = buildPanel(tr("Create"), createPane_);
    auto* commands = new QGridLayout;
    const auto addCommand = [&](const QString& title, const char* name, auto signal, int row, int column) {
        auto* button = new QPushButton(title, createPane_);
        button->setObjectName(QString::fromLatin1(name));
        button->setProperty("card", true);
        button->setProperty("dashed", true);
        button->setIconSize(QSize(26, 26));
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        connect(button, &QPushButton::clicked, this, signal);
        commands->addWidget(button, row, column);
    };
    addCommand(tr("New part"), "startNewPart", &StartPage::newProjectRequested, 0, 0);
    addCommand(tr("New drawing"), "startNewDrawing", &StartPage::newDrawingRequested, 0, 1);
    addCommand(tr("XY sketch"), "startSketch", &StartPage::sketchRequested, 1, 0);
    addCommand(tr("Demo cube"), "startCube", &StartPage::cubeRequested, 1, 1);
    create->addLayout(commands);
    create->addSpacing(8);
    auto* exampleHeader = new QHBoxLayout;
    exampleHeader->addWidget(label(tr("Example projects"), "section", createPane_));
    exampleHeader->addStretch();
    const auto exampleNavigation = [&](const QString& text, const QString& accessibleName, const char* name, int direction) {
        auto* button = new QPushButton(text, createPane_);
        button->setObjectName(QString::fromLatin1(name));
        button->setAccessibleName(accessibleName);
        button->setToolTip(accessibleName);
        connect(button, &QPushButton::clicked, this, [this, direction] { showExample(exampleIndex_ + direction); });
        exampleHeader->addWidget(button);
    };
    exampleNavigation(QStringLiteral("‹"), tr("Previous example"), "startPreviousExample", -1);
    exampleNavigation(QStringLiteral("›"), tr("Next example"), "startNextExample", 1);
    create->addLayout(exampleHeader);
    preview_ = new ExamplePreview(createPane_);
    preview_->setObjectName(QStringLiteral("startPreview"));
    create->addWidget(preview_);
    exampleName_ = label({}, "exampleName", createPane_);
    exampleDescription_ = label({}, "detail", createPane_);
    create->addWidget(exampleName_);
    create->addWidget(exampleDescription_);
    auto* openExample = new QPushButton(tr("Open example"), createPane_);
    openExample->setObjectName(QStringLiteral("startOpenExample"));
    openExample->setProperty("dashed", true);
    create->addWidget(openExample, 0, Qt::AlignLeft);
    connect(openExample, &QPushButton::clicked, this, [this] {
        emit exampleRequested(QString::fromLatin1(examples[exampleIndex_].path));
    });

    auto* help = buildPanel(tr("Help"), helpPane_);
    auto* controls = new QPushButton(tr("Mouse controls…"), helpPane_);
    controls->setObjectName(QStringLiteral("startControls"));
    help->addWidget(controls);
    connect(controls, &QPushButton::clicked, this, &StartPage::controlsRequested);
    help->addSpacing(7);
    help->addWidget(label(tr("Sketch a profile"), "tip", helpPane_));
    help->addWidget(label(tr("Choose a plane. Draw a closed contour and select Finish."), "detail", helpPane_));
    help->addSpacing(5);
    help->addWidget(label(tr("Make a solid"), "tip", helpPane_));
    help->addWidget(label(tr("Select the sketch and use Extrude or Revolve in the Model menu."), "detail", helpPane_));
    help->addSpacing(5);
    help->addWidget(label(tr("Create a drawing"), "tip", helpPane_));
    help->addWidget(label(tr("Select a body and open the Drawing menu to preview an A4 sheet."), "detail", helpPane_));
    help->addStretch();
    arrangePanels(false);
    scroll->setWidget(canvas);
    refreshRecents();
    showExample(0);
}

void StartPage::arrangePanels(bool compact)
{
    for (auto* panel : {recentPane_, createPane_, helpPane_}) {
        columns_->removeWidget(panel);
    }
    columns_->setColumnStretch(0, compact ? 1 : 22);
    columns_->setColumnStretch(1, compact ? 0 : 56);
    columns_->setColumnStretch(2, compact ? 0 : 22);
    if (compact) {
        columns_->addWidget(createPane_, 0, 0);
        columns_->addWidget(recentPane_, 1, 0);
        columns_->addWidget(helpPane_, 2, 0);
    } else {
        columns_->addWidget(recentPane_, 0, 0);
        columns_->addWidget(createPane_, 0, 1);
        columns_->addWidget(helpPane_, 0, 2);
    }
    compact_ = compact;
}

void StartPage::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    const bool compact = width() < 850;
    if (compact != compact_) {
        arrangePanels(compact);
    }
}

void StartPage::showExample(int index)
{
    const auto count = static_cast<int>(examples.size());
    exampleIndex_ = (index + count) % count;
    const auto& example = examples[exampleIndex_];
    exampleName_->setText(tr(example.name));
    exampleDescription_->setText(tr(example.description));
    preview_->setExample(exampleIndex_);
}

void StartPage::setWorkspaceAvailable(bool available)
{
    resume_->setVisible(available);
}

void StartPage::rememberProject(const QString& path)
{
    const QFileInfo file(path);
    const auto canonical = file.canonicalFilePath();
    const auto absolute = canonical.isEmpty() ? file.absoluteFilePath() : canonical;
    auto settings = userSettings();
    auto paths = settings.value(QLatin1String(recentKey)).toStringList();
    paths.removeAll(absolute);
    paths.prepend(absolute);
    settings.setValue(QLatin1String(recentKey), paths.mid(0, maxRecents));
    settings.sync();
    refreshRecents();
}

void StartPage::refreshRecents()
{
    while (auto* item = recentLayout_->takeAt(0)) {
        item->widget()->hide();
        item->widget()->deleteLater();
        delete item;
    }
    const auto paths = userSettings().value(QLatin1String(recentKey)).toStringList().mid(0, maxRecents);
    if (paths.isEmpty()) {
        recentLayout_->addWidget(label(tr("Saved and opened projects will appear here."), "detail", recentPane_));
    }
    for (const auto& path : paths) {
        const QFileInfo file(path);
        const auto title = file.fileName();
        auto* button = new QPushButton(recentPane_);
        button->setObjectName(QStringLiteral("startRecent"));
        button->setAccessibleName(title);
        button->setProperty("projectPath", path);
        button->setProperty("card", true);
        button->setMinimumHeight(44);
        button->setToolTip(QDir::toNativeSeparators(path));
        button->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        button->setText(fontMetrics().elidedText(title, Qt::ElideMiddle, 135));
        if (!file.isFile()) {
            button->setEnabled(false);
            button->setToolTip(tr("File not found: %1").arg(QDir::toNativeSeparators(path)));
        }
        connect(button, &QPushButton::clicked, this, [this, path] { emit recentRequested(path); });
        recentLayout_->addWidget(button);
    }
}

}  // namespace graph3d
