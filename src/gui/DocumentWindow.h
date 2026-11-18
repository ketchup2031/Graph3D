#pragma once

#include <QHash>
#include <QMainWindow>
#include <QStringList>
#include <QTimer>

#include <memory>

class QLockFile;
class QTabWidget;

namespace graph3d {

namespace drawing { struct SpecificationDocument; }
class DraftWindow;
class MainWindow;
class StartPage;
class SpecificationWindow;

class DocumentWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit DocumentWindow(QWidget* parent = nullptr, const QString& recoveryRoot = {});
    ~DocumentWindow() override;
    bool openDocument(const QString& path, bool example = false);
    void offerRecovery();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    friend class DraftWindow;
    friend class MainWindow;
    friend class SpecificationWindow;

    struct Tab {
        QString id;
        QString label;
        QString importPath;
        bool pending = false;
    };

    void showHome();
    void browse();
    void newProject(bool cube = false, bool sketch = false);
    void newDrawing();
    void newSpecification();
    void createSpecification(drawing::SpecificationDocument spec);
    void closeCurrent();
    void addDocument(QWidget* document, const QString& label = {}, const QString& imported = {});
    bool canClose(QWidget* document);
    void removeDocument(QWidget* document);
    void setDarkTheme(bool dark);
    void rememberProject(const QString& path);
    QWidget* findDocument(const QString& path, const QWidget* except = nullptr) const;
    bool canSaveAs(QWidget* document, const QString& path);
    QString pathOf(const QWidget* document) const;
    void documentChanged(QWidget* document);
    void updateTitle();
    QString snapshotPath(QWidget* document) const;
    bool writeSnapshot(QWidget* document);
    void removeSnapshot(const QString& path);
    QStringList pendingRecovery() const;
    bool restoreSnapshot(const QString& path);
    void autosave();

    QTabWidget* tabs_;
    StartPage* home_;
    QHash<QWidget*, Tab> documents_;
    QString recoveryRoot_;
    QString session_;
    std::unique_ptr<QLockFile> sessionLock_;
    QTimer autosaveTimer_;
};

}  // namespace graph3d
