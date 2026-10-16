#ifndef MODELVIEWWINDOW_H
#define MODELVIEWWINDOW_H

#include "ModelAnimator.h"
#include "ModelCamera.h"
#include "ModelDocument.h"
#include "ModelRenderer.h"

#include <QElapsedTimer>
#include <QPoint>
#include <QWindow>

#include <memory>
#include <vector>

class QOpenGLContext;

// Native OpenGL window the model tab embeds with QWidget::createWindowContainer
// (no QOpenGLWidget: that one renders through an FBO and a texture blit into
// the widget backing store every frame).
//
// Renders on demand: one frame per requestUpdate(), which camera input,
// setting changes, a pending upload or a playing animation ask for. Idle,
// the window costs nothing. The GL objects are released while the context is
// still current in the destructor, or when the platform surface goes away;
// since the renderer freed the CPU arrays after upload, losing them asks the
// owner to reload the file (resourcesLost()).
class ModelViewWindow : public QWindow
{
    Q_OBJECT

public:
    ModelViewWindow();
    ~ModelViewWindow() override;

    // keepCamera: a reload of the same file keeps the view.
    void setDocument(const ModelDocumentPtr &doc, bool keepCamera);
    void clearDocument();
    ModelDocumentPtr document() const { return m_doc; }

    const ModelRenderer::Settings &settings() const { return m_settings; }
    void setShading(ModelRenderer::Shading shading);
    void setWireframe(bool on);
    void setGrid(bool on);
    void setSkeleton(bool on);
    // No usable OpenGL (context or renderer init failed); a failed upload of one
    // document does not count.
    bool glUnavailable() const { return m_glFailed; }
    bool hasSkeleton() const;
    void setColors(const ModelRenderer::Colors &colors);

    void setZUp(bool on);
    bool zUp() const { return m_zUp; }
    void setOrthographic(bool on);
    bool isOrthographic() const { return m_camera.isOrthographic(); }
    void frameAll();
    void setView(ModelCamera::View view);

    // -1: rest pose.
    void setAnimation(int index);
    int animation() const { return m_animation; }
    void setPlaying(bool playing);
    bool isPlaying() const { return m_playing; }
    void setTime(float seconds);
    float time() const { return m_time; }
    float duration() const;

    // 0..100 while uploading to the GPU, 100 when done.
    int uploadPercent() const { return m_lastPercent; }

signals:
    void glFailed(const QString &message);
    void uploadProgressChanged(int percent);
    void timeChanged(float seconds);
    void playingChanged(bool playing);
    // A setting changed from the keyboard (W, G, 5, Space): resync the toolbar.
    void settingsChangedByKey();
    void resourcesLost();

protected:
    bool event(QEvent *event) override;
    void exposeEvent(QExposeEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    enum class Drag : quint8 { None, Orbit, Pan };

    bool ensureGl();
    void releaseGl(bool surfaceGone);
    void renderNow();
    void scheduleUpdate() { requestUpdate(); }
    void resetPose();
    void updatePose();
    void updateBounds(bool reframe);
    float aspect() const;

    std::unique_ptr<QOpenGLContext> m_context;
    ModelRenderer m_renderer;
    bool m_glFailed = false;
    bool m_rendererHasDoc = false;  // the renderer got m_doc (its arrays may be freed)

    ModelDocumentPtr m_doc;
    ModelCamera m_camera;
    ModelPose m_pose;
    QMatrix4x4 m_root;
    bool m_zUp = false;
    float m_groundY = 0.0f;
    std::vector<float> m_joints;
    std::vector<int> m_skinBase;
    bool m_jointsChanged = false;

    int m_animation = -1;
    float m_time = 0.0f;
    bool m_playing = false;
    QElapsedTimer m_clock;
    qint64 m_lastFrameNs = 0;

    ModelRenderer::Settings m_settings;
    ModelRenderer::Colors m_colors;
    // Framed by us and not touched since: a resize re-fits it (the first
    // frame happens before the window has its real size).
    bool m_autoFrame = false;
    Drag m_drag = Drag::None;
    QPointF m_lastMouse;
    int m_lastPercent = 100;
};

#endif // MODELVIEWWINDOW_H
