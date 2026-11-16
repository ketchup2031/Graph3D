#include "gui/UserPreferences.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyle>

namespace graph3d {

QSettings userSettings()
{
    return {QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Graph3D"),
            QCoreApplication::applicationName()};
}

bool darkThemeEnabled()
{
    return userSettings().value(QStringLiteral("appearance/darkTheme"), false).toBool();
}

void applyTheme(bool dark)
{
    static const auto lightStyle = QApplication::style()->objectName();
    static const auto lightPalette = QApplication::palette();
    const auto style = dark ? QStringLiteral("Fusion") : lightStyle;
    if (QApplication::style()->objectName().compare(style, Qt::CaseInsensitive) != 0) {
        QApplication::setStyle(style);
    }
    if (!dark) {
        QApplication::setPalette(lightPalette);
        return;
    }

    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#202832"));
    palette.setColor(QPalette::WindowText, QColor("#e6edf6"));
    palette.setColor(QPalette::Base, QColor("#171e27"));
    palette.setColor(QPalette::AlternateBase, QColor("#242e3a"));
    palette.setColor(QPalette::ToolTipBase, QColor("#293646"));
    palette.setColor(QPalette::ToolTipText, QColor("#f0f5fb"));
    palette.setColor(QPalette::Text, QColor("#e6edf6"));
    palette.setColor(QPalette::Button, QColor("#2c3745"));
    palette.setColor(QPalette::ButtonText, QColor("#e6edf6"));
    palette.setColor(QPalette::BrightText, QColor("#ffa8a8"));
    palette.setColor(QPalette::Light, QColor("#435469"));
    palette.setColor(QPalette::Midlight, QColor("#344357"));
    palette.setColor(QPalette::Mid, QColor("#293647"));
    palette.setColor(QPalette::Dark, QColor("#151b23"));
    palette.setColor(QPalette::Shadow, QColor("#10151c"));
    palette.setColor(QPalette::Highlight, QColor("#3876b9"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, QColor("#91caff"));
    palette.setColor(QPalette::LinkVisited, QColor("#c3a9fa"));
    palette.setColor(QPalette::PlaceholderText, QColor("#a4b5c8"));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::HighlightedText}) {
        palette.setColor(QPalette::Disabled, role, QColor("#a1aebc"));
    }
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor("#354356"));
    QApplication::setPalette(palette);
}

}  // namespace graph3d
