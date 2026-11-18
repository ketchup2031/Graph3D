#pragma once

#include "drawing/Projection.h"

#include <QByteArray>
#include <QPointF>
#include <QSizeF>
#include <QString>

#include <array>
#include <memory>
#include <map>
#include <optional>

class QPainter;

namespace graph3d::drawing {

struct ViewPlacement {
    QPointF center;
    double scale;
    bool operator==(const ViewPlacement&) const = default;
};

// Placements are ordered front, right, top and measured in millimetres on paper.
using ViewLayout = std::array<ViewPlacement, 3>;

struct SectionSettings {
    geometry::Plane plane;
    double offset;
    bool operator==(const SectionSettings&) const = default;
};

struct ModelProjection {
    Projections views;
    std::optional<Section> section;
};

struct DetailView {
    int source = 0;
    QPointF center;
    double radius = 10;
    ViewPlacement placement{{140, 90}, 2};
    bool operator==(const DetailView&) const = default;
};

struct LocalSection {
    QPointF center;
    double radius = 12;
    bool operator==(const LocalSection&) const = default;
};

enum class DrawingViewKind {
    Front, Right, Top, Back, Left, Bottom, Isometric, Oblique,
    Projected, Arrow, SteppedSection, AlignedSection, Detail, Local
};

struct DrawingView {
    QUuid id;
    DrawingViewKind kind = DrawingViewKind::Front;
    QString name;
    QUuid source;
    ViewPlacement placement{{140, 90}, 1};
    std::array<double, 3> angles{};
    std::vector<QPointF> path; // Source model coordinates, positive Y upwards.
    QPointF focus;
    double radius = 12;
    double depth = 0;
    std::shared_ptr<const CutProjection> geometry;
};

constexpr int drawingViewLimit = 32;
bool derivedView(DrawingViewKind kind);
bool pathView(DrawingViewKind kind);
QString drawingViewName(DrawingViewKind kind);

struct SpecificationRow {
    QString key;
    int position = 0;
    QString designation, name, material, note;
    std::vector<std::array<QPointF, 3>> instances;
    std::vector<int> componentSlots;
    bool standard = false;
    std::optional<int> explicitQuantity;
    bool sectionTitle = false;
    bool operator==(const SpecificationRow&) const = default;
};

struct PositionCallout {
    int position = 0;
    int instance = 0;
    int view = 0;
    QPointF offset;
    QPointF label;
    int componentSlot = 0;
    bool operator==(const PositionCallout&) const = default;
};

struct AssemblyDrawing {
    QString name;
    QString designation;
    std::vector<SpecificationRow> rows;
    std::vector<PositionCallout> callouts;
    int nextPosition = 1;
    bool includeSpecification = true;
    std::map<QString, int> positionHistory;
    bool operator==(const AssemblyDrawing&) const = default;
};

struct ModelTitleData {
    QString name;
    QString designation;
    std::optional<double> massKg;
};
struct ThreadCallout {
    int view;
    QPointF center; // View coordinates, Y is upwards.
    double diameter;
    QString designation;
};

enum class ModelAnnotationKind { Linear, Diameter, Datum, Roughness };
struct AnnotationTransfer {
    int sourceId;
    int view;
    bool operator==(const AnnotationTransfer&) const = default;
};
struct ProjectedAnnotation {
    int sourceId;
    int view;
    ModelAnnotationKind kind;
    QPointF first, second;
    double value;
    QString text;
    bool faceOn;
};

struct ModelViews {
    QString sourcePath;
    int bodyId = 0;
    QByteArray sourceDigest;
    ViewLayout layout{};
    bool dimensions = true;
    bool hiddenLines = true;
    std::shared_ptr<const ModelProjection> geometry;
    std::optional<DetailView> detail;
    std::optional<LocalSection> local;
    std::shared_ptr<const AssemblyDrawing> assembly;
    std::array<bool, 3> showBase{true, true, true};
    std::vector<DrawingView> additional;
    ModelTitleData titleData;
    std::optional<QString> variant;
    std::vector<ThreadCallout> threads;
    QString specificationPath;
    std::vector<AnnotationTransfer> annotationSelection;
    std::vector<ProjectedAnnotation> annotations;
};

struct LocatedView {
    const View* view;
    ViewPlacement placement;
};

const View& baseModelView(const ModelViews& model, int index);
std::optional<LocatedView> locateModelView(const ModelViews& model, QUuid id);
void rebuildDrawingViews(ModelViews& model, const TopoDS_Shape& solid, const ModelViews* previous);
ViewLayout defaultViewLayout(const Projections& views, QSizeF paper);
std::optional<SectionSettings> sectionSettings(const ModelViews& model);
void paintModelViews(QPainter& painter, const ModelViews& model, bool eskd = false);

}  // namespace graph3d::drawing
