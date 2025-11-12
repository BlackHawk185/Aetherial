// Frustum.h - View frustum for culling
#pragma once

#include <glm/glm.hpp>
#include "../Math/Vec3.h"

// Frustum defined by 6 planes (near, far, left, right, top, bottom)
// Each plane is stored as a 4D vector: ax + by + cz + d = 0
// Normalized so (a,b,c) is unit length
class Frustum
{
public:
    Frustum() = default;
    
    // Extract frustum planes from view-projection matrix
    void extractFromMatrix(const glm::mat4& viewProjection);
    
    // Test if AABB intersects frustum (for chunk culling)
    // Returns true if AABB is fully or partially inside frustum
    bool intersectsAABB(const Vec3& minBounds, const Vec3& maxBounds) const;
    
    // Test if AABB is fully inside frustum (for early-out optimization)
    bool fullyContainsAABB(const Vec3& minBounds, const Vec3& maxBounds) const;
    
    // Test if sphere intersects frustum (for island/cloud culling)
    // Returns true if sphere is fully or partially inside frustum
    bool intersectsSphere(const Vec3& center, float radius) const;
    
    // Test if point is inside frustum
    bool containsPoint(const Vec3& point) const;
    
private:
    // 6 frustum planes: [0]=left, [1]=right, [2]=bottom, [3]=top, [4]=near, [5]=far
    glm::vec4 m_planes[6];
    
    // Normalize plane equation
    static glm::vec4 normalizePlane(const glm::vec4& plane);
    
    // Get signed distance from point to plane
    static float distanceToPlane(const glm::vec4& plane, const Vec3& point);
};
