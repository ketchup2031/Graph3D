#pragma once

#include "drawing/Draft.h"

#include <QWidget>

#include <functional>
#include <optional>

namespace graph3d {

class LineInput;
enum class DraftModify { Trim, Extend, Offset };

class DraftCanvas final : public QWidget {
    Q_OBJECT

public:
    explicit DraftCanvas(const drawing::Draft& draft, LineInput& lineInput, QWidget* parent = nullptr);
    void setTool(std::optional<drawing::DraftKind> tool);
    void setModify(DraftModify mode);
    void setStroke(drawing::DraftStroke stroke);
    void setGrid(bool enabled);
    void setPageNumber(int page, int total) { page_ = page; total_ = total; update(); }
    void fit();
    void pickPoint(std::function<void(QPointF)> callback);
    void cancel();
    void select(int index);
    int selected() const { return selected_; }
    std::optional<drawing::ModelPick> modelPick(QPointF point, bool circularOnly = false) const;

signals:
    void itemCreated(const drawing::DraftItem& item);
    void itemMoved(int index, const drawing::DraftItem& item);
    void selectionChanged(int index);
    void positionChanged(QPointF point);
    void lineOutsideSheet();
    void changeRequested(int index, const std::vector<drawing::DraftItem>& items);
    void toolMessage(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    double scale() const;
    QPointF origin() const;
    QPointF toSheet(QPointF screen) const;
    struct Pick {
        QPointF position;
        std::optional<drawing::DraftReference> reference;
    };
    Pick snapped(QPointF page) const;
    std::optional<drawing::DraftItem> pendingItem(QPointF end) const;

    bool canPlace(QPointF point) const;
    void addLine(QPointF end);
    void finishPolyline();
    void handleModify(QPointF point);
    std::optional<Pick> circularPick(QPointF point) const;
    int hitItem(QPointF point, bool curvesOnly = false) const;

    std::function<void(QPointF)> pointPick_;
    const drawing::Draft& draft_;
    LineInput& lineInput_;
    std::optional<drawing::DraftKind> tool_;
    std::optional<DraftModify> modify_;
    drawing::DraftStroke stroke_ = drawing::DraftStroke::Solid;
    std::vector<Pick> points_;
    QPointF cursor_;
    QPointF pan_;
    QPointF panStart_;
    QPointF dragStart_;
    double zoom_ = 1;
    bool grid_ = true;
    bool cursorAttached_ = false;
    bool panning_ = false;
    bool dragging_ = false;
    int selected_ = -1;
    int page_ = 1;
    int total_ = 1;
    std::optional<drawing::DraftItem> preview_;
};

}  // namespace graph3d
