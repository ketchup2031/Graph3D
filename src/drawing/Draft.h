#pragma once

#include <QPointF>
#include <QSizeF>
#include <QString>

#include <vector>

class QPainter;

namespace graph3d::drawing {

enum class DraftKind { Line, Rectangle, Circle, Text, Horizontal, Vertical };
enum class DraftStroke { Solid, Hidden, Center };

struct DraftItem {
    DraftKind kind = DraftKind::Line;
    QPointF a;
    QPointF b;
    QPointF label;
    QString text;
    DraftStroke stroke = DraftStroke::Solid;
    bool operator==(const DraftItem&) const = default;
};

struct Draft {
    QString title;
    QString number;
    QString author;
    bool a3 = false;
    std::vector<DraftItem> items;
};

QSizeF draftSize(const Draft& draft);
bool validItem(const DraftItem& item);
void paintDraftItem(QPainter& painter, const DraftItem& item, bool selected = false);
bool draftHit(const DraftItem& item, QPointF point, double tolerance);
void paintDraft(QPainter& painter, const Draft& draft, bool grid = false, int selected = -1);
void exportDraftPdf(const QString& path, const Draft& draft);

}  // namespace graph3d::drawing
