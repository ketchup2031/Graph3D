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
QString draftKindName(DraftKind kind)
{
    constexpr std::array titles{"LINE","RECTANGLE","CIRCLE","TEXT","HORIZONTAL DIMENSION","VERTICAL DIMENSION",
        "ARC","POLYLINE","ALIGNED DIMENSION","ANGULAR DIMENSION","RADIAL DIMENSION","DIAMETER DIMENSION",
        "HATCH","LEADER","DATUM","TOLERANCE","ROUGHNESS","ELLIPSE","SPLINE","CONSTRUCTION LINE","BLOCK","TABLE","THREAD","WELD"};
    return QString::fromLatin1(titles.at(static_cast<std::size_t>(kind)));
}
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
    void skip(QString type) { ++result.skipped; ++result.losses[type]; }
    QHash<QString,std::vector<Record>> blocks;
    QSet<QString> expanding;

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
        if (number(record, 210) != 0 || number(record, 220) != 0
            || number(record, 230, 1) != 1 || number(record, 38) != 0 || number(record, 39) != 0
            || number(record, 30) != 0 || (type == "LINE" && number(record, 31) != 0)) {
            skip(type);
            return;
        }
        if (type != "LINE" && type != "CIRCLE" && type != "ARC" && type != "TEXT"
            && type != "LWPOLYLINE" && type != "POLYLINE" && type != "HATCH"
            && type != "INSERT" && type != "DIMENSION") {
            skip(type);
            return;
        }
        DraftItem item;
        item.layer = layer(value(record, 8, QStringLiteral("0")));
        auto lineType = value(record, 6).toUpper();
        if (lineType.isEmpty() || lineType == "BYLAYER" || lineType == "BYBLOCK") {
            lineType = layerStyles.value(item.layer);
        }
        item.stroke = styles.value(lineType, strokeType(lineType));
        if (type == "INSERT") {
            const auto name=value(record,2).toUpper();
            if (!blocks.contains(name) || expanding.contains(name) || !expanding.isEmpty()
                || number(record,41,1)!=1 || number(record,42,1)!=1 || number(record,50)!=0
                || integer(record,70,1)!=1 || integer(record,71,1)!=1) { skip(type); return; }
            const auto& definition=blocks[name];
            if(definition.size()<2) {skip(type); return;}
            const auto origin=has(definition.front(),10) ? coordinate(definition.front()) : QPointF{};
            const auto first=result.drawing.items.size();
            expanding.insert(name);
            for(std::size_t i=1;i<definition.size();++i) entity(definition[i]);
            expanding.remove(name);
            item.kind=DraftKind::Fragment;
            item.a=coordinate(record);
            for(std::size_t i=first;i<result.drawing.items.size();++i) {
                auto child=std::move(result.drawing.items[i]);
                child.layer=0;
                child.a-=origin; child.b-=origin; child.label-=origin;
                for(auto& p:child.points) p-=origin;
                for(auto& loop:child.loops) for(auto& p:loop) p-=origin;
                item.fragment.push_back(std::move(child));
            }
            result.drawing.items.resize(first);
            if(item.fragment.empty()) {skip(type); return;}
        } else if (type == "DIMENSION") {
            const int mode=integer(record,70)&7;
            const auto rotation=std::remainder(number(record,50),180.);
            if(mode==0 || mode==1) {
                if(mode==0 && std::abs(rotation)>1.e-5 && std::abs(std::abs(rotation)-90)>1.e-5) {skip(type); return;}
                item.kind=mode==1 ? DraftKind::Aligned : std::abs(rotation)>45 ? DraftKind::Vertical : DraftKind::Horizontal;
                item.a=coordinate(record,13); item.b=coordinate(record,14); item.label=coordinate(record);
            } else if (mode==3 || mode==4) {
                item.kind=mode==3 ? DraftKind::Diameter : DraftKind::Radius;
                const auto a=coordinate(record), b=coordinate(record,15);
                item.a=mode==3 ? (a+b)/2 : a; item.b=b;
                item.label=has(record,11) ? coordinate(record,11) : b+QPointF(12,-8);
            } else if(mode==5 && has(record,15)) {
                item.kind=DraftKind::Angular;
                item.a=coordinate(record,15); item.b=coordinate(record,13);
                item.points={coordinate(record,14)}; item.label=coordinate(record);
            } else {skip(type); return;}
            const auto label=decoded(value(record,1));
            if(!label.isEmpty() && label!="<>") { item.text=label; }
        } else if (type == "LINE") {
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
                skip(type);
                return;
            }
            item.a = coordinate(record);
            item.text = decoded(value(record, 1));
        } else if (type == "HATCH") {
            if (!hatch(std::move(item), record)) {
                skip(type);
            }
            return;
        } else {
            if (type == "POLYLINE" && (!legacy || integer(record, 70) & 126)) {
                skip(type);
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
        if(!owner.isEmpty()) pair(330,owner);
        pair(100, "AcDbEntity");
        if (!layout.isEmpty()) { pair(67,1); pair(410,layout); }
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

    bool write(const DraftItem& item, const QString& layer, const QString& blockName = {})
    {
        switch (item.kind) {
        case DraftKind::Fragment:
            if (blockName.isEmpty()) { return false; }
            entity("INSERT","AcDbBlockReference",layer,item.stroke);
            pair(2,blockName); point(item.a); pair(30,0);
            break;
        case DraftKind::Horizontal: case DraftKind::Vertical: case DraftKind::Aligned:
        case DraftKind::Diameter: case DraftKind::Radius: case DraftKind::Angular: {
            const bool angular=item.kind==DraftKind::Angular;
            const bool radial=item.kind==DraftKind::Radius || item.kind==DraftKind::Diameter;
            if (blockName.isEmpty()) return false;
            entity("DIMENSION","AcDbDimension",layer,item.stroke);
            pair(2,blockName);
            auto label=item.label;
            if(item.kind==DraftKind::Horizontal) label.setX(item.b.x());
            if(item.kind==DraftKind::Vertical) label.setY(item.b.y());
            point(radial ? (item.kind==DraftKind::Diameter ? 2*item.a-item.b : item.a) : label); pair(30,0);
            point(item.label,11); pair(31,0);
            const int type=angular ? 5 : radial ? (item.kind==DraftKind::Diameter?3:4) : item.kind==DraftKind::Aligned?1:0;
            pair(70,32+type); pair(1,item.text.isEmpty()? QStringLiteral("<>"):encoded(item.text)); pair(3,"Standard");
            if(radial){
                pair(100,item.kind==DraftKind::Diameter?"AcDbDiametricDimension":"AcDbRadialDimension");
                point(item.b,15); pair(35,0); pair(40,QLineF(item.b,item.label).length());
            } else if(angular){
                pair(100,"AcDb3PointAngularDimension");
                point(item.b,13); pair(33,0); point(item.points.front(),14); pair(34,0);
                point(item.a,15); pair(35,0);
            } else {
                pair(100,"AcDbAlignedDimension"); point(item.a,13); pair(33,0); point(item.b,14); pair(34,0);
                if(type==0){pair(50,item.kind==DraftKind::Vertical?90:0); pair(100,"AcDbRotatedDimension");}
            }
            break;
        }
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
    QString layout;
    QString owner;
};

QRectF boundsOf(const DraftItem& item)
{
    if(item.kind==DraftKind::Fragment) {
        QRectF bound;
        bool first=true;
        for(const auto& piece:item.fragment) {
            const auto other=boundsOf(piece).translated(item.a);
            bound=first?other:bound.united(other); first=false;
        }
        return bound;
    }
    if(isDimension(item.kind)) {
        QPolygonF points{item.a,item.b,item.label};
        for(auto point:item.points) points<<point;
        return points.boundingRect();
    }
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
            item.a+=shift;
            if(item.kind!=DraftKind::Fragment) { item.b+=shift; item.label+=shift; }
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
    QString currentBlock;
    for(const auto& entry:records(content.value("BLOCKS"))) {
        const auto type=entry.front().value;
        if(type=="BLOCK"){currentBlock=value(entry,2).toUpper(); reader.blocks[currentBlock]={entry};}
        else if(type=="ENDBLK"){currentBlock.clear();}
        else if(!currentBlock.isEmpty()){reader.blocks[currentBlock].push_back(entry);}
    }
    auto entities = records(content.value(QStringLiteral("ENTITIES")));
    QStringList layouts;
    std::vector<std::pair<int,QString>> declaredLayouts;
    QHash<QString,QString> layoutsByOwner;
    QHash<QString,QString> layoutsByBlock;
    for (const auto& entry : records(content.value("OBJECTS"))) {
        if (entry.front().value != "LAYOUT") { continue; }
        const auto details = std::ranges::find(entry, QStringLiteral("AcDbLayout"), &Pair::value);
        if (details == entry.end()) { continue; }
        const Record info(details, entry.end());
        const auto name = value(info, 1);
        const auto owner = value(info, 330).toUpper();
        if (name.isEmpty() || owner.isEmpty() || name.compare("Model", Qt::CaseInsensitive) == 0) { continue; }
        layoutsByOwner.insert(owner, name);
        declaredLayouts.emplace_back(integer(info, 71), name);
    }
    for (const auto& entry : records(content.value("TABLES"))) {
        if (entry.front().value == "BLOCK_RECORD") {
            const auto layout = layoutsByOwner.value(value(entry, 5).toUpper());
            if (!layout.isEmpty()) { layoutsByBlock.insert(value(entry, 2).toUpper(), layout); }
        }
    }
    QHash<QString,QList<Record>> paperBlocks;
    for (const auto& entry : records(content.value("BLOCKS"))) {
        const auto type = entry.front().value;
        if (type == "BLOCK") { currentBlock = value(entry,2).toUpper(); }
        else if (type == "ENDBLK") { currentBlock.clear(); }
        else if (layoutsByBlock.contains(currentBlock)) {
            paperBlocks[layoutsByBlock.value(currentBlock)].push_back(entry);
        }
    }
    const auto keyOf = [&](Record record) {
        const auto key = value(record, 410).trimmed();
        if (!key.isEmpty()) { return key.compare("Model", Qt::CaseInsensitive) == 0 ? QString{} : key; }
        const auto owner = layoutsByOwner.value(value(record, 330).toUpper());
        if (!owner.isEmpty()) { return owner; }
        return integer(record,67) == 1 ? QStringLiteral("Layout1") : QString{};
    };
    QHash<QString,std::vector<Record>> byLayout;
    QString active;
    for (const auto& entry : entities) {
        const auto type = entry.front().value;
        if (type == "VERTEX" || type == "SEQEND") {
            byLayout[active].push_back(entry);
        } else if (type != "VIEWPORT") {
            active = keyOf(entry);
            if (!layouts.contains(active)) { layouts << active; }
            byLayout[active].push_back(entry);
        }
    }
    // Paper space for inactive tabs is stored in their block definitions.
    for (const auto& [index,name] : declaredLayouts) {
        if (!paperBlocks.contains(name)) { continue; }
        for (const auto& item : paperBlocks.value(name)) {
            if (item.front().value == "VIEWPORT") { continue; }
            if (!layouts.contains(name)) { layouts << name; }
            byLayout[name].push_back(item);
        }
    }
    if (std::ranges::any_of(layouts, [](const auto& name) { return !name.isEmpty(); })) {
        std::ranges::stable_sort(declaredLayouts, {}, &std::pair<int,QString>::first);
        QStringList ordered;
        for (const auto& [index,name] : declaredLayouts) {
            if (!ordered.contains(name)) { ordered << name; }
        }
        for (const auto& name : layouts) {
            if (!ordered.contains(name)) { ordered << name; }
        }
        layouts = std::move(ordered);
    }
    if (layouts.size() > maxSheets) { fileError(QStringLiteral("DXF contains too many layouts.")); }
    if (layouts.removeAll(QString{})) { layouts.prepend(QString{}); }
    DxfDrawing result;
    bool hasGeometry = false, first = true;
    for (const auto& layout : layouts) {
        Reader part = reader;
        const auto& records = byLayout[layout];
        for (std::size_t i = 0; i < records.size(); ++i) {
            const auto entry = records[i];
            if (entry.front().value != "POLYLINE") {
                part.entity(entry);
                continue;
            }
            std::vector<Vertex> vertices;
            bool planar = true;
            while (i + 1 < records.size() && records[i + 1].front().value == "VERTEX") {
                const auto vertex = records[++i];
                vertices.push_back({part.coordinate(vertex), number(vertex, 42)});
                planar = planar && number(vertex, 30) == 0;
                if (vertices.size() > 19999) { invalid(); }
            }
            if (i + 1 == records.size() || records[++i].front().value != "SEQEND") { invalid(); }
            part.entity(entry, planar ? &vertices : nullptr);
        }
        result.skipped += part.result.skipped;
        for (auto it = part.result.losses.cbegin(); it != part.result.losses.cend(); ++it) {
            result.losses[it.key()] += it.value();
        }
        if (!part.result.drawing.items.empty()) {
            fitImported(part.result);
            hasGeometry = true;
        } else if (layout.isEmpty()) {
            continue;
        }
        result.beyondSheet = result.beyondSheet || part.result.beyondSheet;
        part.result.drawing.title = layout.isEmpty() ? QFileInfo(path).completeBaseName().left(200) : decoded(layout).left(200);
        if (first) { result.drawing = std::move(part.result.drawing); first = false; }
        else { result.drawing.additionalSheets.push_back(std::move(part.result.drawing)); }
    }
    if (!hasGeometry) { fileError(QStringLiteral("DXF contains no supported 2D geometry.")); }
    return result;
}

QMap<QString,int> exportDxfReport(const QString& path, const Draft& sheet, bool allSheets)
{
    Writer writer;
    std::vector<const Draft*> sheets{&sheet};
    if(allSheets) for(const auto& item:sheet.additionalSheets) sheets.push_back(&item);
    if (sheets.size() > maxSheets) { fileError(QStringLiteral("DXF contains too many layouts.")); }
    QStringList layouts;
    static const QRegularExpression invalidLayout(QStringLiteral(R"([<>/\\":;?*|=,`])"));
    for (std::size_t i = 0; i < sheets.size(); ++i) {
        auto name = sheets[i]->title.trimmed().replace(invalidLayout, QStringLiteral("_")).left(100);
        if (name.isEmpty() || name.compare("Model", Qt::CaseInsensitive) == 0) {
            name = QStringLiteral("Sheet_%1").arg(i + 1);
        }
        const auto base = name;
        for (int suffix = 2; layouts.contains(encoded(name), Qt::CaseInsensitive); ++suffix) {
            name = base + QStringLiteral("_%1").arg(suffix);
        }
        layouts << encoded(name);
    }
    const bool paper=allSheets && sheets.size()>1;
    const auto hex=[](int id){return QString::number(id,16).toUpper();};
    const auto modelHandle=hex(80), collectionHandle=hex(81);
    const auto paperHandle=[&](int i){return hex(100+i);};
    const auto layoutHandle=[&](int i){return hex(160+i);};
    QMap<QString,int> losses;
    std::vector<QStringList> layerNames;
    QStringList names;
    QSet<QString> layerKeys;
    static const QRegularExpression unsafe(QStringLiteral(R"([<>/\\":;?*|=,`])"));
    for(const auto* page:sheets) {
        QStringList these;
        for(int i=0;i<static_cast<int>(page->layers.size());++i){
            auto name=page->layers[i].name.trimmed().replace(unsafe,QStringLiteral("_"));
            if(name.isEmpty()) name=i==0?"0":QStringLiteral("Layer_%1").arg(i+1);
            const auto base=name;
            for(int suffix=2;these.contains(encoded(name),Qt::CaseInsensitive);++suffix) name=base.left(190)+QStringLiteral("_%1").arg(suffix);
            name=encoded(name);
            these<<name;
            if(!layerKeys.contains(name.toCaseFolded())){layerKeys.insert(name.toCaseFolded());names<<name;}
        }
        layerNames.push_back(std::move(these));
    }
    std::vector<std::pair<QString,DraftItem>> blocks;
    QHash<const DraftItem*,QString> blockNames;
    for(const auto* page:sheets){
        const DraftGeometry resolver(*page);
        for(const auto& item:page->items){
            if(item.kind!=DraftKind::Fragment && !isDimension(item.kind))continue;
            const auto resolved=resolver.resolve(item);
            if(!validItem(item)||resolved.lostReference) continue;
            const bool dim=isDimension(item.kind);
            const auto block=QStringLiteral("%1%2").arg(dim?"*D":"GRAPH3D_BLOCK_").arg(blocks.size()+1);
            blockNames.insert(&item,block);
            blocks.emplace_back(block,resolved);
        }
    }
    writer.section("HEADER");
    writer.pair(9, "$ACADVER"); writer.pair(1, "AC1021");
    writer.pair(9, "$INSUNITS"); writer.pair(70, 4);
    writer.pair(9, "$MEASUREMENT"); writer.pair(70, 1);
    writer.pair(9,"$TILEMODE"); writer.pair(70,paper?0:1);
    writer.pair(9,"$DIMSTYLE"); writer.pair(2,"Standard");
    writer.pair(0, "ENDSEC");
    writer.section("TABLES");
    writer.table("LTYPE", 3);
    writer.lineTable("CONTINUOUS", {});
    writer.lineTable("HIDDEN", {3, -1.5});
    writer.lineTable("CENTER", {8, -2, 1.5, -2});
    writer.pair(0, "ENDTAB");

    const auto styleTable = hex(writer.nextHandle);
    writer.table("STYLE", 1);
    const auto textStyle = hex(writer.nextHandle);
    writer.pair(0, "STYLE"); writer.handle(); writer.pair(330, styleTable);
    writer.pair(100, "AcDbSymbolTableRecord"); writer.pair(100, "AcDbTextStyleTableRecord");
    writer.pair(2, "Standard"); writer.pair(70, 0); writer.pair(40, 0); writer.pair(41, 1);
    writer.pair(50, 0); writer.pair(71, 0); writer.pair(42, 4); writer.pair(3, "txt"); writer.pair(4, "");
    writer.pair(0, "ENDTAB");

    const auto dimStyleTable = hex(writer.nextHandle);
    writer.pair(0, "TABLE"); writer.pair(2, "DIMSTYLE"); writer.handle();
    writer.pair(100, "AcDbSymbolTable"); writer.pair(70, 1); writer.pair(100, "AcDbDimStyleTable"); writer.pair(71, 1);
    writer.pair(0, "DIMSTYLE"); writer.pair(105, hex(writer.nextHandle++)); writer.pair(330, dimStyleTable);
    writer.pair(100, "AcDbSymbolTableRecord"); writer.pair(100, "AcDbDimStyleTableRecord");
    writer.pair(2, "Standard"); writer.pair(70, 0); writer.pair(41, 2.5); writer.pair(140, 4);
    writer.pair(271, 2); writer.pair(340, textStyle);
    writer.pair(0, "ENDTAB");

    writer.table("LAYER", static_cast<int>(names.size()));
    for (qsizetype i = 0; i < names.size(); ++i) {
        writer.pair(0, "LAYER"); writer.handle(); writer.pair(100, "AcDbSymbolTableRecord"); writer.pair(100, "AcDbLayerTableRecord");
        DraftLayer info;
        for(std::size_t j=0;j<sheets.size();++j){
            const auto layer=layerNames[j].indexOf(names[i]); if(layer>=0){info=sheets[j]->layers[layer];break;}
        }
        writer.pair(2, names[i]); writer.pair(70, info.locked ? 4 : 0);
        writer.pair(62, info.visible ? 7 : -7); writer.pair(6, "CONTINUOUS");
    }
    writer.pair(0, "ENDTAB");
    writer.table("BLOCK_RECORD",static_cast<int>(blocks.size()+1+(paper?sheets.size():1)));
    const auto blockRecord=[&](QString name,QString id,QString layoutId={}) {
        writer.pair(0,"BLOCK_RECORD");writer.pair(5,id);writer.pair(100,"AcDbSymbolTableRecord");writer.pair(100,"AcDbBlockTableRecord");
        writer.pair(2,name); if(!layoutId.isEmpty())writer.pair(340,layoutId);writer.pair(70,0);writer.pair(280,1);writer.pair(281,0);
    };
    const auto spaceName=[](std::size_t i){return i==0?QStringLiteral("*Paper_Space"):QStringLiteral("*Paper_Space%1").arg(i-1);};
    blockRecord("*Model_Space",modelHandle,layoutHandle(0));
    for(std::size_t i=0;i<(paper?sheets.size():1);++i) blockRecord(spaceName(i),paperHandle(i),layoutHandle(i+1));
    std::vector<QString> blockOwners;
    for(const auto& [name,item]:blocks){
        blockOwners.push_back(hex(writer.nextHandle++));
        blockRecord(name,blockOwners.back());
    }
    writer.pair(0,"ENDTAB"); writer.pair(0,"ENDSEC");
    int written=0;
    const auto writeSheet = [&](std::size_t index) {
        const auto& page=*sheets[index];
        const auto& layer=layerNames[index];
        writer.layout=paper?layouts[index]:QString{};
        writer.owner=paper?paperHandle(index):modelHandle;
        const bool projected = page.modelViews && !layer.isEmpty() && writer.projectedViews(*page.modelViews, layer.front());
        if(projected) ++written;
        const DraftGeometry geometry(page);
        for (const auto& item : page.items) {
            if (!validItem(item) || item.layer < 0 || item.layer >= layer.size()) {
                invalid();
            }
            const auto exported = item.kind == DraftKind::Hatch || isDimension(item.kind) ? geometry.resolve(item) : item;
            if (exported.lostReference || !writer.write(exported, layer[item.layer], blockNames.value(&item))) ++losses[draftKindName(item.kind)];
            else {
                ++written;
                if(isDimension(item.kind) && (item.anchorA||item.anchorB||item.anchorC)) ++losses["DIMENSION association"];
            }
        }
        const auto& model=page.modelViews;
        if(model) {
            if(!projected) ++losses["Model view"];
            if(model->dimensions) ++losses["Automatic dimensions"];
            if(model->detail||model->local) ++losses["Detail or local section"];
            if(model->assembly && !model->assembly->callouts.empty()) losses["Assembly callouts"]+=static_cast<int>(model->assembly->callouts.size());
        }
    };
    writer.section("BLOCKS");
    const auto begin=[&](const QString& name,const QString& owner,int flags=0){
        writer.pair(0,"BLOCK"); writer.handle();writer.pair(330,owner);writer.pair(100,"AcDbEntity");writer.pair(8,"0");
        writer.pair(100,"AcDbBlockBegin");writer.pair(2,name);writer.pair(70,flags);writer.point({});writer.pair(30,0);writer.pair(3,name);writer.pair(1,"");
    };
    const auto end=[&](const QString& owner){
        writer.pair(0,"ENDBLK");writer.handle();writer.pair(330,owner);writer.pair(100,"AcDbEntity");writer.pair(8,"0");writer.pair(100,"AcDbBlockEnd");
    };
    begin("*Model_Space",modelHandle);end(modelHandle);
    for (std::size_t i = 0; i < (paper ? sheets.size() : 1); ++i) {
        begin(spaceName(i), paperHandle(i));
        if (i != 0) { writeSheet(i); }
        end(paperHandle(i));
    }
    writer.layout.clear();
    for(std::size_t i=0;i<blocks.size();++i){
        const auto& [name,item]=blocks[i];
        const bool dim=isDimension(item.kind);
        begin(name,blockOwners[i],dim?1:0);
        writer.owner=blockOwners[i];
        if(dim) {
            for(const auto& line:dimensionSegments(item)) writer.write(DraftItem{DraftKind::Line,line.p1(),line.p2()},"0");
            DraftItem text{DraftKind::Text,dimensionTextPoint(item)};text.text=draftDimensionText(item);writer.write(text,"0");
        } else {
            for(auto child:item.fragment){
                if(isDimension(child.kind)||!writer.write(child,"0")) ++losses[QStringLiteral("BLOCK/")+draftKindName(child.kind)];
            }
        }
        end(blockOwners[i]);
    }
    writer.pair(0,"ENDSEC");
    writer.section("ENTITIES");
    writeSheet(0);
    if (!written) {
        fileError(QStringLiteral("This sheet contains no supported geometry to export to DXF."));
    }
    writer.pair(0, "ENDSEC");
    writer.section("OBJECTS");
    writer.pair(0,"DICTIONARY");writer.pair(5,"C");writer.pair(330,"0");writer.pair(100,"AcDbDictionary");
    writer.pair(3,"ACAD_LAYOUT");writer.pair(350,collectionHandle);
    writer.pair(0,"DICTIONARY");writer.pair(5,collectionHandle);writer.pair(330,"C");writer.pair(100,"AcDbDictionary");
    writer.pair(3,"Model");writer.pair(350,layoutHandle(0));
    for(std::size_t i=0;i<(paper?sheets.size():1);++i){
        writer.pair(3,paper?layouts[i]:QStringLiteral("Layout1"));writer.pair(350,layoutHandle(i+1));
    }
    const auto space=[&](QString name,QString id,QString owner,int index,QSizeF size){
        writer.pair(0,"LAYOUT");writer.pair(5,id);writer.pair(330,collectionHandle);writer.pair(100,"AcDbPlotSettings");
        writer.pair(1,"");writer.pair(2,"");writer.pair(4,"");writer.pair(6,"");
        writer.pair(44,size.width());writer.pair(45,size.height());writer.pair(72,1);
        writer.pair(100,"AcDbLayout");writer.pair(1,name);writer.pair(70,1);writer.pair(71,index);
        // Drawing coordinates use the DXF upward Y axis; preserve the printable limits.
        writer.pair(10,0);writer.pair(20,-size.height());writer.pair(11,size.width());writer.pair(21,0);
        writer.pair(12,0);writer.pair(22,0);writer.pair(32,0);writer.pair(330,owner);
    };
    space("Model",layoutHandle(0),modelHandle,0,draftSize(sheet));
    for(std::size_t i=0;i<(paper?sheets.size():1);++i) space(paper?layouts[i]:QStringLiteral("Layout1"),layoutHandle(i+1),
        paperHandle(i),static_cast<int>(i)+1,paper?draftSize(*sheets[i]):draftSize(sheet));
    writer.pair(0,"ENDSEC");writer.pair(0, "EOF");
    if (writer.data.size() > fileLimit) {
        invalid();
    }
    writeFile(path, writer.data);
    return losses;
}

int exportDxf(const QString& path,const Draft& sheet)
{
    int total=0;
    for(const auto value:exportDxfReport(path,sheet)) total+=value;
    return total;
}

}  // namespace graph3d::io
