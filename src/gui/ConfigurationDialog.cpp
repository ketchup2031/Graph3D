#include "gui/MainWindow.h"

#include "gui/InterfaceLanguage.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <stdexcept>

namespace graph3d {
void MainWindow::editVariables()
{
    const int id = selectedId();
    const auto* object = document_.find(id);
    auto config = document_.configuration();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("modelConfigurationDialog"));
    dialog.setWindowTitle(tr("Variables and variants"));
    dialog.resize(660, 460);
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel(tr("Use names, numbers, +, −, *, /, ^ and parentheses. Select a model object first to edit its formulas."), &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto* tabs = new QTabWidget(&dialog);
    layout->addWidget(tabs);
    auto table = [&](const QString& name, const QStringList& columns, bool editableRows) {
        auto* page = new QWidget(tabs);
        auto* panel = new QVBoxLayout(page);
        auto* grid = new QTableWidget(0, columns.size(), page);
        grid->setHorizontalHeaderLabels(columns);
        grid->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        panel->addWidget(grid);
        if (editableRows) {
            auto* actions = new QHBoxLayout;
            auto* add = new QPushButton(tr("Add row"), page);
            auto* remove = new QPushButton(tr("Remove row"), page);
            actions->addWidget(add);
            actions->addWidget(remove);
            actions->addStretch();
            panel->addLayout(actions);
            connect(add, &QPushButton::clicked, &dialog, [=] {
                const int row = grid->rowCount();
                grid->insertRow(row);
                auto* first = new QTableWidgetItem;
                grid->setItem(row, 0, first);
                grid->editItem(first);
            });
            connect(remove, &QPushButton::clicked, &dialog, [=] {
                if (grid->currentRow() >= 0) { grid->removeRow(grid->currentRow()); }
            });
        }
        tabs->addTab(page, name);
        return grid;
    };
    const auto row = [](QTableWidget* grid, const QStringList& cells) {
        const int index = grid->rowCount();
        grid->insertRow(index);
        for (int i = 0; i < cells.size(); ++i) { grid->setItem(index, i, new QTableWidgetItem(cells[i])); }
        return index;
    };
    const auto cell = [](const QTableWidget* grid, int r, int c) {
        const auto* item = grid->item(r, c);
        return item ? item->text().trimmed().toStdString() : std::string{};
    };
    auto* vars = table(tr("Variables"), {tr("Name"), tr("Expression")}, true);
    auto* variants = table(tr("Variant overrides"), {tr("Variant"), tr("Variable"), tr("Expression")}, true);
    auto* bindings = table(tr("Object #%1 formulas").arg(id), {tr("Parameter"), tr("Expression")}, false);
    vars->setObjectName(QStringLiteral("modelVariablesTable"));
    variants->setObjectName(QStringLiteral("modelVariantsTable"));
    bindings->setObjectName(QStringLiteral("modelFormulasTable"));
    for (const auto& [key, value] : config.variables) {
        row(vars, {QString::fromStdString(key), QString::fromStdString(value)});
    }
    for (const auto& [name, items] : config.variants) {
        if (items.empty()) { row(variants, {QString::fromStdString(name), {}, {}}); }
        for (const auto& [key, value] : items) {
            row(variants, {QString::fromStdString(name), QString::fromStdString(key), QString::fromStdString(value)});
        }
    }
    if (object) {
        for (const auto& key : document::formulaFields(object->parameters)) {
            const auto& fields = config.bindings[id];
            const auto found = fields.find(key);
            const int index = row(bindings, {QString::fromStdString(key),
                found == fields.end() ? QString{} : QString::fromStdString(found->second)});
            bindings->item(index, 0)->setFlags(bindings->item(index, 0)->flags() & ~Qt::ItemIsEditable);
        }
    }
    tabs->setTabEnabled(2, bindings->rowCount() != 0);
    auto* active = new QComboBox(&dialog);
    active->setObjectName(QStringLiteral("activeModelVariant"));
    layout->addWidget(new QLabel(tr("Active variant (blank uses the base values)"), &dialog));
    layout->addWidget(active);
    active->setEditable(true);
    active->addItem(QString{});
    const auto syncVariants = [&] {
        const auto selected = active->currentText();
        active->clear();
        active->addItem(QString{});
        for (int i = 0; i < variants->rowCount(); ++i) {
            const auto name = QString::fromStdString(cell(variants, i, 0));
            if (!name.isEmpty() && active->findText(name) < 0) { active->addItem(name); }
        }
        active->setEditText(selected);
    };
    syncVariants();
    active->setEditText(QString::fromStdString(config.activeVariant));
    connect(variants, &QTableWidget::cellChanged, &dialog, [&] { syncVariants(); });
    connect(variants->model(), &QAbstractItemModel::rowsRemoved, &dialog, [&] { syncVariants(); });
    auto* diagnostic = new QLabel(&dialog);
    diagnostic->setObjectName(QStringLiteral("formulaDiagnostic"));
    diagnostic->setWordWrap(true);
    layout->addWidget(diagnostic);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            auto edited = config;
            edited.variables.clear();
            edited.variants.clear();
            edited.activeVariant = active->currentText().trimmed().toStdString();
            edited.bindings.erase(id);
            for (int i = 0; i < vars->rowCount(); ++i) {
                const auto key = cell(vars, i, 0);
                const auto value = cell(vars, i, 1);
                if (key.empty() && value.empty()) { continue; }
                if (!edited.variables.emplace(key, value).second) { throw std::invalid_argument("Duplicate variable: " + key); }
            }
            for (int i = 0; i < variants->rowCount(); ++i) {
                const auto name = cell(variants, i, 0);
                const auto key = cell(variants, i, 1);
                const auto value = cell(variants, i, 2);
                if (name.empty() && key.empty() && value.empty()) { continue; }
                auto& overrides = edited.variants[name];
                if (key.empty() && value.empty()) { continue; }
                if (!overrides.emplace(key, value).second) { throw std::invalid_argument("Duplicate variant override: " + key); }
            }
            for (int i = 0; i < bindings->rowCount(); ++i) {
                auto value = cell(bindings, i, 1);
                if (!value.empty()) { edited.bindings[id].emplace(cell(bindings, i, 0), std::move(value)); }
            }
            if (document_.setConfiguration(std::move(edited))) { refreshDocument(id); }
            dialog.accept();
        } catch (const std::exception& error) {
            const auto text = QString::fromUtf8(error.what());
            const auto marker = text.contains(" / ") ? text.section(" / ", -1) : text;
            const auto key = marker.section(": ", 0, 0);
            for (int i = 0; i < vars->rowCount(); ++i) {
                if (QString::fromStdString(cell(vars, i, 0)) == key) {
                    tabs->setCurrentWidget(vars->parentWidget());
                    vars->setCurrentCell(i, 1);
                    break;
                }
            }
            diagnostic->setText(localizedDiagnostic(error.what()));
        }
    });
    dialog.exec();
}
}  // namespace graph3d
