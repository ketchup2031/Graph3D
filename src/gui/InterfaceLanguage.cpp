#include "gui/InterfaceLanguage.h"

#include "gui/UserPreferences.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

namespace graph3d {
namespace {

constexpr auto languageKey = "appearance/language";
constexpr auto languageContext = "graph3d::InterfaceLanguage";

}  // namespace

InterfaceLanguage::InterfaceLanguage(bool russian)
{
    if (!russian) {
        return;
    }

    QFile catalog(QStringLiteral(":/resources/translations/ru.json"));
    if (catalog.open(QIODevice::ReadOnly)) {
        russian_ = QJsonDocument::fromJson(catalog.readAll()).object();
    }
    if (isEmpty()) {
        qWarning("Could not load the Russian Graph3D catalog.");
        return;
    }
    QCoreApplication::installTranslator(this);
}

InterfaceLanguage::~InterfaceLanguage()
{
    if (!isEmpty()) {
        QCoreApplication::removeTranslator(this);
    }
}

bool InterfaceLanguage::isEmpty() const
{
    return russian_.isEmpty();
}

QString InterfaceLanguage::translate(const char* context, const char* sourceText, const char*, int) const
{
    if (!sourceText) {
        return {};
    }
    if (qstrcmp(context, languageContext) == 0 && qstrcmp(sourceText, "en") == 0) {
        return QStringLiteral("ru");
    }
    // Graph3D and its standard controls use the same Russian terms for identical labels.
    return russian_.value(QString::fromUtf8(sourceText)).toString();
}

bool russianLanguagePreferred()
{
    return userSettings().value(QLatin1String(languageKey)).toString() == QStringLiteral("ru");
}

void setRussianLanguagePreferred(bool enabled)
{
    userSettings().setValue(QLatin1String(languageKey), enabled ? QStringLiteral("ru") : QStringLiteral("en"));
}

bool russianInterface()
{
    return QCoreApplication::translate(languageContext, "en") == QStringLiteral("ru");
}

QString localizedDiagnostic(const char* source)
{
    if (!source || !*source) {
        return {};
    }
    const auto translated = QCoreApplication::translate("Diagnostic", source);
    if (!russianInterface()) {
        return translated;
    }
    static const QRegularExpression dependency(QStringLiteral(
        R"(^Input #(\d+) has an error\. Fix it to rebuild this operation\.$)"));
    if (const auto error = dependency.match(translated); error.hasMatch()) {
        return QCoreApplication::translate("Diagnostic", "Input #%1 has an error. Fix it to rebuild this operation.")
            .arg(error.captured(1));
    }
    static const QRegularExpression mate(QStringLiteral(R"(^Mate #(\d+): (.+)$)"));
    if (const auto error = mate.match(translated); error.hasMatch()) {
        const auto details = error.captured(2).toUtf8();
        return QCoreApplication::translate("Diagnostic", "Mate #%1: %2")
            .arg(error.captured(1), QCoreApplication::translate("Diagnostic", details.constData()));
    }
    static const QRegularExpression object(QStringLiteral(R"(^Object #(\d+): (.+)$)"));
    if (const auto error = object.match(translated); error.hasMatch()) {
        const auto details = error.captured(2).toUtf8();
        return QCoreApplication::translate("Diagnostic", "Object #%1: %2")
            .arg(error.captured(1), QCoreApplication::translate("Diagnostic", details.constData()));
    }
    return translated;
}

}  // namespace graph3d
