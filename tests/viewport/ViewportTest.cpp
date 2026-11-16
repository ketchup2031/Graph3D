#include "gui/MainWindow.h"
#include "gui/SketchEditor.h"
#include "viewport/Viewport.h"

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>

#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTest>
#include <QTemporaryDir>
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
    void sceneAndCamera()
    {
        QStandardPaths::setTestModeEnabled(true);
        QApplication::setApplicationName(QStringLiteral("Graph3D-test"));
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

        auto* firstBody = tree->topLevelItem(0)->child(0);
        const auto* secondBody = tree->topLevelItem(1)->child(0);
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
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* title = dialog->findChild<QLineEdit*>("drawingTitle");
            auto* canvas = dialog->findChild<QWidget*>("drawingPreviewCanvas");
            auto* dimensions = dialog->findChild<QCheckBox*>("drawingDimensions");
            auto* save = dialog->findChild<QPushButton*>("drawingSavePdf");
            sawDrawing = dialog->objectName() == "drawingDialog" && title && canvas && dimensions && save;
            if (sawDrawing) {
                title->setText(QStringLiteral("Куб"));
                const auto before = canvas->grab().toImage();
                dimensions->setChecked(false);
                dimensionsChangedPreview = !before.isNull() && canvas->grab().toImage() != before;
                dimensions->setChecked(true);
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
        QFile pdf(file);
        QVERIFY(pdf.open(QIODevice::ReadOnly));
        QCOMPARE(pdf.read(5), QByteArrayLiteral("%PDF-"));
        QVERIFY(pdf.size() > 1000);
        QVERIFY(!window.isWindowModified());
    }
};

QTEST_MAIN(ViewportTest)
#include "ViewportTest.moc"
