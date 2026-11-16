#include "gui/MainWindow.h"
#include "gui/SketchEditor.h"
#include "viewport/Viewport.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QInputDialog>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTest>
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

QImage frame(QWidget* widget)
{
    return widget->screen()->grabWindow(widget->winId()).toImage();
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
        QCOMPARE(errors.count(), 1);
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

        const auto* firstBody = tree->topLevelItem(0)->child(0);
        const auto* secondBody = tree->topLevelItem(1)->child(0);
        QVERIFY(action("booleanUnion")->isEnabled());
        acceptInput();
        action("booleanUnion")->trigger();
        QCOMPARE(tree->topLevelItemCount(), 3);
        QCOMPARE(tree->currentItem()->text(0), QStringLiteral("Union 5"));
        QCOMPARE(firstBody->checkState(0), Qt::Unchecked);
        QCOMPARE(secondBody->checkState(0), Qt::Unchecked);

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

        action("newSketchYZ")->trigger();
        button("sketchCircle")->click();
        click(0, 0);
        click(20, 0);
        button("sketchFinish")->click();
        QCOMPARE(tree->currentItem()->text(1), QStringLiteral("Plane YZ"));
        QCOMPARE(errors.count(), 1);
    }
};

QTEST_MAIN(ViewportTest)
#include "ViewportTest.moc"
