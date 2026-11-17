#include "io/DxfIO.h"

#include "drawing/DraftCurves.h"
#include "io/FileIO.h"

#include <QFileInfo>
#include <QHash>
#include <QLineF>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>

namespace graph3d::io {
namespace {

using namespace drawing;
constexpr double turn = 2 * std::numbers::pi;
constexpr int maxItems = 20000;
constexpr int maxPoints = 750000;

struct Pair {
    int code;
    QString value;
};
using Record = std::span<const Pair>;

[[noreturn]] void invalid()
{
    fileError(QStringLiteral("Invalid or oversized ASCII DXF."));
}

QString value(Record record, int code, QString fallback = {})
{
    const auto found = std::ranges::find(record, code, &Pair::code);
    return found == record.end() ? fallback : found->value;
}

bool has(Record record, int code)
{
    return std::ranges::find(record, code, &Pair::code) != record.end();
}

double number(const QString& value)
{
    bool ok = false;
    const auto result = value.toDouble(&ok);
    if (!ok || !std::isfinite(result) || std::abs(result) > 1.e12) {
        invalid();
    }
    return result;
}

double number(Record record, int code, double fallback = 0)
{
    return has(record, code) ? number(value(record, code)) : fallback;
}

int integer(Record record, int code, int fallback = 0)
{
    const auto result = number(record, code, fallback);
    if (std::abs(result) > 1.e8 || std::floor(result) != result) {
        invalid();
    }
    return static_cast<int>(result);
}

QString decoded(QString text)
{
    static const QRegularExpression escapes(QStringLiteral(R"(\\U\+([A-Fa-f0-9]{4}))"));
    auto matches = escapes.globalMatch(text);
    QList<QRegularExpressionMatch> all;
    while (matches.hasNext()) {
        all.push_back(matches.next());
    }
    for (auto i = all.crbegin(); i != all.crend(); ++i) {
        text.replace(i->capturedStart(), i->capturedLength(), QChar(i->captured(1).toUShort(nullptr, 16)));
    }
    text.replace(QStringLiteral("%%d"), QStringLiteral("°"), Qt::CaseInsensitive);
    text.replace(QStringLiteral("%%c"), QStringLiteral("Ø"), Qt::CaseInsensitive);
    text.replace(QStringLiteral("%%p"), QStringLiteral("±"), Qt::CaseInsensitive);
    return text.replace(QChar::Null, QChar::ReplacementCharacter);
}

QString encoded(const QString& text)
{
    QString result;
    for (const auto character : text) {
        if (character.unicode() < 32) {
            result += u' ';
        } else if (character == u'\\' || character == u'%') {
            result += QStringLiteral("\\U+%1").arg(static_cast<unsigned>(character.unicode()), 4, 16, u'0').toUpper();
        } else {
            result += character;
        }
    }
    return result;
}

std::vector<Pair> parse(const QByteArray& bytes)
{
    if (bytes.startsWith("AutoCAD Binary DXF")) {
        fileError(QStringLiteral("Use an ASCII DXF file."));
    }
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8.decode(bytes);
    if (utf8.hasError()) {
        text = QString::fromLocal8Bit(bytes);
    }
    const auto lines = QStringView(text).split(u'\n');
    qsizetype length = lines.size();
    while (length > 0 && lines[length - 1].trimmed().isEmpty()) {
        --length;
    }
    if (length % 2 != 0 || length > 4'000'000) {
        invalid();
    }
    std::vector<Pair> result;
    result.reserve(static_cast<std::size_t>(length / 2));
    for (qsizetype i = 0; i < length; i += 2) {
        bool ok = false;
        const auto code = lines[i].trimmed().toInt(&ok);
        auto content = lines[i + 1];
        if (content.endsWith(u'\r')) {
            content.chop(1);
        }
        if (!ok || code < 0 || code > 1071 || content.size() > 32768) {
            invalid();
        }
        result.push_back({code, (code == 1 ? content : content.trimmed()).toString()});
    }
    if (result.empty() || result.back().code != 0 || result.back().value != "EOF") {
        invalid();
    }
    return result;
}

QHash<QString, Record> sections(Record file)
{
    QHash<QString, Record> result;
    for (std::size_t i = 0; i + 1 < file.size(); ++i) {
        if (file[i].code != 0 || file[i].value != "SECTION") {
            continue;
        }
        if (file[i + 1].code != 2 || result.contains(file[i + 1].value)) {
            invalid();
        }
        const auto first = i + 2;
        i = first;
        while (i < file.size() && !(file[i].code == 0 && file[i].value == "ENDSEC")) {
            ++i;
        }
        if (i == file.size()) {
            invalid();
        }
        result.insert(file[first - 1].value, file.subspan(first, i - first));
    }
    return result;
}

std::vector<Record> records(Record section)
{
    std::vector<Record> result;
    for (std::size_t i = 0; i < section.size();) {
        const auto start = i++;
        while (i < section.size() && section[i].code != 0) {
            ++i;
        }
        if (section[start].code == 0) {
            result.push_back(section.subspan(start, i - start));
        }
    }
    return result;
}

double mmPerUnit(Record header)
{
    for (std::size_t i = 0; i + 1 < header.size(); ++i) {
        if (header[i].code != 9 || header[i].value != "$INSUNITS") {
            continue;
        }
        const auto unit = integer(header.subspan(i + 1, 1), 70, -1);
        constexpr std::array units{1., 25.4, 304.8, 1609344., 1., 10., 1000., 1.e6, .0000254, .0254,
            914.4, 1.e-7, 1.e-6, .001, 100., 10000., 100000., 1.e12, 1.495978707e14, 9.4607304725808e18,
            3.08567758149137e19, 1200000./3937, 100000./3937, 3600000./3937, 6336000000./3937};
        if (unit < 0 || unit >= static_cast<int>(units.size())) {
            invalid();
        }
        return units[unit];
    }
    return 1;
}

DraftStroke strokeType(const QString& text)
{
    const auto n = text.toUpper();
    if (n.contains("CENTER") || n.contains("CENTRE") || n.contains("DASHDOT")) {
        return DraftStroke::Center;
    }
    if (n.contains("HIDDEN") || n.contains("DASH")) {
        return DraftStroke::Hidden;
    }
    return DraftStroke::Solid;
}

struct Vertex {
    QPointF point;
    double bulge = 0;
};

class Reader {
public:
    explicit Reader(Record header) : scale(mmPerUnit(header)) {}

    int layer(const QString& raw)
    {
        const auto name = decoded(raw);
        const auto key = name.toCaseFolded();
        if (const auto old = layers.constFind(key); old != layers.cend()) {
            return *old;
        }
        if (result.drawing.layers.size() >= maxLayers || name.size() > 200) {
            fileError(QStringLiteral("DXF contains too many layers or a layer name is too long."));
        }
        const int index = static_cast<int>(result.drawing.layers.size());
        result.drawing.layers.push_back({name});
        layers.insert(key, index);
        return index;
    }

    void layerTable(const std::vector<Record>& table)
    {
        for (auto entry : table) {
            if (entry.front().value == "LTYPE") {
                const auto name = value(entry, 2).toUpper();
                auto stroke = strokeType(name);
                if (stroke == DraftStroke::Solid) {
                    const auto elements = integer(entry, 73);
                    stroke = elements >= 4 ? DraftStroke::Center : elements >= 2 ? DraftStroke::Hidden : DraftStroke::Solid;
                }
                styles.insert(name, stroke);
            } else if (entry.front().value == "LAYER") {
                const int index = layer(value(entry, 2, QStringLiteral("0")));
                auto& info = result.drawing.layers[index];
                const auto flags = integer(entry, 70);
                info.visible = !(flags & 1) && integer(entry, 62, 7) >= 0;
                info.locked = flags & 4;
                layerStyles.insert(index, value(entry, 6).toUpper());
            }
        }
    }

    QPointF coordinate(Record record, int code = 10) const
    {
        if (!has(record, code) || !has(record, code + 10)) {
            invalid();
        }
        return {number(record, code) * scale, -number(record, code + 10) * scale};
    }

    void add(DraftItem item)
    {
        if (result.drawing.items.size() >= maxItems || pointBudget < 0) {
            invalid();
        }
        // Bounds are checked after the entire import has been positioned on paper.
        if (!std::isfinite(item.a.x()) || !std::isfinite(item.a.y())) {
            invalid();
        }
        item.id = QUuid::createUuid();
        result.drawing.items.push_back(std::move(item));
    }

    void polyline(DraftItem item, std::vector<Vertex> vertices, bool closed)
    {
        pointBudget -= static_cast<int>(vertices.size());
        if (vertices.size() < 2 || vertices.size() > 19999 || pointBudget < 0) {
            invalid();
        }
        if (closed && vertices.front().point != vertices.back().point) {
            vertices.push_back(vertices.front());
        }
        if (std::ranges::none_of(vertices | std::views::take(vertices.size() - 1), [](const auto& v) { return v.bulge != 0; })) {
            item.kind = DraftKind::Polyline;
            for (const auto& vertex : vertices) {
                if (item.points.empty() || QLineF(item.points.back(), vertex.point).length() > draftTolerance) {
                    item.points.push_back(vertex.point);
                }
            }
            add(std::move(item));
            return;
        }
        for (std::size_t i = 1; i < vertices.size(); ++i) {
            const auto& start = vertices[i - 1];
            const auto end = vertices[i].point;
            auto edge = item;
            edge.a = start.point;
            edge.b = end;
            edge.kind = DraftKind::Line;
            if (start.bulge != 0) {
                const auto chord = end - start.point;
                edge.kind = DraftKind::Arc;
                edge.b = (start.point + end) / 2 + QPointF(-chord.y(), chord.x()) * (start.bulge / 2);
                edge.label = end;
            }
            add(std::move(edge));
        }
    }

    std::vector<Vertex> vertices(Record fields, int vertexCount) const
    {
        std::vector<Vertex> result;
        std::size_t i = 0;
        while (i < fields.size()) {
            if (fields[i].code != 10) {
                ++i;
                continue;
            }
            const auto start = i++;
            while (i < fields.size() && fields[i].code != 10) {
                ++i;
            }
            const auto vertex = fields.subspan(start, i - start);
            result.push_back({coordinate(vertex), number(vertex, 42)});
        }
        if (static_cast<int>(result.size()) != vertexCount) {
            invalid();
        }
        return result;
    }

    bool hatch(DraftItem item, Record record)
    {
        if (value(record, 2) != "ANSI31" || integer(record, 70) != 0) {
            return false;
        }
        const int count = integer(record, 91);
        if (count < 1 || count > maxItems) {
            invalid();
        }
        auto first = std::ranges::find(record, 92, &Pair::code);
        for (int i = 0; i < count; ++i) {
            if (first == record.end()) {
                invalid();
            }
            auto end = first + 1;
            while (end != record.end() && end->code != 92 && end->code != 97 && end->code != 75) {
                ++end;
            }
            const Record path(first, end);
            if (!(integer(path, 92) & 2) || integer(path, 72) || !integer(path, 73)) {
                return false;
            }
            const int count = integer(path, 93);
            if (count < 3 || count >= 50000 || (pointBudget -= count) < 0) {
                invalid();
            }
            auto& loop = item.loops.emplace_back();
            for (const auto& vertex : vertices(path, count)) {
                loop.push_back(vertex.point);
            }
            if (loop.front() != loop.back()) {
                loop.push_back(loop.front());
            }
            first = std::ranges::find(std::ranges::subrange(end, record.end()), 92, &Pair::code);
        }
        item.kind = DraftKind::Hatch;
        add(std::move(item));
        return true;
    }

    void entity(Record record, const std::vector<Vertex>* legacy = nullptr)
    {
        const auto type = record.front().value;
        if (integer(record, 67) != 0 || number(record, 210) != 0 || number(record, 220) != 0
            || number(record, 230, 1) != 1 || number(record, 38) != 0 || number(record, 39) != 0
            || number(record, 30) != 0 || (type == "LINE" && number(record, 31) != 0)) {
            ++result.skipped;
            return;
        }
        if (type != "LINE" && type != "CIRCLE" && type != "ARC" && type != "TEXT"
            && type != "LWPOLYLINE" && type != "POLYLINE" && type != "HATCH") {
            ++result.skipped;
            return;
        }
        DraftItem item;
        item.layer = layer(value(record, 8, QStringLiteral("0")));
        auto lineType = value(record, 6).toUpper();
        if (lineType.isEmpty() || lineType == "BYLAYER" || lineType == "BYBLOCK") {
            lineType = layerStyles.value(item.layer);
        }
        item.stroke = styles.value(lineType, strokeType(lineType));
        if (type == "LINE") {
            item.kind = DraftKind::Line;
            item.a = coordinate(record);
            item.b = coordinate(record, 11);
        } else if (type == "CIRCLE" || type == "ARC") {
            const auto center = coordinate(record);
            const auto radius = number(record, 40) * scale;
            if (!(radius > 0)) {
                invalid();
            }
            item.kind = DraftKind::Circle;
            item.a = center;
            item.b = center + QPointF(radius, 0);
            if (type == "ARC") {
                const auto start = number(record, 50) * std::numbers::pi / 180;
                const auto finish = number(record, 51) * std::numbers::pi / 180;
                const auto sweep = std::fmod(std::fmod(finish - start, turn) + turn, turn);
                if (sweep <= 1.e-10) {
                    invalid();
                }
                const auto at = [&](double angle) { return center + QPointF(std::cos(angle), -std::sin(angle)) * radius; };
                item.kind = DraftKind::Arc;
                item.a = at(start);
                item.b = at(start + sweep / 2);
                item.label = at(start + sweep);
            }
        } else if (type == "TEXT") {
            item.kind = DraftKind::Text;
            if (number(record, 50) != 0 || integer(record, 72) != 0 || integer(record, 73) != 0) {
                ++result.skipped;
                return;
            }
            item.a = coordinate(record);
            item.text = decoded(value(record, 1));
        } else if (type == "HATCH") {
            if (!hatch(std::move(item), record)) {
                ++result.skipped;
            }
            return;
        } else {
            if (type == "POLYLINE" && (!legacy || integer(record, 70) & 126)) {
                ++result.skipped;
                return;
            }
            polyline(std::move(item), legacy ? *legacy : vertices(record, integer(record, 90)), integer(record, 70) & 1);
            return;
        }
        add(std::move(item));
    }

    DxfDrawing result;
    double scale;
    int pointBudget = maxPoints;
    QHash<QString, int> layers{{QStringLiteral("0"), 0}};
    QHash<QString, DraftStroke> styles;
    QHash<int, QString> layerStyles;
};

class Writer {
public:
    void pair(int code, const QString& value) { data += QByteArray::number(code) + '\n' + value.toUtf8() + '\n'; }
    void pair(int code, const char* value) { pair(code, QString::fromLatin1(value)); }
    void pair(int code, int value) { pair(code, QString::number(value)); }
    void pair(int code, double value) { pair(code, QString::number(value, 'g', 16)); }
    void section(const char* name) { pair(0, "SECTION"); pair(2, name); }
    void handle() { pair(5, QString::number(nextHandle++, 16).toUpper()); }

    void table(const char* name, int count)
    {
        pair(0, "TABLE");
        pair(2, name);
        handle();
        pair(100, "AcDbSymbolTable");
        pair(70, count);
    }

    void point(QPointF p, int code = 10)
    {
        pair(code, p.x());
        pair(code + 10, -p.y());
    }

    void entity(const char* name, const char* type, const QString& layer, DraftStroke stroke)
    {
        pair(0, name);
        handle();
        pair(100, "AcDbEntity");
        pair(8, layer);
        pair(6, std::array{"CONTINUOUS", "HIDDEN", "CENTER"}.at(static_cast<unsigned>(stroke)));
        pair(100, type);
    }

    void path(std::span<const QPointF> points, bool closed, const QString& layer, DraftStroke stroke)
    {
        entity("LWPOLYLINE", "AcDbPolyline", layer, stroke);
        pair(90, static_cast<double>(points.size()));
        pair(70, closed ? 1 : 0);
        for (const auto p : points) {
            point(p);
        }
    }

    bool write(const DraftItem& item, const QString& layer)
    {
        switch (item.kind) {
        case DraftKind::Line:
            entity("LINE", "AcDbLine", layer, item.stroke);
            point(item.a);
            point(item.b, 11);
            break;
        case DraftKind::Rectangle: {
            const std::array corners{item.a, QPointF(item.b.x(), item.a.y()), item.b, QPointF(item.a.x(), item.b.y())};
            path(corners, true, layer, item.stroke);
            break;
        }
        case DraftKind::Polyline: {
            const bool closed = item.points.front() == item.points.back();
            path(std::span(item.points).first(item.points.size() - (closed ? 1 : 0)), closed, layer, item.stroke);
            break;
        }
        case DraftKind::Circle:
            entity("CIRCLE", "AcDbCircle", layer, item.stroke);
            point(item.a);
            pair(40, QLineF(item.a, item.b).length());
            break;
        case DraftKind::Arc: {
            const auto curve = draftArc(item.a, item.b, item.label);
            const auto angle = [](double radians) {
                return std::fmod(std::fmod(-radians * 180 / std::numbers::pi, 360.0) + 360.0, 360.0);
            };
            entity("ARC", "AcDbCircle", layer, item.stroke);
            point(curve.a);
            pair(40, curve.radius);
            pair(100, "AcDbArc");
            pair(50, angle(curve.sweep < 0 ? curve.angle : curve.angle + curve.sweep));
            pair(51, angle(curve.sweep < 0 ? curve.angle + curve.sweep : curve.angle));
            break;
        }
        case DraftKind::Text:
            entity("TEXT", "AcDbText", layer, item.stroke);
            point(item.a);
            pair(40, 4);
            pair(1, encoded(item.text));
            pair(100, "AcDbText");
            break;
        case DraftKind::Hatch:
            entity("HATCH", "AcDbHatch", layer, item.stroke);
            point({});
            pair(30, 0);
            pair(210, 0); pair(220, 0); pair(230, 1);
            pair(2, "ANSI31"); pair(70, 0); pair(71, 0);
            pair(91, static_cast<double>(item.loops.size()));
            for (const auto& loop : item.loops) {
                pair(92, 2); pair(72, 0); pair(73, 1); pair(93, static_cast<double>(loop.size() - 1));
                for (const auto p : std::span(loop).first(loop.size() - 1)) {
                    point(p);
                }
                pair(97, 0);
            }
            pair(75, 0); pair(76, 1); pair(52, 0); pair(41, 1); pair(77, 0); pair(78, 1);
            pair(53, 45); pair(43, 0); pair(44, 0); pair(45, -2.121320343559643); pair(46, 2.121320343559643);
            pair(79, 0); pair(47, .01); pair(98, 0);
            break;
        default:
            return false;
        }
        return true;
    }

    void lineTable(const char* name, std::initializer_list<double> segments)
    {
        pair(0, "LTYPE"); handle(); pair(100, "AcDbSymbolTableRecord"); pair(100, "AcDbLinetypeTableRecord");
        pair(2, name); pair(70, 0); pair(3, name); pair(72, 65); pair(73, static_cast<double>(segments.size()));
        double total = 0;
        for (const double segment : segments) {
            total += std::abs(segment);
        }
        pair(40, total);
        for (const double segment : segments) {
            pair(49, segment); pair(74, 0);
        }
    }

    void projectedEdges(const Linework& edges, const ViewPlacement& place, const QString& layer, DraftStroke stroke)
    {
        for (const auto& edge : edges) {
            std::vector<QPointF> points;
            points.reserve(edge.size());
            for (const auto p : edge) {
                const auto at = place.center + QPointF(p.x, -p.y) * place.scale;
                if (points.empty() || QLineF(points.back(), at).length() > draftTolerance) {
                    points.push_back(at);
                }
            }
            if (points.size() > 1) {
                const bool closed = points.size() > 3 && QLineF(points.front(), points.back()).length() <= draftTolerance;
                if (closed) {
                    points.pop_back();
                }
                path(points, closed, layer, stroke);
            }
        }
    }

    void projectedHatches(const std::vector<Linework>& material, const ViewPlacement& place, const QString& layer)
    {
        for (const auto& region : material) {
            DraftItem hatch{DraftKind::Hatch};
            for (const auto& contour : region) {
                if (contour.size() < 3) {
                    continue;
                }
                auto& loop = hatch.loops.emplace_back();
                for (const auto p : contour) {
                    loop.push_back(place.center + QPointF(p.x, -p.y) * place.scale);
                }
                if (loop.front() != loop.back()) {
                    loop.push_back(loop.front());
                }
            }
            if (validItem(hatch)) {
                write(hatch, layer);
            }
        }
    }

    bool projectedViews(const ModelViews& model, const QString& layer)
    {
        if (!model.geometry) {
            return false;
        }
        const auto firstHandle = nextHandle;
        const auto& [front, top, right] = model.geometry->views;
        const std::array views{&front, &right, &top};
        using geometry::Plane;
        constexpr std::array planes{Plane::XZ, Plane::YZ, Plane::XY};

        for (std::size_t i = 0; i < views.size(); ++i) {
            if (!model.showBase[i]) {
                continue;
            }
            const auto& place = model.layout[i];
            const auto& section = model.geometry->section;
            const bool fullSection = section && section->plane == planes[i] && !model.local;
            if (!fullSection && model.hiddenLines) {
                projectedEdges(views[i]->hidden, place, layer, DraftStroke::Hidden);
            }
            projectedEdges(fullSection ? section->view.visible : views[i]->visible, place, layer, DraftStroke::Solid);
            if (fullSection) {
                projectedHatches(section->material, place, layer);
            }
        }
        for (const auto& extra : model.additional) {
            if (!extra.geometry || extra.kind == DrawingViewKind::Detail || extra.kind == DrawingViewKind::Local) {
                continue;
            }
            const auto& view = extra.geometry->view;
            if (model.hiddenLines) {
                projectedEdges(view.hidden, extra.placement, layer, DraftStroke::Hidden);
            }
            projectedEdges(view.visible, extra.placement, layer, DraftStroke::Solid);
            projectedHatches(extra.geometry->material, extra.placement, layer);
        }
        return nextHandle > firstHandle;
    }

    QByteArray data;
    int nextHandle = 256;
};

QRectF boundsOf(const DraftItem& item)
{
    if (item.kind == DraftKind::Hatch) {
        QRectF result;
        bool first = true;
        for (const auto& loop : item.loops) {
            const auto bounds = QPolygonF(QList<QPointF>(loop.begin(), loop.end())).boundingRect();
            result = first ? bounds : result.united(bounds);
            first = false;
        }
        return result;
    }
    if (item.kind == DraftKind::Text) {
        return QRectF(item.a - QPointF(0, 4), QSizeF(item.text.size() * 2.2, 4));
    }
    const auto path = draftCurvePath(item);
    return path.boundingRect();
}

void fitImported(DxfDrawing& result)
{
    if (result.drawing.items.empty()) {
        fileError(QStringLiteral("DXF contains no supported 2D geometry."));
    }
    auto bounds = boundsOf(result.drawing.items.front());
    for (const auto& item : result.drawing.items) {
        const auto other = boundsOf(item);
        bounds.setCoords(std::min(bounds.left(), other.left()), std::min(bounds.top(), other.top()),
            std::max(bounds.right(), other.right()), std::max(bounds.bottom(), other.bottom()));
    }
    result.drawing.a3 = bounds.width() > 255 || bounds.height() > 166;
    const auto page = draftSize(result.drawing);
    result.beyondSheet = bounds.width() > page.width() - 35 || bounds.height() > page.height() - 40;
    const bool fits = QRectF(20, 5, page.width() - 25, page.height() - 10).contains(bounds);
    if (!fits) {
        const auto shift = QPointF(25, 10) - bounds.topLeft();
        for (auto& item : result.drawing.items) {
            switch (item.kind) {
            case DraftKind::Line:
            case DraftKind::Circle: item.b += shift; [[fallthrough]];
            case DraftKind::Text: item.a += shift; break;
            case DraftKind::Arc: item.a += shift; item.b += shift; item.label += shift; break;
            default: break;
            }
            for (auto& point : item.points) {
                point += shift;
            }
            for (auto& loop : item.loops) {
                for (auto& point : loop) {
                    point += shift;
                }
            }
        }
    }
    for (const auto& item : result.drawing.items) {
        if (!validItem(item)) {
            fileError(QStringLiteral("DXF contains invalid or out-of-range drawing geometry."));
        }
    }
}

}  // namespace

DxfDrawing importDxf(const QString& path)
{
    const auto file = parse(readFile(path));
    const auto content = sections(file);
    if (!content.contains(QStringLiteral("ENTITIES"))) {
        invalid();
    }
    Reader reader(content.value(QStringLiteral("HEADER")));
    reader.layerTable(records(content.value(QStringLiteral("TABLES"))));
    const auto entities = records(content.value(QStringLiteral("ENTITIES")));
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const auto entry = entities[i];
        if (entry.front().value == "POLYLINE") {
            std::vector<Vertex> vertices;
            bool planar = true;
            while (i + 1 < entities.size() && entities[i + 1].front().value == "VERTEX") {
                const auto vertex = entities[++i];
                vertices.push_back({reader.coordinate(vertex), number(vertex, 42)});
                planar = planar && number(vertex, 30) == 0;
                if (vertices.size() > 19999) {
                    invalid();
                }
            }
            if (i + 1 == entities.size() || entities[++i].front().value != "SEQEND") {
                invalid();
            }
            reader.entity(entry, planar ? &vertices : nullptr);
        } else {
            reader.entity(entry);
        }
    }
    fitImported(reader.result);
    reader.result.drawing.title = QFileInfo(path).completeBaseName().left(200);
    return std::move(reader.result);
}

int exportDxf(const QString& path, const Draft& sheet)
{
    Writer writer;
    writer.section("HEADER");
    writer.pair(9, "$ACADVER"); writer.pair(1, "AC1021");
    writer.pair(9, "$INSUNITS"); writer.pair(70, 4);
    writer.pair(9, "$MEASUREMENT"); writer.pair(70, 1);
    writer.pair(0, "ENDSEC");
    writer.section("TABLES");
    writer.table("LTYPE", 3);
    writer.lineTable("CONTINUOUS", {});
    writer.lineTable("HIDDEN", {3, -1.5});
    writer.lineTable("CENTER", {8, -2, 1.5, -2});
    writer.pair(0, "ENDTAB");

    QStringList names;
    QSet<QString> used;
    static const QRegularExpression unsafe(QStringLiteral(R"([<>/\\":;?*|=,`])"));
    for (int i = 0; i < static_cast<int>(sheet.layers.size()); ++i) {
        auto name = sheet.layers[i].name.trimmed().replace(unsafe, QStringLiteral("_"));
        if (name.isEmpty()) {
            name = i == 0 ? QStringLiteral("0") : QStringLiteral("Layer_%1").arg(i + 1);
        }
        const auto base = name;
        for (int suffix = 2; used.contains(name.toCaseFolded()); ++suffix) {
            name = base.left(190) + QStringLiteral("_%1").arg(suffix);
        }
        used.insert(name.toCaseFolded());
        names.push_back(encoded(name));
    }
    writer.table("LAYER", static_cast<int>(names.size()));
    for (qsizetype i = 0; i < names.size(); ++i) {
        writer.pair(0, "LAYER"); writer.handle(); writer.pair(100, "AcDbSymbolTableRecord"); writer.pair(100, "AcDbLayerTableRecord");
        writer.pair(2, names[i]); writer.pair(70, sheet.layers[i].locked ? 4 : 0);
        writer.pair(62, sheet.layers[i].visible ? 7 : -7); writer.pair(6, "CONTINUOUS");
    }
    writer.pair(0, "ENDTAB"); writer.pair(0, "ENDSEC");
    writer.section("BLOCKS"); writer.pair(0, "ENDSEC");
    writer.section("ENTITIES");
    int skipped = 0;
    const bool projected = sheet.modelViews && !names.isEmpty() && writer.projectedViews(*sheet.modelViews, names.front());
    const DraftGeometry geometry(sheet);
    for (const auto& item : sheet.items) {
        if (!validItem(item) || item.layer < 0 || item.layer >= names.size()) {
            invalid();
        }
        const auto exported = item.kind == DraftKind::Hatch ? geometry.resolve(item) : item;
        if (exported.lostReference || !writer.write(exported, names[item.layer])) {
            ++skipped;
        }
    }
    if (!projected && sheet.items.size() == static_cast<std::size_t>(skipped)) {
        fileError(QStringLiteral("This sheet contains no supported geometry to export to DXF."));
    }
    writer.pair(0, "ENDSEC"); writer.pair(0, "EOF");
    if (writer.data.size() > fileLimit) {
        invalid();
    }
    writeFile(path, writer.data);
    const auto& model = sheet.modelViews;
    return skipped + (model && (!projected || model->dimensions || model->detail || model->local || !model->additional.empty()
                                || (model->assembly && !model->assembly->callouts.empty())) ? 1 : 0);
}

}  // namespace graph3d::io
