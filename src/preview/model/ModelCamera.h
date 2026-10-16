#ifndef MODELCAMERA_H
#define MODELCAMERA_H

#include <QMatrix4x4>
#include <QVector3D>

// Orbit camera around a target (turntable: yaw about +Y, pitch clamped short
// of the poles), perspective or orthographic with the same framing. Pure
// math, no GL; unit-tested.
//
// yaw 0 / pitch 0 looks from +Z towards -Z (the glTF "front").
class ModelCamera
{
public:
    enum class View : std::uint8_t { Front, Back, Left, Right, Top, Bottom, Iso };

    // Scene bounds the camera frames and derives clip planes from.
    void setBounds(const QVector3D &min, const QVector3D &max);
    // Target the bounds' centre at a distance that fits them in the view.
    void frame(float aspect);
    void setView(View view, float aspect);

    void orbit(float dxPixels, float dyPixels);
    void pan(float dxPixels, float dyPixels, int viewportHeight);
    // Wheel steps (one notch = 1); positive moves closer.
    void zoom(float steps);

    void setOrthographic(bool on) { m_ortho = on; }
    bool isOrthographic() const { return m_ortho; }

    QVector3D target() const { return m_target; }
    QVector3D eye() const;
    QVector3D forward() const; // unit, eye -> target
    float distance() const { return m_distance; }
    float yawDegrees() const { return m_yaw; }
    float pitchDegrees() const { return m_pitch; }
    float radius() const { return m_radius; }
    float fovYDegrees() const { return kFovY; }
    QVector3D boundsMin() const { return m_min; }
    QVector3D boundsMax() const { return m_max; }

    QMatrix4x4 viewMatrix() const;
    QMatrix4x4 projectionMatrix(float aspect) const;
    float nearPlane() const;
    float farPlane() const;
    // World units per pixel at the target (pan speed, grid line width).
    float unitsPerPixel(int viewportHeight) const;

    static constexpr float kFovY = 40.0f;

private:
    float fitDistance(float aspect) const;

    QVector3D m_min{-1, -1, -1};
    QVector3D m_max{1, 1, 1};
    QVector3D m_target;
    float m_radius = 1.0f;
    float m_distance = 3.0f;
    float m_yaw = 35.0f;
    float m_pitch = 22.0f;
    bool m_ortho = false;
};

#endif // MODELCAMERA_H
