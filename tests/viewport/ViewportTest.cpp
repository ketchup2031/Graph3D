#include "gui/MainWindow.h"
#include "gui/InterfaceLanguage.h"
#include "gui/SketchEditor.h"
#include "io/ProjectIO.h"
#include "io/DraftIO.h"
#include "viewport/Viewport.h"

#include <QAction>
#include <QListWidget>
#include <QAbstractButton>
#include <array>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <cmath>
#include <numbers>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>

#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QTest>
#include <QTemporaryDir>
#include <QTabBar>
#include <QTimer>
#include <QTreeWidget>
#include <QWheelEvent>

namespace {

void drag(QWidget* widget, Qt::MouseButton button, QPoint from, QPoint to)
{
    QTest::mousePress(widget, button, Qt::NoModifier, from);
    QMouseEvent move(QEvent::MouseMove, to, widget->mapToGlobal(to), Qt::NoButton,
                     button, Qt::NoModifier);
    QApplication::sendEvent(widget, &move);
    QTest::mouseRelease(widget, button, Qt::NoModifier, to);
}

QImage frame(graph3d::Viewport* viewport)
{
    return viewport->renderSnapshot();
}

}  // namespace

class ViewportTest : public QObject {
    Q_OBJECT

private slots:
    void draftTabsPreserveEditsAcrossSheetUndo()
    {
        graph3d::MainWindow main;
        main.show();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        main.findChild<QPushButton*>("startNewDrawing")->click();
        auto* draft = main.findChild<QMainWindow*>("draftWindow");
        QVERIFY(draft && QTest::qWaitForWindowExposed(draft));
        auto* tabs = draft->findChild<QTabBar*>("draftSheetTabs");
        auto* canvas = draft->findChild<QWidget*>("draftCanvas");
        QVERIFY(tabs && canvas);
        const auto action = [draft](const char* name) { return draft->findChild<QAction*>(name); };
        const auto click = [canvas](double x, double y) {
            const double scale = std::min((canvas->width() - 32) / 297.0, (canvas->height() - 32) / 210.0);
            const QPointF origin((canvas->width() - 297 * scale) / 2, (canvas->height() - 210 * scale) / 2);
            QTest::mouseClick(canvas, Qt::LeftButton, {}, (origin + QPointF(x, y) * scale).toPoint());
        };
        action("draftLine")->trigger();
        click(60, 55);
        click(150, 55);
        action("addDraftSheet")->trigger();
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->currentIndex(), 1);
        action("draftLine")->trigger();
        click(60, 85);
        click(150, 85);
        tabs->setCurrentIndex(0);
        action("undoDraft")->trigger();
        QCOMPARE(tabs->currentIndex(), 1);
        action("redoDraft")->trigger();
        tabs->setCurrentIndex(0);
        action("removeDraftSheet")->trigger();
        QCOMPARE(tabs->count(), 1);
        action("undoDraft")->trigger();
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->currentIndex(), 0);
        action("draftSelect")->trigger();
        click(95, 55);
        QVERIFY(action("draftItemProperties")->isEnabled());
        tabs->setCurrentIndex(1);
        click(95, 85);
        QVERIFY(action("draftItemProperties")->isEnabled());

        action("undoDraft")->trigger();
        action("undoDraft")->trigger();
        action("undoDraft")->trigger();
        QCOMPARE(tabs->count(), 1);
        QVERIFY(!draft->isWindowModified());
        QVERIFY(draft->close());
    }

    void initTestCase()
    {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        QStandardPaths::setTestModeEnabled(true);
        QApplication::setApplicationName(QStringLiteral("Graph3D-test"));
    }

    void russianInterfacePreference()
    {
        QVERIFY(!graph3d::russianLanguagePreferred());
        QVERIFY(!graph3d::russianInterface());
        const auto dismissNotice = [] {
            QTimer::singleShot(0, [] {
                if (auto* notice = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    notice->accept();
                }
            });
        };
        {
            graph3d::MainWindow english;
            auto* button = english.findChild<QPushButton*>("startNewDrawing");
            auto* englishChoice = english.findChild<QAction*>("languageEnglish");
            auto* russianChoice = english.findChild<QAction*>("languageRussian");
            QVERIFY(button && englishChoice && russianChoice);
            QCOMPARE(button->text(), QStringLiteral("New drawing"));
            QVERIFY(englishChoice->isChecked());
            dismissNotice();
            russianChoice->trigger();
            QVERIFY(graph3d::russianLanguagePreferred());
            QCOMPARE(button->text(), QStringLiteral("New drawing"));
        }
        {
            const graph3d::InterfaceLanguage language(graph3d::russianLanguagePreferred());
            QVERIFY(graph3d::russianInterface());
            graph3d::MainWindow russian;
            auto* button = russian.findChild<QPushButton*>("startNewDrawing");
            QVERIFY(button);
            QCOMPARE(button->text(), QStringLiteral("Новый чертёж"));
            QCOMPARE(russian.findChild<QAction*>("newDrawing")->text(), QStringLiteral("Новый 2D-чертёж…"));
            QCOMPARE(russian.findChild<QAction*>("darkTheme")->text(), QStringLiteral("Тёмная тема"));
            QCOMPARE(graph3d::localizedDiagnostic("Move the section plane inside the body bounds."),
                     QStringLiteral("Переместите секущую плоскость внутрь габаритов тела."));
            button->click();
            auto* draft = russian.findChild<QMainWindow*>("draftWindow");
            QVERIFY(draft);
            QCOMPARE(draft->findChild<QAction*>("draftRect")->text(), QStringLiteral("Прямоугольник"));
            QDialogButtonBox dialogButtons(QDialogButtonBox::Cancel);
            auto* cancel = dialogButtons.button(QDialogButtonBox::Cancel);
            QVERIFY(cancel);
            QCOMPARE(QString(cancel->text()).remove(QLatin1Char('&')), QStringLiteral("Отмена"));
            QVERIFY(draft->close());
            auto* englishChoice = russian.findChild<QAction*>("languageEnglish");
            QVERIFY(englishChoice);
            dismissNotice();
            englishChoice->trigger();
            QVERIFY(!graph3d::russianLanguagePreferred());
        }
        QVERIFY(!graph3d::russianInterface());
        graph3d::MainWindow restored;
        QCOMPARE(restored.findChild<QPushButton*>("startNewDrawing")->text(), QStringLiteral("New drawing"));
    }

    void startPageWorkflow()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        graph3d::MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* home = window.findChild<QWidget*>("startPage");
        auto* viewport = window.findChild<graph3d::Viewport*>();
        auto* tree = window.findChild<QTreeWidget*>("modelTree");
        const auto button = [&window](const char* name) { return window.findChild<QPushButton*>(name); };
        const auto action = [&window](const char* name) { return window.findChild<QAction*>(name); };
        QVERIFY(home && viewport && tree);
        QVERIFY(home->isVisible() && !viewport->isVisible() && !tree->isVisible());
        QVERIFY(!button("startResume")->isVisible());
        QVERIFY(action("openProject")->isEnabled());
        QVERIFY(!action("saveProject")->isEnabled());
        auto* preview = home->findChild<QWidget*>("startPreview");
        QVERIFY(preview && preview->isVisible() && !preview->grab().isNull());

        button("startSketch")->click();
        auto* editor = window.findChild<graph3d::SketchEditor*>();
        QVERIFY(editor && editor->isVisible() && !home->isVisible());
        editor->findChild<QPushButton*>("sketchCancel")->click();
        QVERIFY(viewport->isVisible() && tree->isVisible());
        QCOMPARE(tree->topLevelItemCount(), 0);
        QVERIFY(!window.isWindowModified());
        action("createCube")->trigger();
        action("showStartPage")->trigger();
        QVERIFY(home->isVisible() && !tree->isVisible());
        QVERIFY(button("startResume")->isVisible() && window.isWindowModified());
        QTimer::singleShot(0, &window, [] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                dialog->button(QMessageBox::Cancel)->click();
            }
        });
        button("startNewPart")->click();
        QVERIFY(home->isVisible() && window.isWindowModified());
        button("startResume")->click();
        QCOMPARE(tree->topLevelItemCount(), 1);

        const auto path = folder.filePath(QStringLiteral("welcome-project.graph3d"));
        QTimer::singleShot(0, &window, [&] {
            if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                dialog->selectFile(path);
                static_cast<QDialog*>(dialog)->accept();
            }
        });
        action("saveProject")->trigger();
        QVERIFY(QFileInfo::exists(path) && !window.isWindowModified());
        action("showStartPage")->trigger();
        auto* recent = button("startRecent");
        QVERIFY(recent && recent->isVisible());
        QCOMPARE(recent->accessibleName(), QStringLiteral("welcome-project.graph3d"));
        recent->click();
        QCOMPARE(tree->topLevelItemCount(), 1);
        QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("Cube 1"));

        action("showStartPage")->trigger();
        auto* example = home->findChild<QLabel*>("exampleName");
        QVERIFY(example);
        QCOMPARE(example->text(), QStringLiteral("Mounting flange"));
        button("startNextExample")->click();
        QCOMPARE(example->text(), QStringLiteral("Stepped bushing"));
        button("startOpenExample")->click();
        QVERIFY(viewport->isVisible() && !window.isWindowModified());
        QVERIFY(tree->topLevelItemCount() > 0);
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("stepped-bushing.graph3d")));
        QCOMPARE(window.windowTitle(), QStringLiteral("Graph3D[*]"));

        graph3d::MainWindow restarted;
        auto* saved = restarted.findChild<QPushButton*>("startRecent");
        QVERIFY(saved);
        QCOMPARE(saved->accessibleName(), QStringLiteral("welcome-project.graph3d"));
    }

    void standaloneDrawingWorkflow()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        graph3d::MainWindow main;
        main.show();
        main.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        auto* create = main.findChild<QPushButton*>("startNewDrawing");
        QVERIFY(create && create->isVisible());
        create->click();
        auto* window = main.findChild<QMainWindow*>("draftWindow");
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* canvas = window->findChild<QWidget*>("draftCanvas");
        QVERIFY(canvas && canvas->isVisible());
        QVERIFY(!main.findChild<graph3d::Viewport*>()->isVisible());
        const auto action = [window](const char* name) { return window->findChild<QAction*>(name); };
        const auto click = [canvas](double x, double y) {
            const double scale = std::min((canvas->width() - 32) / 297.0, (canvas->height() - 32) / 210.0);
            const QPointF origin((canvas->width() - 297 * scale) / 2, (canvas->height() - 210 * scale) / 2);
            QTest::mouseClick(canvas, Qt::LeftButton, {}, (origin + QPointF(x, y) * scale).toPoint());
        };
        action("draftRect")->trigger();
        click(60, 55);
        click(160, 105);
        action("draftHorizontal")->trigger();
        click(60, 55);
        click(160, 55);
        click(110, 40);
        QVERIFY(window->isWindowModified());
        QVERIFY(action("undoDraft")->isEnabled());
        action("undoDraft")->trigger();
        QVERIFY(action("redoDraft")->isEnabled());
        action("redoDraft")->trigger();

        action("draftLine")->trigger();
        auto* length = window->findChild<QDoubleSpinBox*>("lineLength");
        auto* angle = window->findChild<QDoubleSpinBox*>("lineAngle");
        auto* addLine = window->findChild<QPushButton*>("addLine");
        QVERIFY(length && angle && addLine);
        QVERIFY(length->isVisible() && !length->isEnabled());
        click(60, 130);
        QVERIFY(length->isEnabled());
        length->setValue(1000);
        angle->setValue(30);
        addLine->click();
        QVERIFY(length->isEnabled());
        length->setValue(25);
        addLine->click();
        QVERIFY(!length->isEnabled());

        QTimer::singleShot(0, window, [] {
            if (auto* sheet = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                sheet->findChild<QLineEdit*>("draftTitle")->setText(QStringLiteral("Пластина"));
                sheet->findChild<QComboBox*>("draftPaper")->setCurrentIndex(1);
                sheet->accept();
            }
        });
        action("draftSheetSettings")->trigger();
        const auto selectFile = [window](const QString& path) {
            QTimer::singleShot(0, window, [path] {
                if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                    dialog->selectFile(path);
                    static_cast<QDialog*>(dialog)->accept();
                }
            });
        };
        const auto path = folder.filePath("plate.g3draw");
        selectFile(path);
        action("saveDraft")->trigger();
        QVERIFY(!window->isWindowModified());
        QFile native(path);
        QVERIFY(native.open(QIODevice::ReadOnly));
        const auto contents = QJsonDocument::fromJson(native.readAll()).object();
        native.close();
        const auto items = contents.value("items").toArray();
        QCOMPARE(items.size(), 3);
        const auto line = items.at(2).toObject();
        QCOMPARE(line.value("type").toString(), QStringLiteral("line"));
        const auto endpoint = line.value("b").toArray();
        QVERIFY(std::abs(endpoint.at(0).toDouble() - (60 + 25 * std::cos(std::numbers::pi / 6))) < 1.e-9);
        QVERIFY(std::abs(endpoint.at(1).toDouble() - 117.5) < 1.e-9);
        QCOMPARE(contents.value("title").toString(), QStringLiteral("Пластина"));
        QCOMPARE(contents.value("paper").toString(), QStringLiteral("A3"));
        QCOMPARE(contents.value("version").toInt(), 2);
        const auto rectangle = items.at(0).toObject();
        const auto dimension = items.at(1).toObject();
        QCOMPARE(dimension.value("anchorA").toObject().value("item"), rectangle.value("id"));
        QCOMPARE(dimension.value("anchorB").toObject().value("item"), rectangle.value("id"));

        action("draftSelect")->trigger();
        const auto clickA3 = [canvas](double x, double y) {
            QApplication::processEvents();
            const double scale = std::min((canvas->width() - 32) / 420.0, (canvas->height() - 32) / 297.0);
            const QPointF origin((canvas->width() - 420 * scale) / 2, (canvas->height() - 297 * scale) / 2);
            QTest::mouseClick(canvas, Qt::LeftButton, {}, (origin + QPointF(x, y) * scale).toPoint());
        };
        clickA3(160, 80);
        QVERIFY(action("draftItemProperties")->isEnabled());
        QTimer::singleShot(0, window, [] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog) {
                const auto fields = dialog->findChildren<QDoubleSpinBox*>();
                if (fields.size() == 4) {
                    fields[2]->setValue(180);
                    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
                } else {
                    dialog->reject();
                }
            }
        });
        action("draftItemProperties")->trigger();
        clickA3(180, 80);
        QVERIFY(action("removeDraftItem")->isEnabled());
        action("removeDraftItem")->trigger();
        action("saveDraft")->trigger();
        const auto readSaved = [&native] {
            if (!native.open(QIODevice::ReadOnly)) {
                return QJsonObject{};
            }
            const auto result = QJsonDocument::fromJson(native.readAll()).object();
            native.close();
            return result;
        };
        const auto detached = readSaved();
        QCOMPARE(detached.value("version").toInt(), 1);
        const auto fixed = detached.value("items").toArray().at(0).toObject();
        QCOMPARE(fixed.value("type").toString(), QStringLiteral("horizontal"));
        QCOMPARE(fixed.value("b").toArray().at(0).toDouble(), 180.0);
        QVERIFY(!fixed.contains("anchorB"));

        action("undoDraft")->trigger();
        action("saveDraft")->trigger();
        const auto reattached = readSaved();
        QCOMPARE(reattached.value("version").toInt(), 2);
        QCOMPARE(reattached.value("items").toArray().at(1).toObject().value("anchorB"), dimension.value("anchorB"));
        action("redoDraft")->trigger();
        action("undoDraft")->trigger();
        QVERIFY(!window->isWindowModified());
        const auto pdfPath = folder.filePath("plate.pdf");
        selectFile(pdfPath);
        action("exportDraftPdf")->trigger();
        QFile pdf(pdfPath);
        QVERIFY(pdf.open(QIODevice::ReadOnly));
        QCOMPARE(pdf.read(5), QByteArrayLiteral("%PDF-"));

        QVERIFY(window->close());
        QTRY_VERIFY(!main.findChild<QMainWindow*>("draftWindow"));
        auto* recent = main.findChild<QPushButton*>("startRecent");
        QVERIFY(recent && recent->accessibleName() == QStringLiteral("plate.g3draw"));
        recent->click();
        auto* restored = main.findChild<QMainWindow*>("draftWindow");
        QVERIFY(restored && restored->windowTitle().startsWith(QStringLiteral("plate.g3draw")));
        QVERIFY(!restored->isWindowModified());
    }

    void advancedDraftToolsWorkflow()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        graph3d::MainWindow main;
        main.show();
        main.findChild<QPushButton*>("startNewDrawing")->click();
        auto* draft = main.findChild<QMainWindow*>("draftWindow");
        QVERIFY(draft && QTest::qWaitForWindowExposed(draft));
        auto* canvas = draft->findChild<QWidget*>("draftCanvas");
        QVERIFY(canvas);
        const auto action = [draft](const char* name) { return draft->findChild<QAction*>(name); };
        const auto paperPoint = [canvas](QPointF point) {
            const double scale = std::min((canvas->width() - 32) / 297.0, (canvas->height() - 32) / 210.0);
            const QPointF origin((canvas->width() - 297 * scale) / 2, (canvas->height() - 210 * scale) / 2);
            return (origin + point * scale).toPoint();
        };
        const auto click = [&](double x, double y) {
            QTest::mouseClick(canvas, Qt::LeftButton, {}, paperPoint({x, y}));
        };
        action("draftArc")->trigger();
        click(60, 45);
        click(40, 75);
        click(60, 105);
        action("draftPolyline")->trigger();
        click(200, 45);
        click(200, 105);
        click(60, 105);
        QTest::keyClick(canvas, Qt::Key_Return);
        action("draftLine")->trigger();
        click(35, 45);
        click(200, 45);
        action("draftTrim")->trigger();
        click(45, 45);
        action("undoDraft")->trigger();
        action("redoDraft")->trigger();

        action("draftCircle")->trigger();
        click(90, 70);
        click(100, 70);
        click(150, 82);
        click(159, 82);
        action("draftDiameter")->trigger();
        click(100, 70);
        click(119, 62);
        action("draftRadius")->trigger();
        click(159, 82);
        click(178, 74);
        action("draftAligned")->trigger();
        click(130, 45);
        click(150, 82);
        click(132, 67);
        action("draftAngular")->trigger();
        click(200, 45);
        click(60, 45);
        click(200, 105);
        click(179, 68);
        action("draftSelect")->trigger();
        click(90, 60);
        QVERIFY(action("draftItemProperties")->isEnabled());
        QTimer::singleShot(0, [] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                const auto fields = dialog->findChildren<QDoubleSpinBox*>();
                if (fields.size() == 4) {
                    fields.at(2)->setValue(102);
                    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
                } else {
                    dialog->reject();
                }
            }
        });
        action("draftItemProperties")->trigger();
        action("undoDraft")->trigger();
        action("redoDraft")->trigger();
        action("draftHatch")->trigger();
        click(75, 66);

        action("draftLine")->trigger();
        click(210, 140);
        click(225, 140);
        click(246, 121);
        click(246, 150);
        action("draftExtend")->trigger();
        click(224, 140);
        QTimer::singleShot(0, [] {
            if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                dialog->setDoubleValue(5);
                dialog->accept();
            }
        });
        action("draftOffset")->trigger();
        click(220, 140);

        const auto path = folder.filePath("rounded-plate.g3draw");
        QTimer::singleShot(0, [path] {
            if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                dialog->selectFile(path);
                static_cast<QDialog*>(dialog)->accept();
            }
        });
        action("saveDraft")->trigger();
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QCOMPARE(root.value("version").toInt(), 4);
        const auto items = root.value("items").toArray();
        QCOMPARE(items.size(), 13);
        QCOMPARE(items.at(0).toObject().value("type").toString(), QStringLiteral("arc"));
        QCOMPARE(items.at(1).toObject().value("points").toArray().size(), 3);
        QCOMPARE(items.at(2).toObject().value("a").toArray().at(0).toDouble(), 60.0);
        QCOMPARE(items.at(5).toObject().value("anchorB").toObject().value("point").toString(), QStringLiteral("curve"));
        QCOMPARE(items.at(7).toObject().value("anchorA").toObject().value("parameter").toDouble(), .5);
        QCOMPARE(items.at(9).toObject().value("loops").toArray().size(), 3);
        QCOMPARE(items.at(10).toObject().value("b").toArray().at(0).toDouble(), 246.0);
        QCOMPARE(items.at(12).toObject().value("a").toArray().at(1).toDouble(), 135.0);
        const auto restored = graph3d::io::openDraft(path);
        const graph3d::drawing::DraftGeometry geometry(restored);
        QCOMPARE(graph3d::drawing::draftDimensionText(geometry.resolve(restored.items.at(5))), QStringLiteral("⌀24"));
    }

    void linkedModelDrawingWorkflow()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const auto modelPath = folder.filePath("flange.graph3d");
        const auto drawingPath = folder.filePath("flange.g3draw");
        const auto newModelPath = folder.filePath("flange-moved.graph3d");
        const auto pdfPath = folder.filePath("flange.pdf");
        using namespace graph3d;
        document::Document model;
        const auto profile = model.addSketch({geometry::Plane::XY, {geometry::Circle{{0, 0}, 40}}});
        const auto flange = model.addExtrude(profile, 15);
        const auto hole = model.addHole(flange, {{0, 0, 15}, {0, 0, 1}}, {10, 15, true});
        io::saveProject(modelPath, model);

        MainWindow main;
        main.show();
        main.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        main.findChild<QPushButton*>("startNewDrawing")->click();
        auto* draft = main.findChild<QMainWindow*>("draftWindow");
        QVERIFY(draft && QTest::qWaitForWindowExposed(draft));
        const auto action = [&draft](const char* name) { return draft->findChild<QAction*>(name); };
        const auto chooseFile = [](const QString& path, bool chooseBody = false) {
            QTimer::singleShot(0, [path, chooseBody] {
                if (auto* chooser = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                    chooser->selectFile(path);
                    static_cast<QDialog*>(chooser)->accept();
                    if (chooseBody) {
                        QTimer::singleShot(0, [] {
                            if (auto* body = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                                body->accept();
                            }
                        });
                    }
                }
            });
        };
        auto* canvas = draft->findChild<QWidget*>("draftCanvas");
        QVERIFY(canvas);
        action("draftText")->trigger();
        QTimer::singleShot(0, [] {
            if (auto* note = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                note->setTextValue(QStringLiteral("Keep this note"));
                note->accept();
            }
        });
        const double scale = std::min((canvas->width() - 32) / 297.0, (canvas->height() - 32) / 210.0);
        const QPointF origin((canvas->width() - 297 * scale) / 2, (canvas->height() - 210 * scale) / 2);
        QTest::mouseClick(canvas, Qt::LeftButton, {}, (origin + QPointF(35, 156) * scale).toPoint());

        chooseFile(modelPath, true);
        action("linkedViewsSource")->trigger();
        auto* status = draft->findChild<QLabel*>("linkedViewsStatus");
        QVERIFY(status && status->isVisible());
        QVERIFY(status->text().contains(QStringLiteral("body #3")));
        QTimer::singleShot(0, [] {
            if (auto* settings = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                settings->findChild<QDoubleSpinBox*>("linkedViewX0")->setValue(199.5);
                settings->findChild<QDoubleSpinBox*>("linkedViewScale0")->setValue(0.75);
                settings->findChild<QComboBox*>("linkedSectionPlane")->setCurrentIndex(1);
                settings->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
                if (settings->isVisible()) {
                    settings->reject();
                }
            }
        });
        action("linkedViewsSettings")->trigger();
        action("placeSectionLine")->trigger();
        const double pickScale = std::min((canvas->width() - 32) / 297.0, (canvas->height() - 32) / 210.0);
        const QPointF pickOrigin((canvas->width() - 297 * pickScale) / 2, (canvas->height() - 210 * pickScale) / 2);
        QTest::mouseClick(canvas, Qt::LeftButton, {}, (pickOrigin + QPointF(211, 122) * pickScale).toPoint());
        chooseFile(drawingPath);
        action("saveDraft")->trigger();
        const auto readSaved = [&] {
            QFile saved(drawingPath);
            return saved.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(saved.readAll()).object() : QJsonObject{};
        };
        const auto saved = readSaved();
        const auto before = saved.value("modelViews").toObject();
        QCOMPARE(saved.value("version").toInt(), 3);
        QCOMPARE(before.value("body").toInt(), hole);
        QCOMPARE(before.value("section").toObject().value("plane").toString(), QStringLiteral("XZ"));
        QVERIFY(std::abs(before.value("section").toObject().value("offset").toDouble() - 8) < .5);
        const auto hasRadius = [](const QJsonObject& views, double radius) {
            const auto circles = views.value("top").toObject().value("circles").toArray();
            return std::ranges::any_of(circles, [radius](const auto& circle) {
                return std::abs(circle.toObject().value("radius").toDouble() - radius) < .001;
            });
        };
        QVERIFY(hasRadius(before, 5));
        QVERIFY(draft->close());
        QTRY_VERIFY(!main.findChild<QMainWindow*>("draftWindow"));

        QVERIFY(model.editHole(hole, {12, 15, true}));
        io::saveProject(modelPath, model);
        const auto recent = main.findChildren<QPushButton*>("startRecent");
        const auto button = std::ranges::find_if(recent, [](const auto* item) {
            return item->accessibleName() == "flange.g3draw";
        });
        QVERIFY(button != recent.end());
        (*button)->click();
        draft = main.findChild<QMainWindow*>("draftWindow");
        QVERIFY(draft);
        status = draft->findChild<QLabel*>("linkedViewsStatus");
        QVERIFY(status->text().contains(QStringLiteral("stale views")));
        action("linkedViewsRefresh")->trigger();
        QVERIFY(!status->text().contains(QStringLiteral("stale views")));
        action("saveDraft")->trigger();
        const auto after = readSaved();
        const auto afterViews = after.value("modelViews").toObject();
        QVERIFY(hasRadius(afterViews, 6) && !hasRadius(afterViews, 5));
        QCOMPARE(afterViews.value("layout"), before.value("layout"));
        QCOMPARE(after.value("items"), saved.value("items"));
        QCOMPARE(after.value("items").toArray().at(0).toObject().value("text").toString(), QStringLiteral("Keep this note"));

        QVERIFY(model.editHole(hole, {200, 15, true}));
        io::saveProject(modelPath, model);
        action("linkedViewsRefresh")->trigger();
        QVERIFY(status->text().contains(QStringLiteral("stale views")));
        QVERIFY(!draft->isWindowModified());
        QVERIFY(model.undo());
        io::saveProject(modelPath, model);
        QVERIFY(QFile::rename(modelPath, newModelPath));
        action("linkedViewsRefresh")->trigger();
        QVERIFY(status->text().contains(QStringLiteral("stale views")));
        QVERIFY(action("exportDraftPdf")->isEnabled());
        chooseFile(pdfPath);
        action("exportDraftPdf")->trigger();
        QFile pdf(pdfPath);
        QVERIFY(pdf.open(QIODevice::ReadOnly) && pdf.read(5) == "%PDF-");
        chooseFile(newModelPath, true);
        action("linkedViewsSource")->trigger();
        QVERIFY(status->text().contains(QStringLiteral("flange-moved.graph3d")));
        QVERIFY(!status->text().contains(QStringLiteral("stale views")));
        action("saveDraft")->trigger();
        const auto relinked = readSaved().value("modelViews").toObject();
        QCOMPARE(relinked.value("source").toString(), QStringLiteral("flange-moved.graph3d"));
        QCOMPARE(relinked.value("layout"), before.value("layout"));
        QVERIFY(hasRadius(relinked, 6));
    }

    void darkThemePreference()
    {
        const auto originalWindowColor = QApplication::palette().color(QPalette::Window);
        graph3d::MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* theme = window.findChild<QAction*>("darkTheme");
        auto* preview = window.findChild<QWidget*>("startPreview");
        QVERIFY(theme && preview);
        QVERIFY(!theme->isChecked());
        const auto lightPixel = preview->grab().toImage().pixelColor(15, 15);
        theme->trigger();
        QVERIFY(theme->isChecked());
        QVERIFY(QApplication::palette().color(QPalette::Window).lightness() < 100);
        QVERIFY(preview->grab().toImage().pixelColor(15, 15).lightness() < lightPixel.lightness());

        QVERIFY(window.initializeScene());
        auto* tree = window.findChild<QTreeWidget*>("modelTree");
        QVERIFY(tree && tree->isVisible());
        QTRY_VERIFY(tree->palette().color(QPalette::Base).lightness() < 100);
        QVERIFY(tree->palette().color(QPalette::Text).lightness() > 180);

        graph3d::MainWindow restarted;
        auto* savedTheme = restarted.findChild<QAction*>("darkTheme");
        QVERIFY(savedTheme && savedTheme->isChecked());
        savedTheme->trigger();
        QVERIFY(!savedTheme->isChecked());
        QCOMPARE(QApplication::palette().color(QPalette::Window), originalWindowColor);
    }

    void sceneAndCamera()
    {
        graph3d::MainWindow window;
        window.resize(730, 530);
        window.show();
        window.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* viewport = window.findChild<graph3d::Viewport*>();
        QVERIFY(viewport);
        QSignalSpy errors(viewport, &graph3d::Viewport::operationFailed);
        QVERIFY(window.initializeScene());

        auto action = [&window](const char* name) { return window.findChild<QAction*>(name); };
        auto message = [&window] { return window.statusBar()->currentMessage(); };
        const auto center = viewport->rect().center();
        const auto startFrame = frame(viewport);
        QVERIFY(!startFrame.isNull());
        QVERIFY(startFrame.pixelColor(startFrame.rect().center()) != startFrame.pixelColor(3, 3));

        QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(message(), QStringLiteral("Selected Cube 1"));
        QVERIFY(action("clearSelection"));
        action("clearSelection")->trigger();
        QCOMPARE(message(), QStringLiteral("No selection"));
        QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(4, 4));
        QCOMPARE(message(), QStringLiteral("No selection"));

        auto before = frame(viewport);
        drag(viewport, Qt::RightButton, center, center + QPoint(97, 48));
        QTRY_VERIFY(frame(viewport) != before);
        before = frame(viewport);
        drag(viewport, Qt::MiddleButton, center, center + QPoint(65, -16));
        QTRY_VERIFY(frame(viewport) != before);
        before = frame(viewport);
        QWheelEvent wheel(center, viewport->mapToGlobal(center), {}, QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(viewport, &wheel);
        QTRY_VERIFY(frame(viewport) != before);

        for (const char* name : {"viewFront", "viewTop", "viewRight", "viewIsometric", "fitScene"}) {
            QVERIFY(action(name));
            action(name)->trigger();
        }
        for (const char* name : {"showAxes", "showGrid"}) {
            QVERIFY(action(name));
            before = frame(viewport);
            action(name)->trigger();
            QVERIFY(!action(name)->isChecked());
            QTRY_VERIFY(frame(viewport) != before);
            action(name)->trigger();
            QVERIFY(action(name)->isChecked());
        }

        QVERIFY(action("newScene"));
        action("newScene")->trigger();
        QCOMPARE(message(), QStringLiteral("New scene"));
        for (const auto [name, expected] : {
                 std::pair{"createCube", "Created Cube 1"},
                 std::pair{"createCylinder", "Created Cylinder 2"},
                 std::pair{"createSphere", "Created Sphere 3"}}) {
            QVERIFY(action(name));
            action(name)->trigger();
            QCOMPARE(message(), QString::fromLatin1(expected));
        }
        action("viewTop")->trigger();
        action("fitScene")->trigger();
        QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(message(), QStringLiteral("Selected Cylinder 2"));
        QCOMPARE(errors.count(), 0);
    }

    void sketchLineByLengthAndAngle()
    {
        graph3d::SketchEditor editor;
        editor.resize(740, 520);
        editor.begin(graph3d::geometry::Plane::XY);
        editor.show();
        editor.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&editor));
        auto* canvas = editor.findChild<QWidget*>("sketchCanvas");
        auto* length = editor.findChild<QDoubleSpinBox*>("lineLength");
        auto* angle = editor.findChild<QDoubleSpinBox*>("lineAngle");
        QVERIFY(canvas && length && angle && !length->isEnabled());
        const auto center = QPoint(canvas->width() / 2, canvas->height() / 2);
        QTest::mouseClick(canvas, Qt::LeftButton, {}, center);
        QVERIFY(length->isEnabled());
        const QPoint above = center + QPoint(0, -60);
        QMouseEvent move(QEvent::MouseMove, above, canvas->mapToGlobal(above),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &move);
        QCOMPARE(length->value(), 20.0);
        QCOMPARE(angle->value(), 90.0);
        length->setValue(31.25);
        angle->setValue(-90);
        angle->setFocus();
        QTest::keyClick(angle, Qt::Key_Return);
        QCOMPARE(editor.sketch().entities.size(), std::size_t(1));
        const auto& line = std::get<graph3d::geometry::Line>(editor.sketch().entities.front());
        QCOMPARE(line.start.u, 0.0);
        QCOMPARE(line.start.v, 0.0);
        QCOMPARE(line.end.u, 0.0);
        QCOMPARE(line.end.v, -31.25);
        QVERIFY(!length->isEnabled());
        QTest::mouseClick(canvas, Qt::LeftButton, {}, center);
        QVERIFY(length->isEnabled());
        QTest::mouseClick(canvas, Qt::RightButton, {}, center);
        QVERIFY(!length->isEnabled());
        QCOMPARE(editor.sketch().entities.size(), std::size_t(1));
    }

    void sketchConstraintsInEditor()
    {
        using namespace graph3d::geometry;
        graph3d::SketchEditor editor;
        editor.resize(760, 620);
        editor.begin(Sketch{Plane::XY, {Rectangle{{0, 0}, {50, 30}}, Circle{{20, 15}, 5}}}, {});
        editor.show();
        QVERIFY(QTest::qWaitForWindowExposed(&editor));
        auto* canvas = editor.findChild<QWidget*>("sketchCanvas");
        auto* constraints = editor.findChild<QListWidget*>("sketchConstraints");
        auto* diagnostic = editor.findChild<QLabel*>("sketchConstraintDiagnostic");
        QVERIFY(canvas && constraints && diagnostic);
        const auto click = [canvas](int u, int v) {
            const QPoint origin(canvas->width() / 2, canvas->height() / 2);
            QTest::mouseClick(canvas, Qt::LeftButton, {}, origin + QPoint(3 * u, -3 * v));
        };
        const auto setValue = [](double value) {
            QTimer::singleShot(0, [value] {
                if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                    dialog->setDoubleValue(value);
                    dialog->accept();
                }
            });
        };
        const auto add = [&editor](const char* name) { editor.findChild<QAction*>(name)->trigger(); };
        add("sketchConstraintLength");
        setValue(60);
        click(15, 0);
        QCOMPARE(constraints->count(), 1);
        QCOMPARE(editor.sketch().constraints.at(0).value, 60.0);
        add("sketchConstraintDiameter");
        setValue(12);
        click(25, 15);
        QCOMPARE(constraints->count(), 2);
        QCOMPARE(editor.sketch().constraints.at(1).value, 12.0);

        constraints->setCurrentRow(0);
        setValue(65);
        editor.findChild<QPushButton*>("editSketchDimension")->click();
        QCOMPARE(editor.sketch().constraints.at(0).value, 65.0);
        QVERIFY(!diagnostic->isVisible());

        add("sketchConstraintLength");
        setValue(75);
        click(15, 0);
        QCOMPARE(constraints->count(), 3);
        QVERIFY(diagnostic->isVisible());
        editor.findChild<QPushButton*>("sketchUndo")->click();
        QCOMPARE(constraints->count(), 2);
        QVERIFY(!diagnostic->isVisible());
    }

    void sketchThroughModelTree()
    {
        graph3d::MainWindow window;
        window.resize(1040, 710);
        window.show();
        window.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.initializeScene());

        auto* viewport = window.findChild<graph3d::Viewport*>();
        auto* editor = window.findChild<graph3d::SketchEditor*>();
        auto* tree = window.findChild<QTreeWidget*>();
        QVERIFY(viewport && editor && tree);
        auto* canvas = editor->findChild<QWidget*>("sketchCanvas");
        auto* feedback = editor->findChild<QLabel*>("sketchFeedback");
        QVERIFY(canvas && feedback);
        const auto action = [&window](const char* name) { return window.findChild<QAction*>(name); };
        const auto button = [editor](const char* name) { return editor->findChild<QPushButton*>(name); };
        const auto click = [canvas](int u, int v) {
            const QPoint origin(canvas->width() / 2, canvas->height() / 2);
            QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, origin + QPoint(3 * u, -3 * v));
        };
        const auto acceptInput = [] {
            QTimer::singleShot(0, [] {
                if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                    dialog->accept();
                }
            });
        };
        QSignalSpy errors(viewport, &graph3d::Viewport::operationFailed);
        action("newScene")->trigger();
        QCOMPARE(tree->topLevelItemCount(), 0);

        action("newSketchXY")->trigger();
        QVERIFY(editor->isVisible());
        QVERIFY(!viewport->isVisible());
        click(0, 0);
        click(25, 0);
        button("sketchFinish")->click();
        QVERIFY(editor->isVisible());
        QVERIFY(feedback->text().contains(QStringLiteral("open"), Qt::CaseInsensitive));
        QCOMPARE(tree->topLevelItemCount(), 0);
        QCOMPARE(errors.count(), 0);
        button("sketchCancel")->click();
        QVERIFY(viewport->isVisible());
        QCOMPARE(tree->topLevelItemCount(), 0);

        for (const auto [left, right] : {std::pair{20, 60}, std::pair{40, 80}}) {
            action("newSketchXY")->trigger();
            button("sketchRectangle")->click();
            click(left, 0);
            click(right, 40);
            button("sketchFinish")->click();
            QVERIFY(viewport->isVisible());
            QVERIFY(action("extrudeSketch")->isEnabled());
            auto* sketch = tree->currentItem();
            QVERIFY(sketch && sketch->text(0).startsWith(QStringLiteral("Sketch")));
            acceptInput();
            action("extrudeSketch")->trigger();
            QCOMPARE(sketch->childCount(), 1);
            QCOMPARE(sketch->checkState(0), Qt::Unchecked);
            QCOMPARE(tree->currentItem(), sketch->child(0));
            QVERIFY(sketch->child(0)->text(1).contains(QStringLiteral("30 mm")));
        }

        auto* firstSketch = tree->topLevelItem(0);
        auto* firstBody = firstSketch->child(0);
        tree->setCurrentItem(firstSketch);
        action("editSketch")->trigger();
        QVERIFY(editor->isVisible());
        QCOMPARE(editor->sketch().entities.size(), std::size_t(1));
        editor->findChild<QAction*>("sketchConstraintLength")->trigger();
        QTimer::singleShot(0, [] {
            if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                dialog->setDoubleValue(45);
                dialog->accept();
            }
        });
        click(40, 0);
        QCOMPARE(editor->sketch().constraints.size(), std::size_t(1));
        button("sketchFinish")->click();
        QVERIFY(viewport->isVisible());
        QCOMPARE(firstSketch->child(0), firstBody);
        QCOMPARE(firstBody->text(2), QString{});
        action("undoDocument")->trigger();
        tree->setCurrentItem(firstSketch);
        action("editSketch")->trigger();
        QVERIFY(editor->sketch().constraints.empty());
        button("sketchCancel")->click();

        auto* secondBody = tree->topLevelItem(1)->child(0);
        tree->setCurrentItem(secondBody);
        QVERIFY(action("booleanUnion")->isEnabled());
        acceptInput();
        action("booleanUnion")->trigger();
        QCOMPARE(tree->topLevelItemCount(), 3);
        QCOMPARE(tree->currentItem()->text(0), QStringLiteral("Union 5"));
        QCOMPARE(firstBody->checkState(0), Qt::Unchecked);
        QCOMPARE(secondBody->checkState(0), Qt::Unchecked);

        auto* unionBody = tree->topLevelItem(2);
        auto* distance = window.findChild<QDoubleSpinBox*>("propertyDistance");
        auto* apply = window.findChild<QPushButton*>("applyProperties");
        auto* state = window.findChild<QLabel*>("propertyState");
        QVERIFY(distance && apply && state);
        tree->setCurrentItem(firstBody);
        QVERIFY(distance->isVisible() && apply->isVisible());
        QCOMPARE(distance->value(), 30.0);
        const auto renderedBeforeEdit = frame(viewport);
        QVERIFY(!renderedBeforeEdit.isNull());
        distance->setValue(0);
        apply->click();
        QCOMPARE(firstBody->text(2), QStringLiteral("Invalid"));
        QCOMPARE(unionBody->text(2), QStringLiteral("Blocked"));
        QVERIFY(state->text().contains(QStringLiteral("nonzero")));
        QTRY_VERIFY(frame(viewport) != renderedBeforeEdit);
        const auto renderedAfterFailure = frame(viewport);
        action("undoDocument")->trigger();
        QCOMPARE(firstBody->text(2), QString{});
        QCOMPARE(unionBody->text(2), QString{});
        QCOMPARE(distance->value(), 30.0);
        QTRY_VERIFY(frame(viewport) != renderedAfterFailure);
        action("redoDocument")->trigger();
        QCOMPARE(distance->value(), 0.0);
        QCOMPARE(unionBody->text(2), QStringLiteral("Blocked"));
        distance->setValue(10.0);
        apply->click();
        QCOMPARE(firstBody->text(2), QString{});
        QCOMPARE(unionBody->text(2), QString{});
        QVERIFY(firstBody->text(1).contains(QStringLiteral("10 mm")));
        QTRY_VERIFY(frame(viewport) != renderedAfterFailure);
        QVERIFY(!action("redoDocument")->isEnabled());

        action("newSketchXZ")->trigger();
        button("sketchRectangle")->click();
        click(10, 0);
        click(20, 30);
        button("sketchFinish")->click();
        QVERIFY(action("revolveSketch")->isEnabled());
        auto* radial = tree->currentItem();
        QTimer::singleShot(0, [] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                dialog->findChild<QComboBox*>("revolveAxis")->setCurrentIndex(1);
                dialog->accept();
            }
        });
        action("revolveSketch")->trigger();
        QCOMPARE(radial->childCount(), 1);
        QCOMPARE(radial->child(0)->text(0), QStringLiteral("Revolve 7"));
        auto* angle = window.findChild<QDoubleSpinBox*>("propertyAngle");
        auto* offset = window.findChild<QDoubleSpinBox*>("propertyOffset");
        QVERIFY(angle && offset && angle->isVisible() && offset->isVisible());
        QCOMPARE(angle->value(), 360.0);
        angle->setValue(180);
        offset->setValue(5.0);
        apply->click();
        QVERIFY(radial->child(0)->text(1).contains(QStringLiteral("180°")));
        QVERIFY(radial->child(0)->text(1).contains(QStringLiteral("offset 5 mm")));
        QCOMPARE(radial->child(0)->text(2), QString{});

        action("newSketchYZ")->trigger();
        button("sketchCircle")->click();
        click(0, 0);
        click(20, 0);
        button("sketchFinish")->click();
        QCOMPARE(tree->currentItem()->text(1), QStringLiteral("Plane YZ"));
        QCOMPARE(errors.count(), 0);
    }

    void projectFileAndExchangeMenus()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        graph3d::MainWindow window;
        window.show();
        window.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.initializeScene());

        auto* tree = window.findChild<QTreeWidget*>("modelTree");
        QVERIFY(tree);
        const auto action = [&window](const char* name) { return window.findChild<QAction*>(name); };
        const auto path = folder.filePath(QStringLiteral("сцена.graph3d"));
        const auto step = folder.filePath(QStringLiteral("сцена.step"));
        const auto stl = folder.filePath(QStringLiteral("сцена.stl"));
        const auto dialogFile = [](const QString& name, QMessageBox::StandardButton next = QMessageBox::NoButton) {
            QTimer::singleShot(0, [name, next] {
                auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                if (!dialog) {
                    return;
                }
                dialog->selectFile(name);
                static_cast<QDialog*>(dialog)->accept();
                if (next != QMessageBox::NoButton) {
                    QTimer::singleShot(0, [next] {
                        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                            if (auto* button = box->button(next)) {
                                button->click();
                            }
                        }
                    });
                }
            });
        };

        QVERIFY(!window.isWindowModified());
        action("createCylinder")->trigger();
        QCOMPARE(tree->topLevelItemCount(), 2);
        QVERIFY(window.isWindowModified());
        dialogFile(path);
        action("saveProject")->trigger();
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(!window.isWindowModified());
        action("createSphere")->trigger();
        QCOMPARE(tree->topLevelItemCount(), 3);
        QVERIFY(window.isWindowModified());
        action("undoDocument")->trigger();
        QCOMPARE(tree->topLevelItemCount(), 2);
        QVERIFY(!window.isWindowModified());
        action("redoDocument")->trigger();
        QVERIFY(window.isWindowModified());

        dialogFile(path, QMessageBox::Cancel);
        action("openProject")->trigger();
        QCOMPARE(tree->topLevelItemCount(), 3);
        QVERIFY(window.isWindowModified());
        dialogFile(path, QMessageBox::Discard);
        action("openProject")->trigger();
        QCOMPARE(tree->topLevelItemCount(), 2);
        QCOMPARE(tree->topLevelItem(1)->text(0), QStringLiteral("Cylinder 2"));
        QVERIFY(!window.isWindowModified());
        QVERIFY(!action("undoDocument")->isEnabled());

        const auto invalid = folder.filePath(QStringLiteral("bad.graph3d"));
        QFile damaged(invalid);
        QVERIFY(damaged.open(QIODevice::WriteOnly));
        QCOMPARE(damaged.write("{"), 1);
        damaged.close();
        dialogFile(invalid, QMessageBox::Ok);
        action("openProject")->trigger();
        QCOMPARE(tree->topLevelItemCount(), 2);
        QVERIFY(window.statusBar()->currentMessage().startsWith(QStringLiteral("Could not open")));
        QVERIFY(!window.isWindowModified());

        dialogFile(step);
        action("exportStep")->trigger();
        QVERIFY(QFileInfo::exists(step));
        dialogFile(stl);
        action("exportStl")->trigger();
        QVERIFY(QFileInfo(stl).size() > 84);
        QVERIFY(!window.isWindowModified());
        dialogFile(step);
        action("importStep")->trigger();
        QCOMPARE(tree->topLevelItemCount(), 3);
        QCOMPARE(tree->topLevelItem(2)->text(0), QStringLiteral("Imported STEP 3"));
        QVERIFY(window.isWindowModified());
        action("saveProject")->trigger();
        QVERIFY(!window.isWindowModified());

        dialogFile(path);
        action("openProject")->trigger();
        QCOMPARE(tree->topLevelItemCount(), 3);
        QCOMPARE(tree->topLevelItem(2)->text(1), QStringLiteral("сцена.step"));
        QVERIFY(!window.isWindowModified());
    }

    void drawingFromSelectedBody()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        graph3d::MainWindow window;
        window.show();
        window.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.initializeScene());
        auto* tree = window.findChild<QTreeWidget*>("modelTree");
        auto* action = window.findChild<QAction*>("drawingPreview");
        QVERIFY(tree && action);
        tree->setCurrentItem(nullptr);
        QVERIFY(!action->isEnabled());
        tree->setCurrentItem(tree->topLevelItem(0));
        QVERIFY(action->isEnabled());
        const auto file = folder.filePath(QStringLiteral("чертёж.pdf"));
        bool sawDrawing = false;
        bool dimensionsChangedPreview = false;
        bool sectionChangedPreview = false;
        bool invalidSectionDisabledExport = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* title = dialog->findChild<QLineEdit*>("drawingTitle");
            auto* canvas = dialog->findChild<QWidget*>("drawingPreviewCanvas");
            auto* dimensions = dialog->findChild<QCheckBox*>("drawingDimensions");
            auto* save = dialog->findChild<QPushButton*>("drawingSavePdf");
            auto* plane = dialog->findChild<QComboBox*>("drawingSectionPlane");
            auto* offset = dialog->findChild<QDoubleSpinBox*>("drawingSectionOffset");
            sawDrawing = dialog->objectName() == "drawingDialog" && title && canvas && dimensions
                && plane && offset && save;
            if (sawDrawing) {
                title->setText(QStringLiteral("Куб"));
                const auto before = canvas->grab().toImage();
                dimensions->setChecked(false);
                dimensionsChangedPreview = !before.isNull() && canvas->grab().toImage() != before;
                dimensions->setChecked(true);
                plane->setCurrentIndex(1);
                sectionChangedPreview = offset->isEnabled() && save->isEnabled()
                    && canvas->grab().toImage() != before;
                offset->setValue(offset->maximum());
                invalidSectionDisabledExport = !save->isEnabled();
                offset->setValue(0);
                QTimer::singleShot(0, [&] {
                    if (auto* chooser = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                        chooser->selectFile(file);
                        static_cast<QDialog*>(chooser)->accept();
                    }
                });
                save->click();
            }
            dialog->reject();
        });
        action->trigger();
        QVERIFY(sawDrawing);
        QVERIFY(dimensionsChangedPreview);
        QVERIFY(sectionChangedPreview);
        QVERIFY(invalidSectionDisabledExport);
        QFile pdf(file);
        QVERIFY(pdf.open(QIODevice::ReadOnly));
        QCOMPARE(pdf.read(5), QByteArrayLiteral("%PDF-"));
        QVERIFY(pdf.size() > 1000);
        QVERIFY(!window.isWindowModified());
    }

    void flangeFromSketchToPdf()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        graph3d::MainWindow window;
        window.resize(1040, 710);
        window.show();
        window.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.initializeScene());
        auto* tree = window.findChild<QTreeWidget*>("modelTree");
        auto* editor = window.findChild<graph3d::SketchEditor*>();
        QVERIFY(tree && editor);
        auto* canvas = editor->findChild<QWidget*>("sketchCanvas");
        auto* circle = editor->findChild<QPushButton*>("sketchCircle");
        auto* finish = editor->findChild<QPushButton*>("sketchFinish");
        QVERIFY(canvas && circle && finish);
        const auto action = [&window](const char* name) { return window.findChild<QAction*>(name); };
        const auto chooseFile = [](const QString& path) {
            QTimer::singleShot(0, [path] {
                if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                    dialog->selectFile(path);
                    static_cast<QDialog*>(dialog)->accept();
                }
            });
        };
        const auto at = [canvas](int x, int y) {
            const auto origin = QPoint(canvas->width() / 2, canvas->height() / 2);
            return origin + QPoint(x * 3, -y * 3);
        };
        struct Circle { int x, y, r; };
        const std::array circles{Circle{0, 0, 50}, Circle{0, 0, 20}, Circle{35, 0, 5},
                                 Circle{0, 35, 5}, Circle{-35, 0, 5}, Circle{0, -35, 5}};

        action("newScene")->trigger();
        QTreeWidgetItem* body = nullptr;
        for (const auto& item : circles) {
            action("newSketchXY")->trigger();
            QVERIFY(editor->isVisible());
            circle->click();
            QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, at(item.x, item.y));
            QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, at(item.x + item.r, item.y));
            finish->click();
            QVERIFY(!editor->isVisible());
            QVERIFY(action("extrudeSketch")->isEnabled());
            QTimer::singleShot(0, [] {
                if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                    dialog->setDoubleValue(15);
                    dialog->accept();
                }
            });
            action("extrudeSketch")->trigger();
            auto* tool = tree->currentItem();
            QVERIFY(tool && tool->text(0).startsWith(QStringLiteral("Extrude")));
            if (!body) {
                body = tool;
                continue;
            }
            const auto toolName = tool->text(0);
            tree->setCurrentItem(body);
            QVERIFY(action("booleanCut")->isEnabled());
            QTimer::singleShot(0, [toolName] {
                if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                    auto* options = dialog->findChild<QComboBox*>();
                    if (!options || options->findText(toolName) < 0) {
                        dialog->reject();
                        return;
                    }
                    options->setCurrentText(toolName);
                    dialog->accept();
                }
            });
            action("booleanCut")->trigger();
            QVERIFY(tree->currentItem() && tree->currentItem() != body);
            QCOMPARE(body->checkState(0), Qt::Unchecked);
            QCOMPARE(tool->checkState(0), Qt::Unchecked);
            body = tree->currentItem();
            QVERIFY(body->text(0).startsWith(QStringLiteral("Cut")));
        }
        QCOMPARE(body->text(0), QStringLiteral("Cut 17"));
        QCOMPARE(body->checkState(0), Qt::Checked);
        const auto projectPath = folder.filePath(QStringLiteral("flange.graph3d"));
        chooseFile(projectPath);
        action("saveProject")->trigger();
        QVERIFY(QFileInfo::exists(projectPath));
        QVERIFY(!window.isWindowModified());
        chooseFile(projectPath);
        action("openProject")->trigger();
        const auto results = tree->findItems(QStringLiteral("Cut 17"), Qt::MatchExactly);
        QCOMPARE(results.size(), 1);
        tree->setCurrentItem(results.constFirst());

        const auto pdfPath = folder.filePath(QStringLiteral("flange.pdf"));
        const auto* drawingAction = action("drawingPreview");
        QVERIFY(drawingAction && drawingAction->isEnabled());
        bool sawDrawing = false;
        QTimer::singleShot(0, [&] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                sawDrawing = dialog->objectName() == "drawingDialog";
                if (sawDrawing) {
                    auto* save = dialog->findChild<QPushButton*>("drawingSavePdf");
                    if (save) {
                        chooseFile(pdfPath);
                        save->click();
                    }
                }
                dialog->reject();
            }
        });
        action("drawingPreview")->trigger();
        QVERIFY(sawDrawing);
        QFile pdf(pdfPath);
        QVERIFY(pdf.open(QIODevice::ReadOnly));
        QCOMPARE(pdf.read(5), QByteArrayLiteral("%PDF-"));
        QVERIFY(!window.isWindowModified());
    }

    void createAndEditAssembly()
    {
        graph3d::MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.initializeScene());
        auto* tree = window.findChild<QTreeWidget*>("modelTree");
        auto* create = window.findChild<QAction*>("createAssembly");
        auto* edit = window.findChild<QPushButton*>("editAssemblyComponents");
        QVERIFY(tree && create && edit);
        QVERIFY(create->isEnabled());
        QVERIFY(!edit->isVisible());

        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* table = dialog->findChild<QTableWidget*>("assemblyComponents");
            auto* count = dialog->findChild<QSpinBox*>("assemblyCount");
            if (!table || !count) {
                dialog->reject();
                return;
            }
            count->setValue(3);
            auto* anchor = qobject_cast<QComboBox*>(table->cellWidget(2, 1));
            auto* z = qobject_cast<QDoubleSpinBox*>(table->cellWidget(2, 4));
            anchor->setCurrentIndex(2);
            z->setValue(140);
            dialog->accept();
        });
        create->trigger();
        QCOMPARE(tree->topLevelItemCount(), 2);
        QCOMPARE(tree->topLevelItem(0)->checkState(0), Qt::Unchecked);
        auto* assembly = tree->currentItem();
        QVERIFY(assembly);
        QCOMPARE(assembly->text(0), QStringLiteral("Assembly 2"));
        QCOMPARE(assembly->text(1), QStringLiteral("Components: 3"));
        QVERIFY(edit->isVisible());

        const auto placementX = [&](double value, bool apply) {
            double previous = -1;
            QTimer::singleShot(0, &window, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) {
                    return;
                }
                auto* table = dialog->findChild<QTableWidget*>("assemblyComponents");
                if (!table || table->rowCount() != 3) {
                    dialog->reject();
                    return;
                }
                auto* x = qobject_cast<QDoubleSpinBox*>(table->cellWidget(0, 2));
                previous = x->value();
                if (apply) {
                    x->setValue(value);
                }
                dialog->done(apply ? QDialog::Accepted : QDialog::Rejected);
            });
            edit->click();
            return previous;
        };
        QCOMPARE(placementX(75, true), 0.0);
        QVERIFY(assembly->text(2).isEmpty());
        QCOMPARE(placementX(0, false), 75.0);
        window.findChild<QAction*>("undoDocument")->trigger();
        QCOMPARE(placementX(0, false), 0.0);
        window.findChild<QAction*>("redoDocument")->trigger();
        QCOMPARE(placementX(0, false), 75.0);
    }

    void transformSelectedBody()
    {
        graph3d::MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.initializeScene());
        auto* tree = window.findChild<QTreeWidget*>("modelTree");
        QVERIFY(tree);
        auto* source = tree->topLevelItem(0);
        QVERIFY(source);
        tree->setCurrentItem(source);
        auto* properties = window.findChild<QWidget*>("propertyPanel");
        QVERIFY(properties);
        auto* apply = properties->findChild<QPushButton*>("applyProperties");
        auto* count = properties->findChild<QSpinBox*>("transformCount");
        auto* spacing = properties->findChild<QDoubleSpinBox*>("transformSpacing");
        auto* sweep = properties->findChild<QDoubleSpinBox*>("transformSweep");
        auto* offset = properties->findChild<QDoubleSpinBox*>("transformOffset");
        QVERIFY(apply && count && spacing && sweep && offset);
        for (const auto [actionName, title] : {std::pair{"makeLinearArray", "Linear array"},
                                                std::pair{"makePolarArray", "Circular array"},
                                                std::pair{"makeMirror", "Mirror"}}) {
            auto* action = window.findChild<QAction*>(actionName);
            QVERIFY(action && action->isEnabled());
            bool dialogSeen = false;
            QTimer::singleShot(0, &window, [&] {
                if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                    dialogSeen = dialog->objectName() == "transformDialog";
                    dialog->done(dialogSeen ? QDialog::Accepted : QDialog::Rejected);
                }
            });
            action->trigger();
            QVERIFY(dialogSeen);
            QCOMPARE(source->childCount(), 1);
            QCOMPARE(source->checkState(0), Qt::Unchecked);
            auto* result = source->child(0);
            QVERIFY(result->text(0).startsWith(QString::fromLatin1(title)));
            QVERIFY(result->text(2).isEmpty());
            QCOMPARE(tree->currentItem(), result);
            source = result;
        }
        QVERIFY(offset->isVisible() && !count->isVisible() && !sweep->isVisible());
        offset->setValue(25);
        apply->click();
        QVERIFY(source->text(2).isEmpty());
        window.findChild<QAction*>("undoDocument")->trigger();
        QCOMPARE(offset->value(), 0.0);
        window.findChild<QAction*>("redoDocument")->trigger();
        QCOMPARE(offset->value(), 25.0);
        tree->setCurrentItem(source->parent()->parent());
        QVERIFY(count->isVisible() && spacing->isVisible() && !offset->isVisible());
        count->setValue(2);
        apply->click();
        QCOMPARE(count->value(), 2);
        QVERIFY(source->text(2).isEmpty());
    }

    void machineSelectedFaceAndEdges()
    {
        graph3d::MainWindow window;
        window.resize(920, 650);
        window.show();
        window.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.initializeScene());
        auto* tree = window.findChild<QTreeWidget*>("modelTree");
        auto* viewport = window.findChild<graph3d::Viewport*>();
        QVERIFY(tree && viewport);
        auto* cube = tree->topLevelItem(0);
        const auto action = [&window](const char* name) { return window.findChild<QAction*>(name); };
        QVERIFY(cube);
        tree->setCurrentItem(cube);
        action("viewTop")->trigger();
        action("fitScene")->trigger();
        QVERIFY(action("newSketchOnFace")->isEnabled());
        action("newSketchOnFace")->trigger();
        QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, viewport->rect().center());
        auto* sketchEditor = window.findChild<graph3d::SketchEditor*>();
        QVERIFY(sketchEditor && sketchEditor->isVisible());
        QVERIFY(sketchEditor->sketch().support.has_value());
        QCOMPARE(sketchEditor->sketch().support->body, 1);
        sketchEditor->findChild<QPushButton*>("sketchCancel")->click();
        QVERIFY(viewport->isVisible());
        QCOMPARE(cube->childCount(), 0);
        QVERIFY(action("makeHole")->isEnabled());
        action("makeHole")->trigger();
        QVERIFY(!action("makeChamfer")->isEnabled());
        action("clearSelection")->trigger();
        QCOMPARE(tree->currentItem(), cube);
        QCOMPARE(cube->childCount(), 0);
        QVERIFY(action("makeHole")->isEnabled());

        action("makeHole")->trigger();
        bool holeDialogSeen = false;
        QTimer::singleShot(0, [&] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                holeDialogSeen = dialog->objectName() == "holeDialog";
                if (holeDialogSeen) {
                    auto* through = dialog->findChild<QCheckBox*>("holeThrough");
                    auto* depth = dialog->findChild<QDoubleSpinBox*>("holeDepth");
                    through->setChecked(false);
                    depth->setValue(20);
                    dialog->accept();
                } else {
                    dialog->reject();
                }
            }
        });
        QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, viewport->rect().center());
        QVERIFY(holeDialogSeen);
        QCOMPARE(cube->childCount(), 1);
        auto* hole = cube->child(0);
        QCOMPARE(hole->text(0), QStringLiteral("Hole 2"));
        QVERIFY(hole->text(1).contains(QStringLiteral("Depth 20 mm")));
        QCOMPARE(cube->checkState(0), Qt::Unchecked);
        auto* through = window.findChild<QCheckBox*>("propertyHoleThrough");
        auto* apply = window.findChild<QPushButton*>("applyProperties");
        QVERIFY(through && apply && through->isVisible());
        through->setChecked(true);
        apply->click();
        QVERIFY(hole->text(1).contains(QStringLiteral("Through")));
        action("undoDocument")->trigger();
        QVERIFY(hole->text(1).contains(QStringLiteral("Depth 20 mm")));
        action("redoDocument")->trigger();
        QVERIFY(hole->text(1).contains(QStringLiteral("Through")));
        action("undoDocument")->trigger();
        action("undoDocument")->trigger();
        QCOMPARE(cube->childCount(), 0);
        QCOMPARE(cube->checkState(0), Qt::Checked);

        for (const auto [actionName, prefix] : {std::pair{"makeChamfer", "Chamfer"},
                                                std::pair{"makeFillet", "Fillet"}}) {
            tree->setCurrentItem(cube);
            action("viewTop")->trigger();
            action("fitScene")->trigger();
            QVERIFY(action(actionName)->isEnabled());
            action(actionName)->trigger();
            const auto pixels = frame(viewport);
            QVERIFY(!pixels.isNull());
            const auto center = viewport->rect().center();
            const auto inside = pixels.pixelColor(center);
            const auto diff = [&inside](const QColor& c) {
                return std::abs(c.red() - inside.red()) + std::abs(c.green() - inside.green())
                       + std::abs(c.blue() - inside.blue());
            };
            int rim = 1;
            while (rim < center.y() && diff(pixels.pixelColor(center.x(), rim)) > 25) {
                ++rim;
            }
            QVERIFY(rim < center.y() - 10);
            for (const int offset : {-1, 0, -2, 1, -3, 2, -4, 3}) {
                QTimer::singleShot(0, [] {
                    if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                        dialog->setDoubleValue(3);
                        dialog->accept();
                    }
                });
                QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(center.x(), rim + offset));
                QCoreApplication::processEvents();
                if (cube->childCount() == 1) {
                    break;
                }
            }
            QCOMPARE(cube->childCount(), 1);
            QCOMPARE(cube->child(0)->text(0), QString::fromLatin1(prefix) + QStringLiteral(" 2"));
            auto* edgeSize = window.findChild<QDoubleSpinBox*>("propertyEdgeSize");
            QVERIFY(edgeSize && edgeSize->isVisible());
            QCOMPARE(edgeSize->value(), 3.0);
            edgeSize->setValue(4);
            apply->click();
            QVERIFY(cube->child(0)->text(1).contains(QStringLiteral("4 mm")));
            action("undoDocument")->trigger();
            action("undoDocument")->trigger();
            QCOMPARE(cube->childCount(), 0);
            QCOMPARE(cube->checkState(0), Qt::Checked);
        }
    }
private:
    QTemporaryDir settingsDir_;
};

QTEST_MAIN(ViewportTest)
#include "ViewportTest.moc"
