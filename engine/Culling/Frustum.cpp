// Frustum.cpp - View frustum implementation
#include "Frustum.h"
#include <algorithm>

void Frustum::extractFromMatrix(const glm::mat4& vp)
{
    // Extract frustum planes using Gribb-Hartmann method
    // Left plane: row4 + row1
    m_planes[0] = glm::vec4(
        vp[0][3] + vp[0][0],
        vp[1][3] + vp[1][0],
        vp[2][3] + vp[2][0],
        vp[3][3] + vp[3][0]
    );
    
    // Right plane: row4 - row1
    m_planes[1] = glm::vec4(
        vp[0][3] - vp[0][0],
        vp[1][3] - vp[1][0],
        vp[2][3] - vp[2][0],
        vp[3][3] - vp[3][0]
    );
    
    // Bottom plane: row4 + row2
    m_planes[2] = glm::vec4(
        vp[0][3] + vp[0][1],
        vp[1][3] + vp[1][1],
        vp[2][3] + vp[2][1],
        vp[3][3] + vp[3][1]
    );
    
    // Top plane: row4 - row2
    m_planes[3] = glm::vec4(
        vp[0][3] - vp[0][1],
        vp[1][3] - vp[1][1],
        vp[2][3] - vp[2][1],
        vp[3][3] - vp[3][1]
    );
    
    // Near plane: row4 + row3
    m_planes[4] = glm::vec4(
        vp[0][3] + vp[0][2],
        vp[1][3] + vp[1][2],
        vp[2][3] + vp[2][2],
        vp[3][3] + vp[3][2]
    );
    
    // Far plane: row4 - row3
    m_planes[5] = glm::vec4(
        vp[0][3] - vp[0][2],
        vp[1][3] - vp[1][2],
        vp[2][3] - vp[2][2],
        vp[3][3] - vp[3][2]
    );
    
    // Normalize all planes
    for (int i = 0; i < 6; ++i)
    {
        m_planes[i] = normalizePlane(m_planes[i]);
    }
}

bool Frustum::intersectsAABB(const Vec3& minBounds, const Vec3& maxBounds) const
{
    // Test AABB against all 6 planes
    // If AABB is completely outside any plane, it's culled
    for (int i = 0; i < 6; ++i)
    {
        const glm::vec4& plane = m_planes[i];
        
        // Find the positive vertex (furthest point in direction of plane normal)
        Vec3 positiveVertex(
            plane.x > 0 ? maxBounds.x : minBounds.x,
            plane.y > 0 ? maxBounds.y : minBounds.y,
            plane.z > 0 ? maxBounds.z : minBounds.z
        );
        
        // If positive vertex is outside plane, AABB is completely outside
        if (distanceToPlane(plane, positiveVertex) < 0)
        {
            return false;  // AABB is outside this plane
        }
    }
    
    return true;  // AABB intersects or is inside frustum
}

bool Frustum::intersectsSphere(const Vec3& center, float radius) const
{
    // Test sphere against all 6 planes
    for (int i = 0; i < 6; ++i)
    {
        float distance = distanceToPlane(m_planes[i], center);
        
        // If center is more than radius away from plane (on negative side), sphere is outside
        if (distance < -radius)
        {
            return false;
        }
    }
    
    return true;  // Sphere intersects or is inside frustum
}

bool Frustum::containsPoint(const Vec3& point) const
{
    // Test point against all 6 planes
    for (int i = 0; i < 6; ++i)
    {
        if (distanceToPlane(m_planes[i], point) < 0)
        {
            return false;
        }
    }
    
    return true;
}

glm::vec4 Frustum::normalizePlane(const glm::vec4& plane)
{
    float length = glm::length(glm::vec3(plane.x, plane.y, plane.z));
    if (length > 0.0f)
    {
        return plane / length;
    }
    return plane;
}

float Frustum::distanceToPlane(const glm::vec4& plane, const Vec3& point)
{
    return plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w;
}
