// PlayerController.cpp - Unified player input, physics, and camera control
#include "PlayerController.h"

#include <GLFW/glfw3.h>
#include <cmath>
#include <iostream>

#include "../World/IslandChunkSystem.h"
#include "../Profiling/Profiler.h"

PlayerController::PlayerController()
{
    // Initialize camera with default values
}

PlayerController::~PlayerController()
{
}

void PlayerController::initialize(const Vec3& initialPosition)
{
    // Set initial position
    m_physicsPosition = initialPosition;
    m_camera.position = initialPosition + Vec3(0.0f, m_eyeHeightOffset, 0.0f);
    
    // Initialize velocities
    m_playerVelocity = Vec3(0.0f, 0.0f, 0.0f);
    m_isGrounded = false;
    m_jumpPressed = false;
}

void PlayerController::update(GLFWwindow* window, float deltaTime, IslandChunkSystem* islandSystem, PhysicsSystem* physics)
{
    if (m_noclipMode)
    {
        updateNoclip(window, deltaTime);
    }
    else
    {
        updatePhysics(window, deltaTime, islandSystem, physics);
    }
    
    // Always update camera position based on physics position
    updateCameraPosition(deltaTime);
}

void PlayerController::processMouse(GLFWwindow* window)
{
    if (m_uiBlocking)
    {
        return; // Don't process mouse if UI is open
    }
    
    static bool firstMouse = true;
    static double lastX = 640.0;
    static double lastY = 360.0;
    
    double mouseX, mouseY;
    glfwGetCursorPos(window, &mouseX, &mouseY);
    
    if (firstMouse)
    {
        lastX = mouseX;
        lastY = mouseY;
        firstMouse = false;
        return;
    }
    
    float xOffset = static_cast<float>(mouseX - lastX);
    float yOffset = static_cast<float>(lastY - mouseY); // Reversed for correct Y axis
    lastX = mouseX;
    lastY = mouseY;
    
    xOffset *= m_camera.sensitivity;
    yOffset *= m_camera.sensitivity;
    
    m_camera.yaw += xOffset;
    m_camera.pitch += yOffset;
    
    // Constrain pitch to prevent gimbal lock
    if (m_camera.pitch > 89.0f)
        m_camera.pitch = 89.0f;
    if (m_camera.pitch < -89.0f)
        m_camera.pitch = -89.0f;
    
    // Update camera vectors based on new yaw/pitch
    m_camera.updateCameraVectors();
}

Vec3 PlayerController::getEyePosition() const
{
    return m_physicsPosition + Vec3(0.0f, m_eyeHeightOffset, 0.0f);
}

void PlayerController::setPosition(const Vec3& position)
{
    m_physicsPosition = position;
    m_camera.position = position + Vec3(0.0f, m_eyeHeightOffset, 0.0f);
    m_playerVelocity = Vec3(0.0f, 0.0f, 0.0f);
}

void PlayerController::updateNoclip(GLFWwindow* window, float deltaTime)
{
    // Free-flying movement for debugging
    Vec3 movement(0, 0, 0);
    
    if (!m_uiBlocking)
    {
        float flySpeed = 30.0f;
        
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
            movement = movement + m_camera.front * flySpeed * deltaTime;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
            movement = movement - m_camera.front * flySpeed * deltaTime;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
            movement = movement - m_camera.right * flySpeed * deltaTime;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
            movement = movement + m_camera.right * flySpeed * deltaTime;
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
            movement = movement + m_camera.up * flySpeed * deltaTime;
        if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
            movement = movement - m_camera.up * flySpeed * deltaTime;
    }
    
    m_physicsPosition = m_physicsPosition + movement;
    m_camera.position = m_physicsPosition; // No eye offset in noclip
}

void PlayerController::updatePhysics(GLFWwindow* window, float deltaTime, IslandChunkSystem* islandSystem, PhysicsSystem* physics)
{
    PROFILE_FUNCTION();
    (void)islandSystem; // Reserved for future island-specific physics interactions
    
    if (!physics)
        return; // Cannot update physics without physics system
    
    // ==========================================
    // PHASE 0: UPDATE STEP-UP ANIMATION
    // ==========================================
    
    if (m_isStepping)
    {
        m_stepProgress += deltaTime / m_stepDuration;
        
        if (m_stepProgress >= 1.0f)
        {
            // Step complete
            m_isStepping = false;
            m_stepProgress = 0.0f;
        }
        else
        {
            // Interpolate height during step
            float t = m_stepProgress;
            // Use smoothstep for smooth animation: 3t^2 - 2t^3
            float smoothT = t * t * (3.0f - 2.0f * t);
            float currentStepHeight = m_stepStartHeight + (m_stepTargetHeight - m_stepStartHeight) * smoothT;
            m_physicsPosition.y = currentStepHeight;
        }
    }
    
    // ==========================================
    // PHASE 1: GATHER INPUT
    // ==========================================
    
    Vec3 inputDirection = getInputDirection(window);
    
    // Check for jump input
    bool jumpThisFrame = false;
    if (!m_uiBlocking)
    {
        jumpThisFrame = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
    }
    
    // If jumping, cancel any active step animation
    if (jumpThisFrame && m_isStepping)
    {
        m_isStepping = false;
        m_stepProgress = 0.0f;
    }
    
    // ==========================================
    // PHASE 2: DETECT GROUND STATE
    // ==========================================
    
    const float raycastMargin = 0.1f;
    GroundInfo groundInfo = physics->detectGroundCapsule(m_physicsPosition, m_capsuleRadius,
                                                          m_capsuleHeight, raycastMargin);
    m_isGrounded = groundInfo.isGrounded;
    
    // ==========================================
    // PHASE 3: APPLY PHYSICS
    // ==========================================
    
    // Apply gravity (not during stepping)
    if (!m_isStepping)
    {
        m_playerVelocity.y -= m_gravity * deltaTime;
    }
    
    // Ground physics and jumping
    if (m_isGrounded)
    {
        // Stop falling when on ground
        if (m_playerVelocity.y < 0)
        {
            m_playerVelocity.y = 0;
        }
        
        // Handle jump
        if (jumpThisFrame && !m_jumpPressed)
        {
            m_playerVelocity.y = m_jumpStrength;
        }
        
        // Apply ground friction
        m_playerVelocity.x *= m_groundFriction;
        m_playerVelocity.z *= m_groundFriction;
    }
    else
    {
        // Check for climbing - if holding space in air and moving into a wall
        if (jumpThisFrame && inputDirection.lengthSquared() > 0.01f)
        {
            // Try moving forward slightly to detect wall collision
            Vec3 forwardTest = m_physicsPosition + (inputDirection.normalized() * 0.3f);
            Vec3 climbNormal;
            if (physics->checkCapsuleCollision(forwardTest, m_capsuleRadius, m_capsuleHeight, climbNormal, nullptr))
            {
                // Wall detected! Check if we can climb over it (max 3 blocks)
                // Check 1 block forward and 1 block above player's top
                Vec3 climbCheckPos = m_physicsPosition + (inputDirection.normalized() * 1.0f);
                climbCheckPos.y += (m_capsuleHeight * 0.5f) + 1.0f; // Top of capsule + 1 block
                
                Vec3 topCheckNormal;
                if (!physics->checkCapsuleCollision(climbCheckPos, m_capsuleRadius, m_capsuleHeight, topCheckNormal, nullptr))
                {
                    // No obstruction above - can climb!
                    m_playerVelocity.y = m_climbSpeed;
                }
                // else: Wall is too tall (>3 blocks), don't allow climbing
            }
        }
        
        // Apply air resistance
        m_playerVelocity.x *= m_airFriction;
        m_playerVelocity.z *= m_airFriction;
    }
    
    m_jumpPressed = jumpThisFrame;
    
    // Apply input acceleration (with slowdown during stepping and in air)
    float controlStrength = m_isGrounded ? 1.0f : m_airControl;
    float speedMultiplier = m_isStepping ? m_stepSlowdown : 1.0f;
    
    // Reduce max speed in air (70% of ground speed)
    if (!m_isGrounded) {
        speedMultiplier *= 0.7f;
    }
    
    Vec3 targetHorizontalVelocity = inputDirection * m_moveSpeed * speedMultiplier;
    Vec3 currentHorizontalVelocity = Vec3(m_playerVelocity.x, 0, m_playerVelocity.z);
    
    Vec3 velocityDelta = (targetHorizontalVelocity - currentHorizontalVelocity) * controlStrength * 10.0f * deltaTime;
    m_playerVelocity.x += velocityDelta.x;
    m_playerVelocity.z += velocityDelta.z;
    
    // Calculate intended movement
    Vec3 intendedMovement = m_playerVelocity * deltaTime;
    
    // ==========================================
    // PHASE 4: COLLISION DETECTION
    // ==========================================
    
    Vec3 intendedPosition = m_physicsPosition + intendedMovement;
    
    Vec3 collisionNormal;
    const FloatingIsland* collidingIsland = nullptr;
    if (physics->checkCapsuleCollision(intendedPosition, m_capsuleRadius, m_capsuleHeight,
                                       collisionNormal, &collidingIsland))
    {
        // Collision detected - use axis-separated movement
        Vec3 relativeMovement = intendedMovement;
        
        // Try axis-separated movement
        // Process Y first to allow jumping/climbing to work smoothly
        Vec3 testPos;
        
        // Try Y (vertical movement - jumping/falling/climbing)
        testPos = m_physicsPosition + Vec3(0, relativeMovement.y, 0);
        if (!physics->checkCapsuleCollision(testPos, m_capsuleRadius, m_capsuleHeight, collisionNormal, nullptr))
        {
            m_physicsPosition = testPos;
        }
        else
        {
            m_playerVelocity.y = 0;
        }
        
        // Try X with step-up (includes unstuck logic)
        testPos = m_physicsPosition + Vec3(relativeMovement.x, 0, 0);
        if (!physics->checkCapsuleCollision(testPos, m_capsuleRadius, m_capsuleHeight, collisionNormal, nullptr))
        {
            m_physicsPosition = testPos;
        }
        else
        {
            // Collision detected - try step-up
            bool stepped = false;
            
            // Step-up works in TWO cases:
            // 1. When grounded (normal terrain climbing)
            // 2. When stuck inside terrain (unstuck mechanism - always enabled)
            if (!m_isStepping) // Don't interrupt existing step
            {
                // Try stepping up in increments to find the minimum step height
                // Use 0.4f increment (4x more aggressive) for faster unstuck
                for (float stepHeight = 0.4f; stepHeight <= m_maxStepHeight; stepHeight += 0.4f)
                {
                    Vec3 stepUpPos = m_physicsPosition + Vec3(relativeMovement.x, stepHeight, 0);
                    if (!physics->checkCapsuleCollision(stepUpPos, m_capsuleRadius, m_capsuleHeight, collisionNormal, nullptr))
                    {
                        // Initialize step-up animation
                        m_isStepping = true;
                        m_stepProgress = 0.0f;
                        m_stepStartHeight = m_physicsPosition.y;
                        m_stepTargetHeight = m_physicsPosition.y + stepHeight;
                        
                        // Apply horizontal movement immediately
                        m_physicsPosition.x += relativeMovement.x;
                        stepped = true;
                        break;
                    }
                }
            }
            
            if (!stepped)
            {
                m_playerVelocity.x = 0;
            }
        }
        
        // Try Z with step-up (includes unstuck logic)
        testPos = m_physicsPosition + Vec3(0, 0, relativeMovement.z);
        if (!physics->checkCapsuleCollision(testPos, m_capsuleRadius, m_capsuleHeight, collisionNormal, nullptr))
        {
            m_physicsPosition = testPos;
        }
        else
        {
            // Collision detected - try step-up
            bool stepped = false;
            
            // Step-up works in TWO cases:
            // 1. When grounded (normal terrain climbing)
            // 2. When stuck inside terrain (unstuck mechanism - always enabled)
            if (!m_isStepping) // Don't interrupt existing step
            {
                // Try stepping up in increments to find the minimum step height
                for (float stepHeight = 0.1f; stepHeight <= m_maxStepHeight; stepHeight += 0.1f)
                {
                    Vec3 stepUpPos = m_physicsPosition + Vec3(0, stepHeight, relativeMovement.z);
                    if (!physics->checkCapsuleCollision(stepUpPos, m_capsuleRadius, m_capsuleHeight, collisionNormal, nullptr))
                    {
                        // Initialize step-up animation
                        m_isStepping = true;
                        m_stepProgress = 0.0f;
                        m_stepStartHeight = m_physicsPosition.y;
                        m_stepTargetHeight = m_physicsPosition.y + stepHeight;
                        
                        // Apply horizontal movement immediately
                        m_physicsPosition.z += relativeMovement.z;
                        stepped = true;
                        break;
                    }
                }
            }
            
            if (!stepped)
            {
                m_playerVelocity.z = 0;
            }
        }
        
        // Apply island movement
        if (collidingIsland)
        {
            m_physicsPosition = m_physicsPosition + (collidingIsland->velocity * deltaTime);
        }
    }
    else
    {
        // No collision - move freely
        m_physicsPosition = intendedPosition;
        
        // If grounded, move with the island (raycast-based riding)
        if (m_isGrounded && groundInfo.standingOnIslandID != 0)
        {
            FloatingIsland* island = islandSystem->getIsland(groundInfo.standingOnIslandID);
            if (island)
            {
                // Apply linear velocity
                m_physicsPosition = m_physicsPosition + (groundInfo.groundVelocity * deltaTime);
                
                // Apply angular velocity (rotation around island center)
                if (island->angularVelocity.lengthSquared() > 0.0001f)
                {
                    // Get player's offset from island center
                    Vec3 offset = m_physicsPosition - island->physicsCenter;
                    
                    // Rotate offset around Y axis
                    float angleChange = island->angularVelocity.y * deltaTime;
                    float cosAngle = std::cos(angleChange);
                    float sinAngle = std::sin(angleChange);
                    
                    Vec3 rotatedOffset;
                    rotatedOffset.x = offset.x * cosAngle + offset.z * sinAngle;
                    rotatedOffset.y = offset.y;
                    rotatedOffset.z = -offset.x * sinAngle + offset.z * cosAngle;
                    
                    // Update position
                    m_physicsPosition = island->physicsCenter + rotatedOffset;
                    
                    // Rotate camera yaw to match island rotation (negative because camera is inverted)
                    m_camera.yaw -= angleChange * (180.0f / 3.14159265f);
                    m_camera.updateCameraVectors();
                }
            }
        }
    }
    
    // ==========================================
    // PHASE 5: UPDATE PILOTING STATE
    // ==========================================
    
    if (m_isGrounded)
    {
        m_pilotedIslandID = groundInfo.standingOnIslandID;
    }
    else
    {
        if (!m_isPiloting)
        {
            m_pilotedIslandID = 0;
        }
    }
}

Vec3 PlayerController::getInputDirection(GLFWwindow* window) const
{
    Vec3 inputDirection(0, 0, 0);
    
    if (m_uiBlocking)
    {
        return inputDirection; // No input if UI is blocking
    }
    
    // If piloting and grounded, A/D rotate the vehicle
    if (m_isPiloting && m_isGrounded && m_pilotedIslandID != 0)
    {
        // Only W/S for forward/backward when piloting
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        {
            inputDirection = inputDirection + m_camera.front;
        }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        {
            inputDirection = inputDirection - m_camera.front;
        }
        // Note: A/D handled separately for rotation in GameClient
    }
    else
    {
        // Normal WASD movement
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        {
            inputDirection = inputDirection + m_camera.front;
        }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        {
            inputDirection = inputDirection - m_camera.front;
        }
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        {
            inputDirection = inputDirection - m_camera.right;
        }
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        {
            inputDirection = inputDirection + m_camera.right;
        }
    }
    
    // Flatten to horizontal plane
    inputDirection.y = 0;
    if (inputDirection.length() > 0.001f)
    {
        inputDirection = inputDirection.normalized();
    }
    
    return inputDirection;
}

void PlayerController::updateCameraPosition(float deltaTime)
{
    (void)deltaTime; // Unused - no smoothing applied
    
    Vec3 eyePosition = m_physicsPosition + Vec3(0.0f, m_eyeHeightOffset, 0.0f);
    
    // Snap camera directly to target position (no smoothing)
    m_camera.position = eyePosition;
}
