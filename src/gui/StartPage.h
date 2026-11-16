#pragma once

#include <QWidget>

class QGridLayout;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace graph3d {

class ExamplePreview;

class StartPage final : public QWidget {
    Q_OBJECT

public:
    explicit StartPage(QWidget* parent = nullptr);
    void rememberProject(const QString& path);
    void setWorkspaceAvailable(bool available);

protected:
    void resizeEvent(QResizeEvent* event) override;

signals:
    void newProjectRequested();
    void cubeRequested();
    void sketchRequested();
    void openRequested();
    void recentRequested(const QString& path);
    void exampleRequested(const QString& path);
    void resumeRequested();
    void controlsRequested();

private:
    void arrangePanels(bool compact);
    void refreshRecents();
    void showExample(int index);

    QGridLayout* columns_;
    QWidget* recentPane_;
    QWidget* createPane_;
    QWidget* helpPane_;
    QVBoxLayout* recentLayout_;
    QPushButton* resume_;
    ExamplePreview* preview_;
    QLabel* exampleName_;
    QLabel* exampleDescription_;
    int exampleIndex_ = 0;
    bool compact_ = false;
};

}  // namespace graph3d
