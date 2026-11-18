#include "gui/DocumentWindow.h"

#include "gui/DraftWindow.h"
#include "gui/InterfaceLanguage.h"
#include "gui/MainWindow.h"
#include "gui/StartPage.h"
#include "gui/SpecificationWindow.h"
#include "gui/UserPreferences.h"
#include "io/DraftIO.h"
#include "io/FileIO.h"
#include "io/ProjectIO.h"
#include "io/SpecificationIO.h"

#include <QActionGroup>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QMenuBar>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QUuid>

#include <limits>

namespace graph3d {
namespace {

const auto lockName = QStringLiteral("session.lock");

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString identity(const QString& path)
{
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

bool samePath(const QString& left, const QString& right)
{
#ifdef Q_OS_WIN
    return identity(left).compare(identity(right), Qt::CaseInsensitive) == 0;
#else
    return identity(left) == identity(right);
#endif
}

bool hasExtension(const QString& path, const QString& extension)
{
    return path.endsWith(extension, Qt::CaseInsensitive);
}

QString snapshotLabel(const QString& path)
{
    QFile title(path + QStringLiteral(".title"));
    if (title.open(QIODevice::ReadOnly)) {
        return QString::fromUtf8(title.read(4096)).simplified();
    }
    return {};
}

}  // namespace

DocumentWindow::DocumentWindow(QWidget* parent, const QString& recoveryRoot)
    : QMainWindow(parent), tabs_(new QTabWidget(this)), home_(new StartPage(tabs_)),
      recoveryRoot_(recoveryRoot.isEmpty()
          ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("recovery"))
          : recoveryRoot)
{
    setObjectName(QStringLiteral("documentWindow"));
    resize(1120, 760);
    setMinimumSize(480, 320);
    tabs_->setObjectName(QStringLiteral("documentTabs"));
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    setCentralWidget(tabs_);
    const int start = tabs_->addTab(home_, tr("Start page"));
    tabs_->tabBar()->setTabButton(start, QTabBar::LeftSide, nullptr);
    tabs_->tabBar()->setTabButton(start, QTabBar::RightSide, nullptr);
    applyTheme(darkThemeEnabled());
    setDarkTheme(darkThemeEnabled());
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
        auto* document = tabs_->widget(index);
        if (documents_.contains(document)) {
            tabs_->setCurrentWidget(document);
            closeCurrent();
        }
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this] { updateTitle(); });
    connect(home_, &StartPage::newProjectRequested, this, [this] { newProject(); });
    connect(home_, &StartPage::cubeRequested, this, [this] { newProject(true); });
    connect(home_, &StartPage::sketchRequested, this, [this] { newProject(false, true); });
    connect(home_, &StartPage::newDrawingRequested, this, &DocumentWindow::newDrawing);
    connect(home_, &StartPage::openRequested, this, &DocumentWindow::browse);
    connect(home_, &StartPage::recentRequested, this, [this](const QString& path) { openDocument(path); });
    connect(home_, &StartPage::exampleRequested, this, [this](const QString& path) { openDocument(path, true); });
    connect(home_, &StartPage::controlsRequested, this, [this] {
        QMessageBox::information(this, tr("Mouse controls"),
            tr("Left click: select a body\nRight drag: orbit\nMiddle drag: pan\nWheel: zoom\nF: fit all"));
    });
    auto* close = new QShortcut(QKeySequence::Close, this);
    connect(close, &QShortcut::activated, this, &DocumentWindow::closeCurrent);
    auto* open = new QShortcut(QKeySequence::Open, home_);
    open->setContext(Qt::WidgetWithChildrenShortcut);
    connect(open, &QShortcut::activated, this, &DocumentWindow::browse);
    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("New part"), this, [this] { newProject(); });
    file->addAction(tr("New drawing"), this, &DocumentWindow::newDrawing);
    file->addAction(tr("New specification"), this, &DocumentWindow::newSpecification)
        ->setObjectName(QStringLiteral("newSpecification"));
    file->addAction(tr("Open document…"), this, &DocumentWindow::browse);
    auto* recover = file->addAction(tr("Recover documents…"), this, &DocumentWindow::offerRecovery);
    recover->setObjectName(QStringLiteral("recoverDocuments"));
    tabs_->setContextMenuPolicy(Qt::ActionsContextMenu);
    tabs_->addAction(recover);
    home_->setContextMenuPolicy(Qt::ActionsContextMenu);
    home_->addAction(recover);
    file->addAction(tr("E&xit"), this, &QWidget::close);
    auto* settings = menuBar()->addMenu(tr("Sett&ings"));
    auto* theme = settings->addAction(tr("Dark theme"));
    theme->setObjectName(QStringLiteral("homeDarkTheme"));
    theme->setCheckable(true);
    theme->setChecked(darkThemeEnabled());
    connect(theme, &QAction::toggled, this, [this](bool dark) {
        userSettings().setValue(QStringLiteral("appearance/darkTheme"), dark);
        applyTheme(dark);
        setDarkTheme(dark);
    });
    auto* languages = settings->addMenu(tr("Language"));
    auto* group = new QActionGroup(languages);
    for (const bool russian : {false, true}) {
        auto* choice = languages->addAction(russian ? QStringLiteral("Русский") : QStringLiteral("English"));
        group->addAction(choice);
        choice->setCheckable(true);
        choice->setChecked(russian == russianLanguagePreferred());
        connect(choice, &QAction::triggered, this, [this, russian] {
            if (russian != russianLanguagePreferred()) {
                setRussianLanguagePreferred(russian);
                if (russian != russianInterface()) {
                    QMessageBox::information(this, tr("Language settings"), tr("Restart Graph3D to apply the language change."));
                }
            }
        });
    }
    connect(file, &QMenu::aboutToShow, file, [this, recover] { recover->setEnabled(!pendingRecovery().isEmpty()); });
    connect(languages, &QMenu::aboutToShow, languages, [group] {
        group->actions().at(russianLanguagePreferred() ? 1 : 0)->setChecked(true);
    });

    autosaveTimer_.setSingleShot(true);
    autosaveTimer_.setInterval(1000);
    connect(&autosaveTimer_, &QTimer::timeout, this, &DocumentWindow::autosave);
    updateTitle();
}

DocumentWindow::~DocumentWindow()
{
    for (auto* model : findChildren<MainWindow*>()) {
        model->host_ = nullptr;
    }
    for (auto* draft : findChildren<DraftWindow*>()) {
        draft->host_ = nullptr;
    }
    for (auto* spec : findChildren<SpecificationWindow*>()) { spec->host_ = nullptr; }
    sessionLock_.reset();
    if (!session_.isEmpty()) {
        QDir().rmdir(session_);
    }
}

void DocumentWindow::showHome()
{
    tabs_->setCurrentWidget(home_);
}

void DocumentWindow::setDarkTheme(bool dark)
{
    home_->setDarkTheme(dark);
    if (auto* action = findChild<QAction*>(QStringLiteral("homeDarkTheme"))) {
        const QSignalBlocker block(action);
        action->setChecked(dark);
    }
    for (auto* model : findChildren<MainWindow*>()) {
        if (auto* action = model->findChild<QAction*>(QStringLiteral("darkTheme"))) {
            const QSignalBlocker block(action);
            action->setChecked(dark);
            model->startPage_->setDarkTheme(dark);
        }
    }
}

void DocumentWindow::rememberProject(const QString& path)
{
    home_->rememberProject(path);
}

void DocumentWindow::browse()
{
    const auto path = QFileDialog::getOpenFileName(this, tr("Open document"), pathOf(tabs_->currentWidget()),
        tr("Supported files (*.graph3d *.g3draw *.g3spec *.dxf *.dwg);;3D projects (*.graph3d);;2D drawings (*.g3draw);;Specifications (*.g3spec);;CAD drawings (*.dxf *.dwg)"),
        nullptr, (darkThemeEnabled() || russianInterface()) ? QFileDialog::DontUseNativeDialog : QFileDialog::Options{});
    if (!path.isEmpty()) {
        openDocument(path);
    }
}

void DocumentWindow::addDocument(QWidget* document, const QString& label, const QString& imported)
{
    documents_.insert(document, Tab{newId(), label, imported});
    tabs_->addTab(document, {});
    tabs_->setCurrentWidget(document);
    if (auto* draft = qobject_cast<DraftWindow*>(document)) {
        connect(draft, &DraftWindow::fileUsed, this, &DocumentWindow::rememberProject);
    }
    documentChanged(document);
}

void DocumentWindow::newProject(bool cube, bool sketch)
{
    auto* model = new MainWindow(this);
    const auto previous = tabs_->currentWidget();
    addDocument(model);
    const bool ready = cube ? model->initializeScene() : model->newProject();
    if (!ready) {
        removeDocument(model);
        tabs_->setCurrentWidget(previous);
    } else if (sketch) {
        model->beginSketch(geometry::Plane::XY);
    }
}

void DocumentWindow::newDrawing()
{
    addDocument(new DraftWindow(this, this));
}

void DocumentWindow::createDrawing(drawing::Draft draft)
{
    auto* window = new DraftWindow(this,this);
    addDocument(window);
    window->replaceDrawing(std::move(draft));
    window->history_.resetClean();
    window->updateTitle();
}

void DocumentWindow::newSpecification() { createSpecification({}); }

void DocumentWindow::createSpecification(drawing::SpecificationDocument spec)
{
    auto* window = new SpecificationWindow(this);
    addDocument(window);
    window->setDocument(std::move(spec));
}

bool DocumentWindow::openDocument(const QString& path, bool example)
{
    if (!example) {
        if (auto* existing = findDocument(path)) {
            tabs_->setCurrentWidget(existing);
            return true;
        }
    }
    const auto previous = tabs_->currentWidget();
    const bool dxf = hasExtension(path, QStringLiteral(".dxf")) || hasExtension(path, QStringLiteral(".dwg"));
    QWidget* opened = nullptr;
    bool success = false;
    if (hasExtension(path, QStringLiteral(".g3spec"))) {
        auto* spec = new SpecificationWindow(this);
        opened = spec;
        addDocument(spec);
        success = spec->open(path);
    } else if (dxf || hasExtension(path, QStringLiteral(".g3draw"))) {
        auto* draft = new DraftWindow(this, this);
        opened = draft;
        addDocument(draft, dxf ? QFileInfo(path).completeBaseName() : QString{}, dxf ? path : QString{});
        success = dxf ? draft->importDxf(path) : draft->openDrawing(path);
    } else {
        auto* model = new MainWindow(this);
        opened = model;
        addDocument(model, example ? QFileInfo(path).completeBaseName() : QString{});
        model->openProjectPath(path, example);
        success = model->hasWorkspace_;
    }
    if (!success) {
        removeDocument(opened);
        tabs_->setCurrentWidget(previous);
    } else if (!dxf && !example) {
        rememberProject(path);
    }
    return success;
}

QString DocumentWindow::pathOf(const QWidget* document) const
{
    if (const auto* model = qobject_cast<const MainWindow*>(document)) {
        return model->projectPath_;
    }
    if (const auto* draft = qobject_cast<const DraftWindow*>(document)) {
        return draft->path_;
    }
    if (const auto* spec = qobject_cast<const SpecificationWindow*>(document)) { return spec->path_; }
    return {};
}

QWidget* DocumentWindow::findDocument(const QString& path, const QWidget* except) const
{
    if (path.isEmpty()) {
        return nullptr;
    }
    for (auto it = documents_.cbegin(); it != documents_.cend(); ++it) {
        auto* document = it.key();
        if (document == except) {
            continue;
        }
        const auto original = pathOf(document);
        const auto& imported = it->importPath;
        if ((!original.isEmpty() && samePath(path, original))
            || (!imported.isEmpty() && samePath(path, imported))) {
            return document;
        }
    }
    return nullptr;
}

bool DocumentWindow::canSaveAs(QWidget* document, const QString& path)
{
    if (findDocument(path, document)) {
        QMessageBox::warning(document, tr("Save document"), tr("This file is already open in another tab."));
        return false;
    }
    return true;
}

void DocumentWindow::updateTitle()
{
    auto* current = tabs_->currentWidget();
    home_->setEnabled(current == home_);
    menuBar()->setVisible(current == home_);
    for (auto* document : documents_.keys()) {
        document->setEnabled(document == current);
    }
    const int index = tabs_->currentIndex();
    setWindowTitle(!documents_.contains(current) ? QStringLiteral("Graph3D")
        : tr("%1 — Graph3D").arg(tabs_->tabText(index)));
}

void DocumentWindow::documentChanged(QWidget* document)
{
    auto it = documents_.find(document);
    if (it == documents_.end()) {
        return;
    }
    const auto path = pathOf(document);
    if (!path.isEmpty()) {
        it->importPath.clear();
    }
    const auto title = !path.isEmpty() ? QFileInfo(path).fileName()
        : !it->label.isEmpty() ? it->label
        : qobject_cast<DraftWindow*>(document) ? tr("Untitled drawing")
        : qobject_cast<SpecificationWindow*>(document) ? tr("Untitled specification") : tr("Untitled part");
    const int index = tabs_->indexOf(document);
    const bool modified = document->isWindowModified();
    tabs_->setTabText(index, modified ? title + QStringLiteral(" *") : title);
    tabs_->setTabToolTip(index, path.isEmpty() ? title : path);
    if (document == tabs_->currentWidget()) {
        updateTitle();
    }
    it->pending = modified;
    if (!modified) {
        removeSnapshot(snapshotPath(document));
    } else if (!autosaveTimer_.isActive()) {
        autosaveTimer_.start(1000);
    }
}

bool DocumentWindow::canClose(QWidget* document)
{
    if (auto* model = qobject_cast<MainWindow*>(document)) {
        return model->canClose();
    }
    if (auto* spec = qobject_cast<SpecificationWindow*>(document)) { return spec->confirmSave(); }
    return qobject_cast<DraftWindow*>(document)->confirmSave();
}

void DocumentWindow::removeDocument(QWidget* document)
{
    removeSnapshot(snapshotPath(document));
    documents_.remove(document);
    tabs_->removeTab(tabs_->indexOf(document));
    document->hide();
    document->deleteLater();
    updateTitle();
}

void DocumentWindow::closeCurrent()
{
    auto* document = tabs_->currentWidget();
    if (documents_.contains(document) && canClose(document)) {
        removeDocument(document);
    }
}

void DocumentWindow::closeEvent(QCloseEvent* event)
{
    auto* current = tabs_->currentWidget();
    QList<QWidget*> closing;
    for (int i = 0; i < tabs_->count(); ++i) {
        if (auto* document = tabs_->widget(i); documents_.contains(document)) {
            closing.push_back(document);
        }
    }
    for (auto* document : closing) {
        tabs_->setCurrentWidget(document);
        if (!canClose(document)) {
            event->ignore();
            return;
        }
    }
    autosaveTimer_.stop();
    for (auto* document : closing) {
        removeDocument(document);
    }
    tabs_->setCurrentWidget(current == home_ ? current : home_);
    event->accept();
}
QString DocumentWindow::snapshotPath(QWidget* document) const
{
    if (session_.isEmpty()) {
        return {};
    }
    const auto extension = qobject_cast<DraftWindow*>(document) ? QStringLiteral(".g3draw")
        : qobject_cast<SpecificationWindow*>(document) ? QStringLiteral(".g3spec") : QStringLiteral(".graph3d");
    return QDir(session_).filePath(documents_.value(document).id + extension);
}

bool DocumentWindow::writeSnapshot(QWidget* document)
{
    try {
        if (!sessionLock_) {
            const auto folder = QDir(recoveryRoot_).filePath(newId());
            if (!QDir().mkpath(folder)) {
                io::fileError(tr("The recovery directory could not be created."));
            }
            auto lock = std::make_unique<QLockFile>(QDir(folder).filePath(lockName));
            lock->setStaleLockTime(0);
            if (!lock->tryLock()) {
                io::fileError(tr("The recovery directory could not be locked."));
            }
            session_ = folder;
            sessionLock_ = std::move(lock);
        }
        const auto path = snapshotPath(document);
        if (auto* draft = qobject_cast<DraftWindow*>(document)) {
            io::saveDraft(path, draft->wholeDrawing());
        } else if (auto* spec = qobject_cast<SpecificationWindow*>(document)) {
            io::saveSpecification(path, spec->spec_, true);
        } else {
            io::saveProject(path, qobject_cast<MainWindow*>(document)->document_);
        }
        const auto label = tabs_->tabToolTip(tabs_->indexOf(document));
        io::writeFile(path + QStringLiteral(".title"), label.toUtf8());
        documents_[document].pending = false;
        return true;
    } catch (const std::exception& error) {
        statusBar()->showMessage(tr("Autosave failed: %1").arg(localizedDiagnostic(error.what())), 10000);
        return false;
    }
}

void DocumentWindow::removeSnapshot(const QString& path)
{
    if (!path.isEmpty()) {
        QFile::remove(path);
        QFile::remove(path + QStringLiteral(".title"));
    }
}

void DocumentWindow::autosave()
{
    bool retry = false;
    for (auto* document : documents_.keys()) {
        if (documents_.value(document).pending && !writeSnapshot(document)) {
            retry = true;
        }
    }
    if (retry) {
        autosaveTimer_.start(5000);
    }
}

QStringList DocumentWindow::pendingRecovery() const
{
    QStringList found;
    const QDir recovery(recoveryRoot_);
    for (const auto& folder : recovery.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        if (QUuid::fromString(folder.fileName()).isNull() || folder.absoluteFilePath() == session_) {
            continue;
        }
        QDir directory(folder.absoluteFilePath());
        QLockFile lock(directory.filePath(lockName));
        lock.setStaleLockTime(0);
        if (!lock.tryLock()) {
            continue;
        }
        for (const auto& file : directory.entryInfoList({QStringLiteral("*.g3draw"), QStringLiteral("*.graph3d"), QStringLiteral("*.g3spec")},
                                                       QDir::Files | QDir::NoSymLinks, QDir::Time)) {
            found.append(file.absoluteFilePath());
        }
    }
    return found;
}

bool DocumentWindow::restoreSnapshot(const QString& path)
{
    const auto label = snapshotLabel(path);
    const auto recovered = label.isEmpty() ? tr("Recovered document")
                                          : tr("Recovered — %1").arg(QFileInfo(label).fileName());
    QWidget* document = nullptr;
    if (hasExtension(path, QStringLiteral(".g3spec"))) {
        auto restored = io::openSpecification(path);
        auto* spec = new SpecificationWindow(this);
        document = spec;
        addDocument(spec, recovered);
        spec->loadDocument(std::move(restored));
        spec->undo_.resetClean();
        spec->updateTitle();
    } else if (hasExtension(path, QStringLiteral(".g3draw"))) {
        auto draft = io::openDraft(path);
        auto* window = new DraftWindow(this, this);
        document = window;
        addDocument(window, recovered);
        window->replaceDrawing(std::move(draft));
        window->history_.resetClean();
    } else {
        auto* window = new MainWindow(this);
        document = window;
        addDocument(window, recovered);
        window->openProjectPath(path, true);
        if (!window->hasWorkspace_) {
            removeDocument(window);
            return false;
        }
        window->savedRevision_ = std::numeric_limits<std::uint64_t>::max();
        window->updateFileState();
    }
    // Keep the previous copy until this session has saved a replacement.
    return writeSnapshot(document);
}

void DocumentWindow::offerRecovery()
{
    const auto files = pendingRecovery();
    if (files.isEmpty()) {
        return;
    }
    QMessageBox prompt(QMessageBox::Question, tr("Recover documents"), tr("Documents from a previous session are available."),
                       QMessageBox::NoButton, this);
    QStringList labels;
    for (const auto& path : files) {
        const auto label = snapshotLabel(path);
        labels.append(label.isEmpty() ? QFileInfo(path).fileName() : label);
    }
    prompt.setDetailedText(labels.join(u'\n'));
    auto* restore = prompt.addButton(tr("Restore"), QMessageBox::AcceptRole);
    auto* discard = prompt.addButton(tr("Discard recovery copies"), QMessageBox::DestructiveRole);
    auto* later = prompt.addButton(tr("Keep for later"), QMessageBox::RejectRole);
    prompt.setDefaultButton(restore);
    prompt.setEscapeButton(later);
    prompt.exec();
    if (prompt.clickedButton() != restore && prompt.clickedButton() != discard) {
        return;
    }

    for (const auto& path : files) {
        const auto folder = QFileInfo(path).absoluteDir();
        bool empty = false;
        {
            QLockFile lock(folder.filePath(lockName));
            lock.setStaleLockTime(0);
            if (!lock.tryLock() || !QFileInfo::exists(path)) {
                continue;
            }
            try {
                if (prompt.clickedButton() == discard || restoreSnapshot(path)) {
                    removeSnapshot(path);
                }
            } catch (const std::exception& error) {
                QMessageBox::warning(this, tr("Recover documents"), localizedDiagnostic(error.what()));
            }
            empty = folder.entryList(QDir::Files | QDir::AllDirs | QDir::NoDotAndDotDot).size() == 1;
        }
        if (empty) {
            QDir().rmdir(folder.absolutePath());
        }
    }
}

}  // namespace graph3d
