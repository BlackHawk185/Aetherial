#include "Window.h"
#include <GLFW/glfw3.h>
#include <glad/gl.h>
#include <iostream>

namespace Engine {
namespace Core {

Window::Window() 
    : m_window(nullptr)
    , m_width(0)
    , m_height(0)
{
}

Window::~Window() {
    shutdown();
}

bool Window::initialize(int width, int height, const std::string& title, bool enableDebug) {
    m_width = width;
    m_height = height;
    m_title = title;

    std::cout << "Window: Initializing GLFW..." << std::endl;

    // Set GLFW error callback before initialization
    glfwSetErrorCallback(glfwErrorCallback);

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Window: Failed to initialize GLFW" << std::endl;
        return false;
    }

    // Configure GLFW for OpenGL 4.6 Core Profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // macOS compatibility
    
    // Enable debug context for development builds
    #ifdef _DEBUG
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);
    #endif
    
    // Additional window hints
    glfwWindowHint(GLFW_RESIZABLE, GL_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 4); // 4x MSAA

    std::cout << "Window: Creating GLFW window (" << width << "x" << height << ")..." << std::endl;

    // Create window
    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        std::cerr << "Window: Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return false;
    }

    // Make OpenGL context current
    glfwMakeContextCurrent(m_window);

    // Set up FPS-style mouse capture
    glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    
    // Load OpenGL functions
    if (!loadOpenGL(enableDebug)) {
        std::cerr << "Window: Failed to load OpenGL" << std::endl;
        glfwDestroyWindow(m_window);
        glfwTerminate();
        m_window = nullptr;
        return false;
    }

    // Set up callbacks
    if (!setupCallbacks()) {
        std::cerr << "Window: Failed to setup callbacks" << std::endl;
        return false;
    }

    // Disable V-Sync for maximum performance (uncapped FPS)
    glfwSwapInterval(0);

    // Print basic system information
    std::cout << "OpenGL " << glGetString(GL_VERSION) << " (" << glGetString(GL_RENDERER) << ")" << std::endl;

    std::cout << "Window: Initialization complete" << std::endl;
    return true;
}

void Window::shutdown() {
    if (m_window) {
        std::cout << "Window: Shutting down..." << std::endl;
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    
    glfwTerminate();
    std::cout << "Window: Shutdown complete" << std::endl;
}

bool Window::shouldClose() const {
    return m_window ? glfwWindowShouldClose(m_window) : true;
}

void Window::setShouldClose(bool shouldClose) {
    if (m_window) {
        glfwSetWindowShouldClose(m_window, shouldClose ? GLFW_TRUE : GLFW_FALSE);
    }
}

bool Window::isKeyPressed(int key) const {
    if (!m_window) return false;
    return glfwGetKey(m_window, key) == GLFW_PRESS;
}

void Window::update() {
    if (!m_window) return;

    // Swap front and back buffers
    glfwSwapBuffers(m_window);

    // Poll for and process events
    glfwPollEvents();
}

void Window::getSize(int& width, int& height) const {
    if (m_window) {
        glfwGetFramebufferSize(m_window, &width, &height);
    } else {
        width = m_width;
        height = m_height;
    }
}

bool Window::setupCallbacks() {
    if (!m_window) return false;

    // Store this instance pointer in GLFW window user pointer
    glfwSetWindowUserPointer(m_window, this);

    // Set GLFW callbacks
    glfwSetKeyCallback(m_window, glfwKeyCallback);
    glfwSetCursorPosCallback(m_window, glfwCursorPosCallback);
    glfwSetFramebufferSizeCallback(m_window, glfwFramebufferSizeCallback);

    return true;
}

bool Window::loadOpenGL(bool enableDebug) {
    std::cout << "Window: Loading OpenGL with GLAD..." << std::endl;

    // Load OpenGL functions using GLAD
    if (!gladLoadGL(glfwGetProcAddress)) {
        std::cerr << "Window: Failed to initialize GLAD" << std::endl;
        return false;
    }

    // Enable KHR_debug output only if debug is enabled and we're in a debug build
    #ifdef _DEBUG
    if (enableDebug) {
        GLint flags = 0;
        glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
        if (flags & GL_CONTEXT_FLAG_DEBUG_BIT)
        {
            std::cout << "Window: Enabling OpenGL debug output" << std::endl;
            glEnable(GL_DEBUG_OUTPUT);
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            auto cb = [](GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                         const GLchar* message, const void* userParam)
            {
                (void)length; (void)userParam; (void)source; (void)type; (void)id;
                
                // Filter out NOTIFY level messages to reduce spam
                if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) {
                    return;
                }
                
                const char* sev = severity == GL_DEBUG_SEVERITY_HIGH ? "HIGH" :
                                  severity == GL_DEBUG_SEVERITY_MEDIUM ? "MEDIUM" :
                                  severity == GL_DEBUG_SEVERITY_LOW ? "LOW" : "NOTIFY";
                std::cerr << "[GL DEBUG][" << sev << "] " << message << std::endl;
            };
            glDebugMessageCallback((GLDEBUGPROC)cb, nullptr);
            glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
        }
    }
    #else
    (void)enableDebug; // Suppress unused parameter warning in release builds
    #endif

    std::cout << "Window: OpenGL loaded successfully" << std::endl;
    return true;
}

void Window::printOpenGLInfo() const {
    std::cout << "=== OpenGL Information ===" << std::endl;
    std::cout << "Vendor: " << glGetString(GL_VENDOR) << std::endl;
    std::cout << "Renderer: " << glGetString(GL_RENDERER) << std::endl;
    std::cout << "Version: " << glGetString(GL_VERSION) << std::endl;
    std::cout << "GLSL Version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << std::endl;
    std::cout << "=========================" << std::endl;
}

void Window::printGLFWInfo() const {
    int major, minor, revision;
    glfwGetVersion(&major, &minor, &revision);
    std::cout << "=== GLFW Information ===" << std::endl;
    std::cout << "Version: " << major << "." << minor << "." << revision << std::endl;
    std::cout << "========================" << std::endl;
}

// Static GLFW callback implementations
void Window::glfwErrorCallback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

void Window::glfwKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    Window* windowInstance = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (windowInstance && windowInstance->m_keyCallback) {
        windowInstance->m_keyCallback(key, scancode, action, mods);
    }

    // Built-in ESC to close window
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
}

void Window::glfwCursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    Window* windowInstance = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (windowInstance && windowInstance->m_mouseCallback) {
        windowInstance->m_mouseCallback(xpos, ypos);
    }
}

void Window::glfwFramebufferSizeCallback(GLFWwindow* window, int width, int height) {
    // Update OpenGL viewport
    glViewport(0, 0, width, height);

    Window* windowInstance = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (windowInstance) {
        windowInstance->m_width = width;
        windowInstance->m_height = height;
        
        if (windowInstance->m_resizeCallback) {
            windowInstance->m_resizeCallback(width, height);
        }
    }
}

} // namespace Core
} // namespace Engine
