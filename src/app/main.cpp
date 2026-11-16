#include "geometry/PreviewBox.h"

#include <QApplication>
#include <QColor>
#include <QMainWindow>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QStatusBar>
#include <QWidget>

#include <algorithm>
#include <exception>
#include <vector>

namespace {

class Preview final : public QWidget {
public:
    Preview() : edges_(mechforge::geometry::makePreviewBox()) {}

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(25, 31, 41));
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(121, 209, 245), 2));
        painter.translate(width() / 2.0, height() / 2.0);
        const double scale = std::min(width(), height()) / 260.0;
        const auto project = [scale](const mechforge::geometry::Point& p) {
            return QPointF(0.8660254 * (p.x - p.y) * scale,
                           (0.5 * (p.x + p.y) - p.z) * scale);
        };

        for (const auto& edge : edges_) {
            painter.drawLine(project(edge.start), project(edge.end));
        }
    }

private:
    std::vector<mechforge::geometry::Segment> edges_;
};

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try {
        QMainWindow window;
        window.setWindowTitle(QStringLiteral("MechForge"));
        window.resize(800, 560);
        window.setCentralWidget(new Preview);
        window.statusBar()->showMessage(QStringLiteral("OCCT test solid | 100 x 100 x 100 mm"));
        window.show();
        return app.exec();
    } catch (const std::exception& error) {
        QMessageBox::critical(nullptr, QStringLiteral("MechForge"), QString::fromUtf8(error.what()));
        return 1;
    }
}
