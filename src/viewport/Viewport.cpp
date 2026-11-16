#include "viewport/Viewport.h"

#include "document/Document.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Image_PixMap.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Quantity_Color.hxx>
#include <Standard_Failure.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include <QGuiApplication>
#include <QMouseEvent>
#include <QWheelEvent>

#if defined(Q_OS_WIN)
#include <WNT_Window.hxx>
#elif defined(Q_OS_LINUX)
#include <Xw_Window.hxx>
#endif

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace graph3d {
namespace {

Handle(Aspect_Window) nativeWindow(WId id, const Handle(Aspect_DisplayConnection)& display)
{
#if defined(Q_OS_WIN)
    Q_UNUSED(display);
    return new WNT_Window((Aspect_Handle)id);
#elif defined(Q_OS_LINUX)
    if (QGuiApplication::platformName() != QStringLiteral("xcb")) {
        throw std::runtime_error("This build of the 3D viewport requires an X11 (xcb) session.");
    }
    return new Xw_Window(display, static_cast<Aspect_Drawable>(id));
#else
    Q_UNUSED(id);
    Q_UNUSED(display);
    throw std::runtime_error("A native 3D viewport is unavailable on this platform.");
#endif
}

}  // namespace

struct Viewport::Scene {
    Handle(OpenGl_GraphicDriver) driver;
    Handle(V3d_Viewer) viewer;
    Handle(AIS_InteractiveContext) context;
    Handle(V3d_View) view;
    std::vector<Handle(AIS_Shape)> objects;
    QPoint previousMouse;
    bool ready = false;

    ~Scene()
    {
        if (!view.IsNull()) {
            view->Remove();
        }
    }
};

Viewport::Viewport(QWidget* parent)
    : QWidget(parent), scene_(std::make_unique<Scene>())
{
    setObjectName(QStringLiteral("viewport"));
    setAccessibleName(tr("3D viewport"));
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

Viewport::~Viewport() = default;

bool Viewport::initialize()
{
    if (scene_->ready) {
        return true;
    }
    return runOperation(tr("Initialize 3D viewport"), [this] {
        auto scene = std::make_unique<Scene>();
        const Handle(Aspect_DisplayConnection) display = new Aspect_DisplayConnection;
        const auto window = nativeWindow(winId(), display);

        scene->driver = new OpenGl_GraphicDriver(display);
        scene->viewer = new V3d_Viewer(scene->driver);
        scene->viewer->SetDefaultLights();
        scene->viewer->SetLightOn();
        scene->viewer->SetRectangularGridValues(0, 0, 20, 20, 0);
        scene->viewer->SetRectangularGridGraphicValues(600, 600, 0);
        scene->viewer->ActivateGrid(Aspect_GT_Rectangular, Aspect_GDM_Lines);
        scene->context = new AIS_InteractiveContext(scene->viewer);
        scene->context->SetDisplayMode(AIS_Shaded, false);
        scene->view = scene->viewer->CreateView();
        scene->view->SetWindow(window);
        scene->view->SetBackgroundColor(Quantity_Color(0.10, 0.13, 0.17, Quantity_TOC_sRGB));
        scene->view->SetProj(V3d_TypeOfOrientation_Zup_AxoRight);
        scene->view->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, Quantity_NOC_WHITE, 0.09, V3d_ZBUFFER);
        scene->view->MustBeResized();
        scene->view->Redraw();
        scene->ready = true;
        scene_ = std::move(scene);
    });
}

bool Viewport::runOperation(const QString& operation, const std::function<void()>& action)
{
    QString reason;
    try {
        action();
        return true;
    } catch (const Standard_Failure& error) {
        reason = QString::fromUtf8(error.GetMessageString());
    } catch (const std::exception& error) {
        reason = QString::fromUtf8(error.what());
    }
    if (reason.isEmpty()) {
        reason = tr("Unknown error");
    }
    emit operationFailed(tr("%1: %2").arg(operation, reason));
    return false;
}

void Viewport::syncDocument(const document::Document& document)
{
    runOperation(tr("Refresh scene"), [this, &document] {
        const auto& records = document.objects();
        while (scene_->objects.size() > records.size()) {
            const auto& object = scene_->objects.back();
            if (!object.IsNull()) {
                scene_->context->Remove(object, false);
            }
            scene_->objects.pop_back();
        }
        scene_->objects.resize(records.size());
        for (std::size_t i = 0; i < records.size(); ++i) {
            const auto& record = records[i];
            auto& object = scene_->objects[i];
            if (record.shape.IsNull()) {
                if (!object.IsNull()) {
                    scene_->context->Remove(object, false);
                    object.Nullify();
                }
                continue;
            }
            const bool sketch = std::holds_alternative<geometry::Sketch>(record.parameters);
            const auto color = sketch ? Quantity_Color(0.35, 0.83, 1.00, Quantity_TOC_sRGB)
                                      : Quantity_Color(0.39, 0.67, 0.82, Quantity_TOC_sRGB);
            if (object.IsNull()) {
                object = new AIS_Shape(record.shape);
                object->SetColor(color);
                object->SetWidth(sketch ? 2.0 : 1.0);
            } else if (!object->Shape().IsSame(record.shape)) {
                object->SetShape(record.shape);
                object->SetColor(color);
                object->SetWidth(sketch ? 2.0 : 1.0);
                scene_->context->Redisplay(object, false);
            }
            const bool displayed = scene_->context->IsDisplayed(object);
            if (record.visible && !displayed) {
                scene_->context->Display(object, sketch ? AIS_WireFrame : AIS_Shaded, 0, false);
            } else if (!record.visible && displayed) {
                scene_->context->Erase(object, false);
            }
        }
        scene_->view->ZFitAll();
        scene_->context->UpdateCurrentViewer();
    });
}

QImage Viewport::renderSnapshot()
{
    QImage output;
    if (!scene_->ready || width() < 1 || height() < 1) {
        return output;
    }
    runOperation(tr("Render 3D view"), [this, &output] {
        Image_PixMap pixels;
        if (!pixels.InitTrash(Image_Format_RGB, width(), height(), width() * 3)
            || !scene_->view->ToPixMap(pixels, width(), height())) {
            throw std::runtime_error("Could not render the offscreen view.");
        }
        const auto format = pixels.Format() == Image_Format_RGB ? QImage::Format_RGB888
                          : pixels.Format() == Image_Format_BGR ? QImage::Format_BGR888
                          : QImage::Format_Invalid;
        if (format == QImage::Format_Invalid) {
            throw std::runtime_error("Unsupported pixel layout for the offscreen view.");
        }
        output = QImage(width(), height(), format);
        if (output.isNull()) {
            throw std::runtime_error("Could not allocate the offscreen image.");
        }
        output.fill(Qt::black);
        for (int row = 0; row < output.height(); ++row) {
            std::memcpy(output.scanLine(row), pixels.Row(row), static_cast<std::size_t>(output.width()) * 3);
        }
    });
    return output;
}

void Viewport::resetCamera()
{
    runOperation(tr("Reset camera"), [this] {
        scene_->view->Reset(false);
        scene_->view->SetProj(V3d_TypeOfOrientation_Zup_AxoRight);
        scene_->view->Redraw();
    });
}

void Viewport::selectObject(int id)
{
    runOperation(tr("Select object"), [this, id] {
        scene_->context->ClearSelected(false);
        if (id > 0 && static_cast<std::size_t>(id) <= scene_->objects.size()) {
            const auto& object = scene_->objects[id - 1];
            if (!object.IsNull() && scene_->context->IsDisplayed(object)) {
                scene_->context->SetSelected(object, false);
            }
        }
        scene_->context->UpdateCurrentViewer();
    });
}

void Viewport::clearSelection()
{
    selectObject(0);
    emit objectSelected(0);
}

void Viewport::fitScene()
{
    runOperation(tr("Fit scene"), [this] {
        scene_->view->FitAll(0.12, false);
        scene_->view->ZFitAll();
        scene_->view->Redraw();
    });
}

void Viewport::setDirection(ViewDirection direction)
{
    runOperation(tr("Change view"), [this, direction] {
        switch (direction) {
        case ViewDirection::Front: scene_->view->SetProj(V3d_TypeOfOrientation_Zup_Front); break;
        case ViewDirection::Top: scene_->view->SetProj(V3d_TypeOfOrientation_Zup_Top); break;
        case ViewDirection::Right: scene_->view->SetProj(V3d_TypeOfOrientation_Zup_Right); break;
        case ViewDirection::Isometric: scene_->view->SetProj(V3d_TypeOfOrientation_Zup_AxoRight); break;
        }
        scene_->view->ZFitAll();
        scene_->view->Redraw();
    });
}

void Viewport::setGridVisible(bool visible)
{
    runOperation(tr("Change grid visibility"), [this, visible] {
        if (visible) {
            scene_->viewer->ActivateGrid(Aspect_GT_Rectangular, Aspect_GDM_Lines);
        } else {
            scene_->viewer->DeactivateGrid();
        }
        scene_->view->Redraw();
    });
}

void Viewport::setAxesVisible(bool visible)
{
    runOperation(tr("Change axes visibility"), [this, visible] {
        if (visible) {
            scene_->view->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, Quantity_NOC_WHITE, 0.09, V3d_ZBUFFER);
        } else {
            scene_->view->TriedronErase();
        }
        scene_->view->Redraw();
    });
}

QPaintEngine* Viewport::paintEngine() const
{
    return nullptr;
}

void Viewport::paintEvent(QPaintEvent*)
{
    if (scene_->ready && !runOperation(tr("Draw 3D view"), [this] { scene_->view->Redraw(); })) {
        setUpdatesEnabled(false);
    }
}

void Viewport::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (scene_->ready) {
        runOperation(tr("Resize 3D view"), [this] { scene_->view->MustBeResized(); });
    }
}

QPoint Viewport::pixels(const QPointF& logicalPosition) const
{
    return (logicalPosition * devicePixelRatioF()).toPoint();
}

void Viewport::mousePressEvent(QMouseEvent* event)
{
    if (!scene_->ready) {
        return;
    }
    const auto point = pixels(event->position());
    scene_->previousMouse = point;
    runOperation(tr("Handle pointer"), [this, event, point] {
        if (event->button() == Qt::RightButton) {
            scene_->view->StartRotation(point.x(), point.y());
        } else if (event->button() == Qt::LeftButton) {
            scene_->context->MoveTo(point.x(), point.y(), scene_->view, false);
            scene_->context->SelectDetected(AIS_SelectionScheme_Replace);
            scene_->context->UpdateCurrentViewer();
            const auto selected = scene_->context->FirstSelectedObject();
            const auto match = std::find(scene_->objects.begin(), scene_->objects.end(), selected);
            const auto id = selected.IsNull() || match == scene_->objects.end() ? 0
                          : static_cast<int>(match - scene_->objects.begin()) + 1;
            emit objectSelected(id);
        }
    });
}

void Viewport::mouseMoveEvent(QMouseEvent* event)
{
    if (!scene_->ready) {
        return;
    }
    const auto point = pixels(event->position());
    const auto delta = point - scene_->previousMouse;
    scene_->previousMouse = point;
    runOperation(tr("Move pointer"), [this, event, point, delta] {
        if (event->buttons().testFlag(Qt::RightButton)) {
            scene_->view->Rotation(point.x(), point.y());
        } else if (event->buttons().testFlag(Qt::MiddleButton)) {
            scene_->view->Pan(delta.x(), -delta.y());
        } else if (event->buttons() == Qt::NoButton) {
            scene_->context->MoveTo(point.x(), point.y(), scene_->view, true);
        }
    });
}

void Viewport::wheelEvent(QWheelEvent* event)
{
    if (!scene_->ready) {
        return;
    }
    const auto point = pixels(event->position());
    const int zoom = std::clamp(event->angleDelta().y(), -600, 600) / 3;
    if (zoom != 0) {
        runOperation(tr("Zoom"), [this, point, zoom] {
            scene_->view->StartZoomAtPoint(point.x(), point.y());
            scene_->view->ZoomAtPoint(0, 0, zoom, 0);
        });
    }
    event->accept();
}

}  // namespace graph3d
