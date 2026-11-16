#include "gui/MainWindow.h"
#include "viewport/Viewport.h"

#include <QAction>
#include <QApplication>
#include <QMouseEvent>
#include <QScreen>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTest>
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
};

QTEST_MAIN(ViewportTest)
#include "ViewportTest.moc"
