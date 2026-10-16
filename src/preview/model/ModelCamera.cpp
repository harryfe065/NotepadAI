#include "ModelCamera.h"

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {

constexpr float kOrbitDegreesPerPixel = 0.35f;
constexpr float kMaxPitch = 89.9f;
constexpr float kZoomPerStep = 0.85f;
constexpr float kMargin = 1.08f; // breathing room around the framed bounds

} // namespace

void ModelCamera::setBounds(const QVector3D &min, const QVector3D &max)
{
    m_min = min;
    m_max = max;
    const float r = (max - min).length() * 0.5f;
    m_radius = r > 1e-6f && std::isfinite(r) ? r : 1.0f;
}

float ModelCamera::fitDistance(float aspect) const
{
    // The bounding sphere must fit both the vertical and the horizontal fov.
    const float halfV = qDegreesToRadians(kFovY) * 0.5f;
    const float halfH = std::atan(std::tan(halfV) * std::max(aspect, 1e-3f));
    return m_radius * kMargin / std::sin(std::min(halfV, halfH));
}

void ModelCamera::frame(float aspect)
{
    m_target = (m_min + m_max) * 0.5f;
    m_distance = fitDistance(aspect);
}

void ModelCamera::setView(View view, float aspect)
{
    switch (view) {
    case View::Front: m_yaw = 0.0f; m_pitch = 0.0f; break;
    case View::Back: m_yaw = 180.0f; m_pitch = 0.0f; break;
    case View::Right: m_yaw = 90.0f; m_pitch = 0.0f; break;
    case View::Left: m_yaw = -90.0f; m_pitch = 0.0f; break;
    case View::Top: m_yaw = 0.0f; m_pitch = kMaxPitch; break;
    case View::Bottom: m_yaw = 0.0f; m_pitch = -kMaxPitch; break;
    case View::Iso: m_yaw = 35.0f; m_pitch = 22.0f; break;
    }
    frame(aspect);
}

void ModelCamera::orbit(float dxPixels, float dyPixels)
{
    m_yaw = std::fmod(m_yaw - dxPixels * kOrbitDegreesPerPixel, 360.0f);
    m_pitch = std::clamp(m_pitch + dyPixels * kOrbitDegreesPerPixel, -kMaxPitch, kMaxPitch);
}

QVector3D ModelCamera::forward() const
{
    const float yaw = qDegreesToRadians(m_yaw);
    const float pitch = qDegreesToRadians(m_pitch);
    // From the target to the eye, negated.
    const QVector3D toEye(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
    return -toEye;
}

QVector3D ModelCamera::eye() const
{
    return m_target - forward() * m_distance;
}

float ModelCamera::unitsPerPixel(int viewportHeight) const
{
    const float h = std::max(1, viewportHeight);
    return 2.0f * m_distance * std::tan(qDegreesToRadians(kFovY) * 0.5f) / h;
}

void ModelCamera::pan(float dxPixels, float dyPixels, int viewportHeight)
{
    const QVector3D f = forward();
    QVector3D right = QVector3D::crossProduct(f, QVector3D(0, 1, 0));
    if (right.lengthSquared() < 1e-8f)
        right = QVector3D(1, 0, 0);
    right.normalize();
    const QVector3D up = QVector3D::crossProduct(right, f).normalized();
    const float s = unitsPerPixel(viewportHeight);
    m_target += (-right * dxPixels + up * dyPixels) * s;
}

void ModelCamera::zoom(float steps)
{
    m_distance *= std::pow(kZoomPerStep, steps);
    m_distance = std::clamp(m_distance, m_radius * 1e-3f, m_radius * 1e3f);
}

QMatrix4x4 ModelCamera::viewMatrix() const
{
    QMatrix4x4 m;
    m.lookAt(eye(), m_target, QVector3D(0, 1, 0));
    return m;
}

float ModelCamera::nearPlane() const
{
    if (m_ortho)
        return -m_radius * 20.0f; // nothing in front of the target is clipped
    return std::max(m_distance * 0.01f, m_radius * 1e-5f);
}

float ModelCamera::farPlane() const
{
    return m_distance + m_radius * 20.0f;
}

QMatrix4x4 ModelCamera::projectionMatrix(float aspect) const
{
    QMatrix4x4 m;
    if (m_ortho) {
        // Same apparent size at the target as the perspective view.
        const float h = m_distance * std::tan(qDegreesToRadians(kFovY) * 0.5f);
        m.ortho(-h * aspect, h * aspect, -h, h, nearPlane(), farPlane());
    } else {
        m.perspective(kFovY, aspect, nearPlane(), farPlane());
    }
    return m;
}
