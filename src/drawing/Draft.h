#pragma once

#include "drawing/ModelViews.h"

#include <QHash>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QUuid>

#include <optional>
#include <span>
#include <vector>

class QPainter;

namespace graph3d::drawing {

enum class DraftKind {
    Line, Rectangle, Circle, Text, Horizontal, Vertical,
    Arc, Polyline, Aligned, Angular, Radius, Diameter, Hatch, Leader, Datum, Tolerance, Roughness
};
enum class DraftStroke { Solid, Hidden, Center };

enum class DraftAnchor { Start, End, CornerAB, CornerBA, Center, Right, Left, Top, Bottom, Curve, Intersection };

struct DraftReference {
    QUuid item;
    DraftAnchor anchor;
    double parameter = 0;
    QUuid crossing;
    bool operator==(const DraftReference&) const = default;
};

struct DraftItem {
    DraftKind kind = DraftKind::Line;
    QPointF a;
    QPointF b;
    QPointF label;
    QString text;
    DraftStroke stroke = DraftStroke::Solid;
    QUuid id;
    std::optional<DraftReference> anchorA;
    std::optional<DraftReference> anchorB;
    std::optional<DraftReference> anchorC;
    std::vector<QPointF> points;
    std::vector<std::vector<QPointF>> loops;
    int layer = 0;
    bool operator==(const DraftItem&) const = default;
};

constexpr int maxSheets = 32;
constexpr int maxLayers = 32;

struct DraftLayer {
    QString name;
    bool visible = true;
    bool locked = false;
    bool operator==(const DraftLayer&) const = default;
};

struct DraftPaper {
    bool portrait = false;
    bool eskd = false;
    QString material;
    QString organization;
    QString requirements;
    bool operator==(const DraftPaper&) const = default;
};

struct Draft {
    QString title;
    QString number;
    QString author;
    bool a3 = false;
    std::vector<DraftItem> items;
    std::optional<ModelViews> modelViews;
    DraftPaper paper;
    std::vector<DraftLayer> layers{{}};
    std::vector<Draft> additionalSheets;
};

bool isAnnotation(DraftKind kind);
bool layerVisible(const Draft& draft, int layer);
bool layerEditable(const Draft& draft, int layer);

std::span<const DraftAnchor> draftAnchors(DraftKind kind);
std::optional<QPointF> draftAnchorPoint(const DraftItem& item, DraftAnchor anchor);
bool isLinearDimension(DraftKind kind);
bool isDimension(DraftKind kind);
QString draftDimensionText(const DraftItem& item);

class DraftGeometry {
public:
    explicit DraftGeometry(const Draft& draft, const DraftItem* preview = nullptr);
    std::optional<QPointF> point(const DraftReference& reference) const;
    bool validReference(const DraftReference& reference) const;
    DraftItem resolve(const DraftItem& item) const;

private:
    QHash<QUuid, const DraftItem*> sources_;
};

QSizeF draftSize(const Draft& draft);
bool validItem(const DraftItem& item);
void paintDraftItem(QPainter& painter, const DraftItem& item, bool selected = false);
bool draftHit(const DraftItem& item, QPointF point, double tolerance);
void paintDraft(QPainter& painter, const Draft& draft, bool grid = false, int selected = -1,
                const DraftItem* preview = nullptr, int sheet = 1, int total = 1);
void exportDraftPdf(const QString& path, const Draft& draft);

}  // namespace graph3d::drawing
