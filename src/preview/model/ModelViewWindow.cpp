#include "ModelViewWindow.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QPlatformSurfaceEvent>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr qint64 kMaxFrameStepNs = 100'000'000; // a stalled frame never jumps the animation further

QSurfaceFormat viewFormat()
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
#ifdef Q_OS_MACOS
    format.setVersion(4, 1);
#else
    format.setVersion(3, 3);
#endif
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setSamples(4);
    format.setSwapInterval(1);
    return format;
}

} // namespace

ModelViewWindow::ModelViewWindow()
{
    setSurfaceType(QSurface::OpenGLSurface);
    setFormat(viewFormat());
    m_clock.start();
}

ModelViewWindow::~ModelViewWindow()
{
    releaseGl(false);
}

void ModelViewWindow::releaseGl(bool surfaceGone)
{
    if (!m_context)
        return;
    // The surface still exists here (QWindow destroys it after this body, and
    // SurfaceAboutToBeDestroyed arrives before it goes); when it was never
    // created, an offscreen surface stands in so the objects can be deleted.
    QOffscreenSurface fallback;
    bool current = handle() && m_context->makeCurrent(this);
    if (!current) {
        fallback.setFormat(m_context->format());
        fallback.create();
        current = m_context->makeCurrent(&fallback);
    }
    if (current) {
        m_renderer.release();
        m_context->doneCurrent();
    }
    m_context.reset();
    if (surfaceGone && m_rendererHasDoc && m_doc) {
        m_rendererHasDoc = false;
        // Queued: we are inside the platform window's teardown.
        QTimer::singleShot(0, this, &ModelViewWindow::resourcesLost);
    }
    m_rendererHasDoc = false;
}

bool ModelViewWindow::ensureGl()
{
    if (m_glFailed)
        return false;
    if (m_context && m_context->isValid())
        return true;
    if (m_context) {
        // Context lost (GPU reset): its objects are gone with it.
        m_context.reset();
        if (m_rendererHasDoc && m_doc) {
            m_rendererHasDoc = false;
            QTimer::singleShot(0, this, &ModelViewWindow::resourcesLost);
        }
    }
    m_context = std::make_unique<QOpenGLContext>();
    m_context->setFormat(requestedFormat());
    if (!m_context->create() || !m_context->makeCurrent(this)) {
        m_context.reset();
        m_glFailed = true;
        emit glFailed(tr("3D preview could not create an OpenGL context."));
        return false;
    }
    QString error;
    if (!m_renderer.initialize(m_context.get(), &error)) {
        m_context->doneCurrent();
        m_context.reset();
        m_glFailed = true;
        emit glFailed(error);
        return false;
    }
    return true;
}

void ModelViewWindow::setDocument(const ModelDocumentPtr &doc, bool keepCamera)
{
    m_doc = doc;
    m_rendererHasDoc = false;
    if (!keepCamera)
        m_zUp = doc && doc->zUp;
    m_root = ModelScene::rootTransform(m_zUp);
    m_animation = -1;
    m_time = 0.0f;
    const bool wasPlaying = m_playing;
    m_playing = false;
    resetPose();
    updateBounds(!keepCamera);
    if (doc && !doc->animations.empty()) {
        m_animation = 0;
        m_playing = keepCamera ? wasPlaying : true;
        m_lastFrameNs = m_clock.nsecsElapsed();
        updatePose();
    }
    if (wasPlaying != m_playing)
        emit playingChanged(m_playing);
    emit timeChanged(m_time);
    m_lastPercent = doc ? 0 : 100;
    scheduleUpdate();
}

void ModelViewWindow::clearDocument()
{
    m_doc.reset();
    if (m_context && m_context->makeCurrent(this)) {
        m_renderer.clearDocument();
        m_context->doneCurrent();
    }
    m_rendererHasDoc = false;
    m_animation = -1;
    if (m_playing) {
        m_playing = false;
        emit playingChanged(false);
    }
    scheduleUpdate();
}

void ModelViewWindow::resetPose()
{
    m_joints.clear();
    m_skinBase.clear();
    if (!m_doc)
        return;
    m_pose.reset(*m_doc);
    m_pose.updateWorld(*m_doc, m_root);
    if (!m_doc->skins.empty())
        m_pose.jointMatrices(*m_doc, m_root, m_joints, m_skinBase);
    m_jointsChanged = true;
}

void ModelViewWindow::updatePose()
{
    if (!m_doc)
        return;
    if (m_animation >= 0 && size_t(m_animation) < m_doc->animations.size())
        m_pose.apply(m_doc->animations[size_t(m_animation)], m_time);
    m_pose.updateWorld(*m_doc, m_root);
    if (!m_doc->skins.empty())
        m_pose.jointMatrices(*m_doc, m_root, m_joints, m_skinBase);
    m_jointsChanged = true;
}

void ModelViewWindow::updateBounds(bool reframe)
{
    QVector3D min(-1, -1, -1);
    QVector3D max(1, 1, 1);
    if (m_doc) {
        // Rest pose bounds: an animation never re-frames the camera.
        const std::vector<QMatrix4x4> rest = ModelScene::restWorld(*m_doc, m_root);
        if (!ModelScene::sceneBounds(*m_doc, rest, m_root, min, max)) {
            min = QVector3D(-1, -1, -1);
            max = QVector3D(1, 1, 1);
        }
    }
    m_camera.setBounds(min, max);
    m_groundY = min.y();
    if (reframe) {
        m_camera.setView(ModelCamera::View::Iso, aspect());
        m_autoFrame = true;
    }
}

float ModelViewWindow::aspect() const
{
    return height() > 0 ? float(width()) / float(height()) : 1.0f;
}

void ModelViewWindow::setShading(ModelRenderer::Shading shading)
{
    m_settings.shading = shading;
    scheduleUpdate();
}

void ModelViewWindow::setWireframe(bool on)
{
    m_settings.wireframe = on;
    scheduleUpdate();
}

void ModelViewWindow::setGrid(bool on)
{
    m_settings.grid = on;
    scheduleUpdate();
}

void ModelViewWindow::setSkeleton(bool on)
{
    m_settings.skeleton = on;
    scheduleUpdate();
}

bool ModelViewWindow::hasSkeleton() const
{
    if (!m_doc)
        return false;
    for (const ModelSkin &skin : m_doc->skins) {
        if (!skin.joints.empty())
            return true;
    }
    return false;
}

void ModelViewWindow::setColors(const ModelRenderer::Colors &colors)
{
    m_colors = colors;
    scheduleUpdate();
}

void ModelViewWindow::setZUp(bool on)
{
    if (m_zUp == on)
        return;
    m_zUp = on;
    m_root = ModelScene::rootTransform(on);
    updatePose();
    updateBounds(true);
    scheduleUpdate();
}

void ModelViewWindow::setOrthographic(bool on)
{
    m_camera.setOrthographic(on);
    scheduleUpdate();
}

void ModelViewWindow::frameAll()
{
    m_camera.frame(aspect());
    m_autoFrame = true;
    scheduleUpdate();
}

void ModelViewWindow::setView(ModelCamera::View view)
{
    m_camera.setView(view, aspect());
    m_autoFrame = true;
    scheduleUpdate();
}

float ModelViewWindow::duration() const
{
    if (!m_doc || m_animation < 0 || size_t(m_animation) >= m_doc->animations.size())
        return 0.0f;
    return m_doc->animations[size_t(m_animation)].duration;
}

void ModelViewWindow::setAnimation(int index)
{
    if (!m_doc)
        return;
    m_animation = index >= 0 && size_t(index) < m_doc->animations.size() ? index : -1;
    m_time = 0.0f;
    // Nodes the new clip does not animate go back to their rest values.
    m_pose.reset(*m_doc);
    updatePose();
    if (m_animation < 0 && m_playing) {
        m_playing = false;
        emit playingChanged(false);
    }
    m_lastFrameNs = m_clock.nsecsElapsed();
    emit timeChanged(m_time);
    scheduleUpdate();
}

void ModelViewWindow::setPlaying(bool playing)
{
    playing = playing && m_animation >= 0 && duration() > 0.0f;
    if (m_playing == playing)
        return;
    m_playing = playing;
    m_lastFrameNs = m_clock.nsecsElapsed();
    emit playingChanged(playing);
    scheduleUpdate();
}

void ModelViewWindow::setTime(float seconds)
{
    m_time = std::clamp(seconds, 0.0f, duration());
    updatePose();
    emit timeChanged(m_time);
    scheduleUpdate();
}

bool ModelViewWindow::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::UpdateRequest:
        renderNow();
        return true;
    case QEvent::PlatformSurface:
        if (static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType()
            == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            releaseGl(true);
        break;
    default:
        break;
    }
    return QWindow::event(event);
}

void ModelViewWindow::exposeEvent(QExposeEvent *event)
{
    Q_UNUSED(event);
    if (isExposed()) {
        m_lastFrameNs = m_clock.nsecsElapsed(); // hidden time does not advance the clip
        renderNow();
    }
}

void ModelViewWindow::resizeEvent(QResizeEvent *event)
{
    QWindow::resizeEvent(event);
    if (m_autoFrame)
        m_camera.frame(aspect());
    scheduleUpdate();
}

void ModelViewWindow::renderNow()
{
    if (!isExposed() || !ensureGl())
        return;
    if (!m_context->makeCurrent(this))
        return;

    if (m_doc && !m_rendererHasDoc) {
        m_renderer.setDocument(m_doc);
        m_rendererHasDoc = true;
        m_jointsChanged = true;
        if (!m_renderer.error().isEmpty()) {
            const QString error = m_renderer.error();
            m_doc.reset();
            m_rendererHasDoc = false;
            emit glFailed(error);
        }
    }

    bool more = false;
    if (m_renderer.hasPendingUpload()) {
        more = m_renderer.upload(ModelRenderer::kUploadBudgetNs);
        const int percent = more ? std::min(99, int(m_renderer.uploadProgress() * 100.0f)) : 100;
        if (percent != m_lastPercent) {
            m_lastPercent = percent;
            emit uploadProgressChanged(percent);
        }
    } else if (m_lastPercent != 100) {
        m_lastPercent = 100;
        emit uploadProgressChanged(100);
    }

    if (m_playing) {
        const qint64 now = m_clock.nsecsElapsed();
        const qint64 step = std::min(now - m_lastFrameNs, kMaxFrameStepNs);
        m_lastFrameNs = now;
        const float length = duration();
        if (length > 0.0f) {
            m_time = std::fmod(m_time + float(double(step) * 1e-9), length);
            updatePose();
            emit timeChanged(m_time);
        }
    }

    const qreal dpr = devicePixelRatio();
    ModelRenderer::Frame frame;
    frame.view = m_camera.viewMatrix();
    frame.projection = m_camera.projectionMatrix(aspect());
    frame.eye = m_camera.eye();
    frame.target = m_camera.target();
    frame.pixelSize = QSize(qRound(width() * dpr), qRound(height() * dpr));
    frame.devicePixelRatio = float(dpr);
    frame.sceneRadius = m_camera.radius();
    frame.groundY = m_groundY;
    frame.world = &m_pose.world();
    frame.joints = m_joints.empty() ? nullptr : &m_joints;
    frame.skinBase = &m_skinBase;
    frame.jointsChanged = m_jointsChanged;
    m_renderer.render(frame, m_settings, m_colors);
    m_jointsChanged = false;
    m_context->swapBuffers(this);

    if (more || m_playing)
        requestUpdate();
}

void ModelViewWindow::mousePressEvent(QMouseEvent *event)
{
    const bool pan = event->button() == Qt::MiddleButton || event->button() == Qt::RightButton
                     || (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ShiftModifier));
    m_drag = pan ? Drag::Pan : event->button() == Qt::LeftButton ? Drag::Orbit : Drag::None;
    m_lastMouse = event->position();
    event->accept();
}

void ModelViewWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drag == Drag::None)
        return;
    const QPointF delta = event->position() - m_lastMouse;
    m_lastMouse = event->position();
    if (m_drag == Drag::Orbit) {
        m_camera.orbit(float(delta.x()), float(delta.y()));
    } else {
        m_camera.pan(float(delta.x()), float(delta.y()), height());
        m_autoFrame = false; // orbiting keeps the target, so the fit still holds
    }
    scheduleUpdate();
}

void ModelViewWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->buttons() == Qt::NoButton)
        m_drag = Drag::None;
}

void ModelViewWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        frameAll();
}

void ModelViewWindow::wheelEvent(QWheelEvent *event)
{
    const QPoint angle = event->angleDelta();
    if (angle.y() != 0) {
        m_camera.zoom(float(angle.y()) / 120.0f);
        m_autoFrame = false;
        scheduleUpdate();
    }
    event->accept();
}

void ModelViewWindow::keyPressEvent(QKeyEvent *event)
{
    const bool opposite = event->modifiers() & Qt::ControlModifier;
    switch (event->key()) {
    case Qt::Key_F:
    case Qt::Key_Home:
        frameAll();
        break;
    case Qt::Key_1:
        setView(opposite ? ModelCamera::View::Back : ModelCamera::View::Front);
        break;
    case Qt::Key_3:
        setView(opposite ? ModelCamera::View::Left : ModelCamera::View::Right);
        break;
    case Qt::Key_7:
        setView(opposite ? ModelCamera::View::Bottom : ModelCamera::View::Top);
        break;
    case Qt::Key_0:
        setView(ModelCamera::View::Iso);
        break;
    case Qt::Key_5:
        setOrthographic(!isOrthographic());
        emit settingsChangedByKey();
        break;
    case Qt::Key_W:
        setWireframe(!m_settings.wireframe);
        emit settingsChangedByKey();
        break;
    case Qt::Key_G:
        setGrid(!m_settings.grid);
        emit settingsChangedByKey();
        break;
    case Qt::Key_Space:
        setPlaying(!m_playing);
        break;
    default:
        QWindow::keyPressEvent(event);
        return;
    }
    event->accept();
}
