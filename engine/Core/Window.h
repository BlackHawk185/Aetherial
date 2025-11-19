#pragma once

#include <string>
#include <functional>

// Forward declare GLFW types
struct GLFWwindow;

namespace Engine {
namespace Core {

/**
 * Window management system using GLFW
 * Handles window creation, OpenGL context, and basic event handling
 */
class Window {
public:
    Window();
    ~Window();

    /**
     * Initialize GLFW and create window with OpenGL context
     * @param width Window width in pixels
     * @param height Window height in pixels
     * @param title Window title
     * @param enableDebug Enable OpenGL debug output
     * @return true if successful
     */
    bool initialize(int width = 1280, int height = 720, const std::string& title = "MMORPG Engine", bool enableDebug = false);

    /**
     * Shutdown and cleanup
     */
    void shutdown();

    /**
     * Check if window should close
     */
    bool shouldClose() const;

    /**
     * Set window should close flag
     */
    void setShouldClose(bool shouldClose);

    /**
     * Check if a key is currently pressed
     */
    bool isKeyPressed(int key) const;

    /**
     * Poll events and swap buffers
     */
    void update();

    /**
     * Get window handle
     */
    GLFWwindow* getHandle() const { return m_window; }

    /**
     * Get window dimensions
     */
    void getSize(int& width, int& height) const;

    /**
     * Check if window is initialized
     */
    bool isInitialized() const { return m_window != nullptr; }

    /**
     * Set window callbacks
     */
    void setKeyCallback(std::function<void(int, int, int, int)> callback) { m_keyCallback = callback; }
    void setMouseCallback(std::function<void(double, double)> callback) { m_mouseCallback = callback; }
    void setResizeCallback(std::function<void(int, int)> callback) { m_resizeCallback = callback; }

private:
    GLFWwindow* m_window;
    int m_width;
    int m_height;
    std::string m_title;

    // Event callbacks
    std::function<void(int, int, int, int)> m_keyCallback;      // key, scancode, action, mods
    std::function<void(double, double)> m_mouseCallback;        // xpos, ypos
    std::function<void(int, int)> m_resizeCallback;             // width, height

    // Static GLFW callbacks (forward to instance methods)
    static void glfwKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void glfwCursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static void glfwFramebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void glfwErrorCallback(int error, const char* description);

    // Internal methods
    bool setupCallbacks();

public:
    // Debug information
    void printGLFWInfo() const;
};

} // namespace Core
} // namespace Engine
