#pragma once

#include <QJsonObject>
#include <QTranslator>

namespace graph3d {

class InterfaceLanguage final : public QTranslator {
public:
    explicit InterfaceLanguage(bool russian);
    ~InterfaceLanguage() override;

    bool isEmpty() const override;
    QString translate(const char* context, const char* sourceText,
                      const char* disambiguation = nullptr, int n = -1) const override;

private:
    QJsonObject russian_;
};

bool russianLanguagePreferred();
bool russianInterface();
void setRussianLanguagePreferred(bool enabled);
QString localizedDiagnostic(const char* source);

}  // namespace graph3d
