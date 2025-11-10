// Camera.cpp - Pure view/projection matrix system
#include "Camera.h"
#include "../Culling/Frustum.h"

#define _USE_MATH_DEFINES
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
// Use a float-precision PI for camera trig to avoid double->float truncation warnings
constexpr float PI_F = 3.14159265358979323846f;

Camera::Camera()
    : m_frustum(new Frustum())
{
    updateCameraVectors();
}

Camera::~Camera()
{
    delete m_frustum;
}

void Camera::updateCameraVectors()
{
    // Calculate front vector from yaw and pitch using Vec3
    Vec3 newFront;
    newFront.x = std::cosf(yaw * (PI_F / 180.0f)) * std::cosf(pitch * (PI_F / 180.0f));
    newFront.y = std::sinf(pitch * (PI_F / 180.0f));
    newFront.z = std::sinf(yaw * (PI_F / 180.0f)) * std::cosf(pitch * (PI_F / 180.0f));
    front = newFront.normalized();

    // Calculate right and up vectors using Vec3 cross products
    Vec3 worldUp(0.0f, 1.0f, 0.0f);
    right = front.cross(worldUp).normalized();
    up = right.cross(front).normalized();
}

// Old array-based interface for compatibility
void Camera::getViewMatrix(float* matrix)
{
    glm::mat4 viewMat = getViewMatrix();
    const float* p = glm::value_ptr(viewMat);
    for (int i = 0; i < 16; ++i) matrix[i] = p[i];
}

// New clean Vec3-based view matrix
glm::mat4 Camera::getViewMatrix()
{
    Vec3 center = position + front;
    return glm::lookAt(
        glm::vec3(position.x, position.y, position.z),
        glm::vec3(center.x, center.y, center.z),
        glm::vec3(up.x, up.y, up.z));
}

// Old array-based interface for compatibility
// Old array-based interface for compatibility
void Camera::getProjectionMatrix(float* matrix, float aspect)
{
    glm::mat4 projMat = getProjectionMatrix(aspect);
    const float* p = glm::value_ptr(projMat);
    for (int i = 0; i < 16; ++i) matrix[i] = p[i];
}

// New clean Vec3-based projection matrix
glm::mat4 Camera::getProjectionMatrix(float aspect)
{
    float fov = 70.0f * PI_F / 180.0f;  // radians (tighter FOV - makes spaces feel more accurate)
    return glm::perspective(fov, aspect, 0.1f, 1000.0f);
}

void Camera::updateFrustum(float aspect)
{
    glm::mat4 viewProj = getProjectionMatrix(aspect) * getViewMatrix();
    m_frustum->extractFromMatrix(viewProj);
}

const Frustum& Camera::getFrustum() const
{
    return *m_frustum;
}
