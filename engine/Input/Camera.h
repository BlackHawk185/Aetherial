// Camera.h - Pure view/projection matrix system
// Camera is now a simple data structure for view calculations
// Input and physics are handled by PlayerController
#pragma once
#include "Math/Vec3.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

class Frustum;  // Forward declaration

class Camera {
public:
    Camera();
    ~Camera();
    
    // View and projection matrix generation
    void getViewMatrix(float* matrix);
    glm::mat4 getViewMatrix();
    void getProjectionMatrix(float* matrix, float aspect);
    glm::mat4 getProjectionMatrix(float aspect);
    glm::mat4 getWideFOVProjectionMatrix(float aspect);  // Wide FOV for SSR data
    
    // Frustum calculation for culling
    void updateFrustum(float aspect);
    const Frustum& getFrustum() const;
    
    // Position and orientation - Controlled by PlayerController
    Vec3 position;
    Vec3 front{0.0f, 0.0f, -1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 right{1.0f, 0.0f, 0.0f};
    
    // Camera settings
    float yaw = 0.0f;     // Horizontal rotation
    float pitch = -30.0f; // Vertical rotation
    float sensitivity = 0.1f; // Mouse sensitivity
    
    // Update camera vectors when yaw/pitch are modified
    void updateCameraVectors();

private:
    Frustum* m_frustum;  // Cached frustum for culling
};
