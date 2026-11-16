#pragma once

#include <QMainWindow>

namespace graph3d {

class Viewport;

class MainWindow final : public QMainWindow {
public:
    MainWindow();
    bool initializeScene();

private:
    void createMenus();
    void writeLog(const QString& message);
    void showSceneError(const QString& message);

    Viewport* viewport_;
    QString logPath_;
};

}  // namespace graph3d
