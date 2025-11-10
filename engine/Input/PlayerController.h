// PlayerController.h - Unified player input, physics, and camera control
// This class cleanly separates player logic from rendering (GameClient) and pure view math (Camera)
#pragma once

#include "Camera.h"
#include "../Math/Vec3.h"
#include "../Physics/PhysicsSystem.h"

// Forward declarations
struct GLFWwindow;
class IslandChunkSystem;

/**
 * PlayerController handles all player-related logic:
 * - Input gathering (keyboard/mouse)
 * - Physics simulation (movement, gravity, collision)
 * - Camera control (position, smoothing)
 * 
 * This cleanly separates concerns:
 * - Camera = Pure view/projection math (no input, no physics)
 * - PlayerController = Input + physics + camera positioning
 * - GameClient = Rendering + UI + game loop orchestration
 */
class PlayerController
{
public:
    PlayerController();
    ~PlayerController();

    // ================================
    // LIFECYCLE
    // ================================
    
    /**
     * Initialize the player controller
     */
    void initialize(const Vec3& initialPosition);
    
    /**
     * Update player physics and camera
     * @param window - GLFW window for input
     * @param deltaTime - Time since last frame
     * @param islandSystem - Island system for collision detection
     * @param physics - Physics system for collision queries
     */
    void update(GLFWwindow* window, float deltaTime, IslandChunkSystem* islandSystem, PhysicsSystem* physics);
    
    /**
     * Process mouse input for camera rotation
     * @param window - GLFW window for input
     */
    void processMouse(GLFWwindow* window);

    // ================================
    // CAMERA ACCESS
    // ================================
    
    Camera& getCamera() { return m_camera; }
    const Camera& getCamera() const { return m_camera; }

    // ================================
    // PHYSICS STATE
    // ================================
    
    Vec3 getPosition() const { return m_physicsPosition; }
    Vec3 getVelocity() const { return m_playerVelocity; }
    Vec3 getEyePosition() const; // Position + eye height offset
    bool isGrounded() const { return m_isGrounded; }
    
    /**
     * Set player position (for spawning, teleporting, etc.)
     * Also updates camera position to match
     */
    void setPosition(const Vec3& position);

    // ================================
    // DEBUG / SETTINGS
    // ================================
    
    void setNoclipMode(bool enabled) { m_noclipMode = enabled; }
    bool isNoclipMode() const { return m_noclipMode; }
    
    void setCameraSmoothing(bool enabled) { m_disableCameraSmoothing = !enabled; }
    bool isCameraSmoothingEnabled() const { return !m_disableCameraSmoothing; }
    
    // Piloting state (for vehicle control)
    void setPiloting(bool piloting, uint32_t islandID = 0) { m_isPiloting = piloting; m_pilotedIslandID = islandID; }
    bool isPiloting() const { return m_isPiloting; }
    uint32_t getPilotedIslandID() const { return m_pilotedIslandID; }

    // Check if UI is blocking input (e.g., periodic table open)
    void setUIBlocking(bool blocking) { m_uiBlocking = blocking; }

private:
    // ================================
    // INTERNAL STATE
    // ================================
    
    // Camera system
    Camera m_camera;
    
    // Physics state
    Vec3 m_playerVelocity{0.0f, 0.0f, 0.0f};        // Player's own velocity (input, gravity, jumps)
    Vec3 m_physicsPosition{0.0f, 0.0f, 0.0f};       // Actual hitbox position (can jitter)
    bool m_isGrounded = false;
    bool m_jumpPressed = false;
    
    // Movement settings
    float m_moveSpeed = 30.0f;          // Walk speed (adjusted 1.5x for larger world scale)
    float m_jumpStrength = 8.0f;       // Jump velocity (adjusted 1.5x for larger world scale)
    float m_gravity = 20.0f;            // Gravity acceleration
    float m_airControl = 0.2f;          // How much control in air (reduced from 0.4)
    float m_groundFriction = 0.85f;     // Ground friction multiplier
    float m_airFriction = 0.94f;        // Air resistance (reduced from 0.98 for more drag)
    float m_cameraSmoothing = 0.15f;    // Camera interpolation speed (lower = smoother)
    
    // Capsule collision dimensions (player is 2.9 blocks tall, ~1.1 blocks wide)
    float m_capsuleRadius = 0.55f;      // Horizontal radius (requires 2-block gap, won't fit through 1)
    float m_capsuleHeight = 2.9f;       // Total height including caps (fits through 3-block gaps)
    float m_eyeHeightOffset = 1.2f;     // Eye level above physics center
    float m_maxStepHeight = 1.1f;       // Maximum height the player can step up (1 block + margin)
    
    // Debug modes
    bool m_noclipMode = false;          // Debug: disable physics
    bool m_disableCameraSmoothing = false;  // Debug: disable camera LERP to see raw physics
    
    // Piloting state (for vehicle control)
    bool m_isPiloting = false;          // Is player currently piloting a vehicle?
    uint32_t m_pilotedIslandID = 0;     // Which island is being piloted
    
    // UI state
    bool m_uiBlocking = false;          // Is UI open and blocking input?
    
    // ================================
    // INTERNAL METHODS
    // ================================
    
    /**
     * Handle noclip (free-flying) movement
     */
    void updateNoclip(GLFWwindow* window, float deltaTime);
    
    /**
     * Handle physics-based player movement
     */
    void updatePhysics(GLFWwindow* window, float deltaTime, IslandChunkSystem* islandSystem, PhysicsSystem* physics);
    
    /**
     * Gather input direction from keyboard
     */
    Vec3 getInputDirection(GLFWwindow* window) const;
    
    /**
     * Apply camera smoothing to hide physics jitter
     */
    void updateCameraPosition(float deltaTime);
};
