#pragma once

#include "drawing/Draft.h"

#include <QWidget>

#include <optional>

namespace graph3d {

class LineInput;

class DraftCanvas final : public QWidget {
    Q_OBJECT

public:
    explicit DraftCanvas(const drawing::Draft& draft, LineInput& lineInput, QWidget* parent = nullptr);
    void setTool(std::optional<drawing::DraftKind> tool);
    void setStroke(drawing::DraftStroke stroke);
    void setGrid(bool enabled);
    void fit();
    void cancel();
    void select(int index);
    int selected() const { return selected_; }

signals:
    void itemCreated(const drawing::DraftItem& item);
    void itemMoved(int index, const drawing::DraftItem& item);
    void selectionChanged(int index);
    void positionChanged(QPointF point);
    void lineOutsideSheet();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
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

    const drawing::Draft& draft_;
    LineInput& lineInput_;
    std::optional<drawing::DraftKind> tool_;
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
    std::optional<drawing::DraftItem> preview_;
};

}  // namespace graph3d
