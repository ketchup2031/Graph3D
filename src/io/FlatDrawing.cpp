#include "io/FlatDrawing.h"
#include "io/FileIO.h"
#include "io/ProjectIO.h"
#include <QCoreApplication>
#include <QSet>
#include <algorithm>
#include <numeric>

namespace graph3d::io {
drawing::Draft flatDrawing(const geometry::SheetMetal& sheet)
{
    using namespace drawing;
    const auto flat = geometry::flattenSheet(sheet);
    const auto tr = [](const char* message) { return QCoreApplication::translate("FlatDrawing",message); };
    Draft draft;
    draft.title = tr("Sheet metal development");
    draft.a3 = flat.length > 220 || flat.width > 140;
    draft.layers = {{"CUT"},{"BEND"},{"NOTES"}};
    constexpr double x0=25,y0=25;
    const auto at = [](double x,double y){return QPointF(x0+x,y0+y);};
    auto bends = flat.bends;
    std::ranges::sort(bends,{},&geometry::FlatBend::begin);
    DraftItem outline{DraftKind::Polyline};
    outline.points.push_back(at(0,0));
    for (const auto& b : bends) {
        if (b.relief > 0) {
            outline.points.insert(outline.points.end(),{at(b.begin,0),at(b.begin,b.relief),at(b.end,b.relief),at(b.end,0)});
        }
    }
    outline.points.insert(outline.points.end(),{at(flat.length,0),at(flat.length,flat.width)});
    for (auto i=bends.rbegin();i!=bends.rend();++i) {
        const auto& b=*i;
        if (b.relief > 0) {
            outline.points.insert(outline.points.end(),{at(b.end,flat.width),at(b.end,flat.width-b.relief),
                at(b.begin,flat.width-b.relief),at(b.begin,flat.width)});
        }
    }
    outline.points.insert(outline.points.end(),{at(0,flat.width),at(0,0)});
    draft.items.push_back(std::move(outline));
    for (const auto& hole : flat.holes) {
        draft.items.push_back({DraftKind::Circle,at(hole.x,hole.y),at(hole.x+hole.radius,hole.y)});
    }
    for (const auto& b : bends) {
        double x=std::midpoint(b.begin,b.end);
        DraftItem line{DraftKind::Line,at(x,b.relief),at(x,flat.width-b.relief)};
        line.stroke=DraftStroke::Center; line.layer=1;
        draft.items.push_back(std::move(line));
    }
    for (auto& item : draft.items) { item.id=QUuid::createUuid(); }
    draft.paper.requirements=tr("Blank: %1 × %2 mm. Sheet thickness: %3 mm.").arg(flat.length,0,'f',2).arg(flat.width).arg(sheet.thickness);
    return draft;
}

drawing::Draft refreshFlatDrawing(drawing::Draft draft)
{
    if (!draft.flatPattern) { fileError(QStringLiteral("This drawing has no sheet metal source.")); }
    auto& reference = *draft.flatPattern;
    const auto document = openProject(reference.sourcePath);
    const auto* object = document.find(reference.objectId);
    if (!object || object->status != document::Status::Ready
        || !std::holds_alternative<geometry::SheetMetal>(object->parameters)) {
        fileError(QStringLiteral("The source sheet metal part is missing or has an error."));
    }
    auto replacement = flatDrawing(std::get<geometry::SheetMetal>(document.effectiveParameters(reference.objectId)));
    int layers[2];
    for (int i = 0; i < 2; ++i) {
        const auto& name = replacement.layers[i].name;
        const auto layer = std::ranges::find(draft.layers, name, &drawing::DraftLayer::name);
        layers[i] = static_cast<int>(layer - draft.layers.begin());
        if (layer == draft.layers.end()) {
            if (draft.layers.size() >= drawing::maxLayers) {
                fileError(QStringLiteral("There is no space for the CUT and BEND layers."));
            }
            draft.layers.push_back(replacement.layers[i]);
        }
    }
    std::vector<const drawing::DraftItem*> previous;
    QSet<QUuid> generated;
    for (const auto& id : reference.geometry) {
        generated.insert(id);
        const auto item = std::ranges::find(draft.items, id, &drawing::DraftItem::id);
        previous.push_back(item == draft.items.end() ? nullptr : &*item);
    }
    const drawing::DraftGeometry oldGeometry(draft);
    std::vector<drawing::DraftItem> annotations;
    for (const auto& item : draft.items) {
        if (!generated.contains(item.id)) {
            annotations.push_back(oldGeometry.resolve(item));
        }
    }
    reference.geometry.clear();
    for (std::size_t i = 0; i < replacement.items.size(); ++i) {
        auto& item = replacement.items[i];
        item.layer = layers[item.layer];
        if (i < previous.size() && previous[i] && previous[i]->kind == item.kind
            && (i == 0 || previous.size() == replacement.items.size())) {
            item.id = previous[i]->id;
        }
        reference.geometry.push_back(item.id);
    }
    std::erase_if(draft.items, [&](const auto& item) {
        return generated.contains(item.id);
    });
    std::ranges::move(replacement.items, std::back_inserter(draft.items));
    const drawing::DraftGeometry updatedGeometry(draft);
    for (std::size_t i = 0; i < annotations.size(); ++i) {
        const auto& resolved = annotations[i];
        auto& item = draft.items[i];
        if (item.anchorA && !updatedGeometry.validReference(*item.anchorA)) { item.anchorA.reset(); item.a = resolved.a; }
        if (item.anchorB && !updatedGeometry.validReference(*item.anchorB)) { item.anchorB.reset(); item.b = resolved.b; }
        if (item.anchorC && !updatedGeometry.validReference(*item.anchorC)) {
            item.anchorC.reset();
            item.points = resolved.points;
        }
    }
    draft.paper.requirements = replacement.paper.requirements;
    draft.a3 = draft.a3 || replacement.a3;
    return draft;
}
}  // namespace graph3d::io
