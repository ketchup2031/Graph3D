#pragma once

#include "drawing/ModelViews.h"

#include <QHash>
#include <QLineF>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QRectF>
#include <QUuid>

#include <optional>
#include <span>
#include <vector>

class QPainter;

namespace graph3d::drawing {

enum class DraftKind {
    Line, Rectangle, Circle, Text, Horizontal, Vertical,
    Arc, Polyline, Aligned, Angular, Radius, Diameter, Hatch, Leader, Datum, Tolerance, Roughness,
    Ellipse, Spline, ConstructionLine, Fragment, Table, Thread, Weld
};
enum class DraftStroke { Solid, Hidden, Center };

enum class DraftAnchor { Start, End, CornerAB, CornerBA, Center, Right, Left, Top, Bottom, Curve, Intersection, ModelCurve, ModelCenter };

struct DraftReference {
    QUuid item;
    DraftAnchor anchor;
    double parameter = 0;
    QUuid crossing;
    QUuid view;
    bool operator==(const DraftReference&) const = default;
};

struct DimensionNote {
    QString fit;
    QString upper;
    QString lower;
    bool operator==(const DimensionNote&) const = default;
};

struct DraftTable {
    int columns = 1;
    double width = 60;
    double rowHeight = 6;
    QStringList cells;
    bool operator==(const DraftTable&) const = default;
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
    std::vector<std::vector<QUuid>> hatchBoundaries;
    std::vector<DraftItem> fragment;
    std::optional<DraftTable> table;
    DimensionNote dimensionNote;
    bool weldReverse = false;
    // Resolved dimensions carry presentation state. The file stores the references.
    double measurementScale = 1;
    bool lostReference = false;
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

enum class TitleField { Text, Title, Designation, Author, Material, Organization, Mass, Page, Pages };

struct TitleBlockRow {
    QString label;
    TitleField field = TitleField::Text;
    QString text;
    bool operator==(const TitleBlockRow&) const = default;
};

struct TitleBlockLayout {
    double width = 130;
    double rowHeight = 6;
    std::vector<TitleBlockRow> rows;
    bool operator==(const TitleBlockLayout&) const = default;
};

struct DraftPaper {
    bool portrait = false;
    bool eskd = false;
    QString material;
    QString organization;
    QString requirements;
    bool modelTitle = false;
    std::optional<TitleBlockLayout> titleBlock;
    bool operator==(const DraftPaper&) const = default;
};

struct FlatPatternReference {
    QString sourcePath;
    int objectId;
    std::vector<QUuid> geometry;
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
    std::optional<FlatPatternReference> flatPattern;
};

bool isAnnotation(DraftKind kind);
bool layerVisible(const Draft& draft, int layer);
bool layerEditable(const Draft& draft, int layer);

std::span<const DraftAnchor> draftAnchors(DraftKind kind);
std::optional<QPointF> draftAnchorPoint(const DraftItem& item, DraftAnchor anchor);
bool isLinearDimension(DraftKind kind);
bool isDimension(DraftKind kind);
bool isModelReference(const DraftReference& reference);

struct ModelPick {
    QPointF position;
    DraftReference reference;
};
std::optional<ModelPick> pickModelGeometry(const ModelViews& model, QPointF page,
                                           double tolerance, bool circularOnly = false);
QString draftDimensionText(const DraftItem& item);
std::vector<QLineF> dimensionSegments(const DraftItem& item);
QPointF dimensionTextPoint(const DraftItem& item);

class DraftGeometry {
public:
    explicit DraftGeometry(const Draft& draft, const DraftItem* preview = nullptr);
    std::optional<QPointF> point(const DraftReference& reference) const;
    bool validReference(const DraftReference& reference) const;
    DraftItem resolve(const DraftItem& item) const;

private:
    QHash<QUuid, const DraftItem*> sources_;
    const ModelViews* model_ = nullptr;
};

QSizeF draftSize(const Draft& draft);
bool validItem(const DraftItem& item);
bool validTitleBlock(const TitleBlockLayout& layout);
QString draftTitleField(const Draft& draft, TitleField field, int page, int pages);
QRectF draftItemBounds(const DraftItem& item);
DraftItem translateDraftItem(DraftItem item, QPointF delta);
void paintDraftItem(QPainter& painter, const DraftItem& item, bool selected = false);
bool draftHit(const DraftItem& item, QPointF point, double tolerance);
void paintDraft(QPainter& painter, const Draft& draft, bool grid = false, int selected = -1,
                const DraftItem* preview = nullptr, int sheet = 1, int total = 1);
void exportDraftPdf(const QString& path, const Draft& draft);

}  // namespace graph3d::drawing
