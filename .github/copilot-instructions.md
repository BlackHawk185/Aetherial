# AI Assistant Guidance for MMORPG Engine Project

This file is for the AI assistant (GitHub Copilot) to reference when generating code, explanations, or project structure for the user.


## Data Layout Directive: SoA (Structure of Arrays)

- **Use SoA (Structure of Arrays) for all entity/component storage and performance-critical systems.**
- When designing systems that manage many objects (players, islands, voxels, etc.), prefer SoA for better cache efficiency and SIMD/vectorization potential.
- Example: Instead of `std::vector<Player>`, use separate arrays for each property: `std::vector<float> positionsX, positionsY, positionsZ; std::vector<int> health;` etc.

## Key Reminders

- **User is new to game development**: Explanations should be clear, concise, and avoid assuming prior game dev knowledge.
- **Bite-sized steps**: Break down tasks into small, manageable pieces. Avoid overwhelming the user with large code dumps or complex changes at once.
- **Explain rationale**: Briefly explain why each file, function, or system is being created or changed.
- **Comment code**: Add comments in code to clarify purpose, especially for game engine or C++-specific patterns.
- **Modular design**: Keep code modular and organized per the project structure in the prompt.
- **Iterative approach**: After each step, check with the user before proceeding to the next major system or feature.
- **Cross-platform**: Ensure all code and build steps are cross-platform (Windows, Linux, macOS) unless otherwise specified.
- **Custom physics**: The engine uses a custom voxel-face-based collision system, not external physics engines.
- **Console support**: Keep future console support in mind (no platform-specific hacks).
- **Unified networking**: All game modes use the network layer - avoid direct GameState access from clients.
- **Server authority**: Game state changes should go through server validation, even in integrated mode.
- **Delete over deprecate**: Remove old systems entirely rather than commenting them out. This project evolves rapidly and needs aggressive cleanup.
	- On Windows, always use PowerShell's `Remove-Item` command to delete files (e.g., `Remove-Item -Path "path\to\file" -Force`). This ensures reliable file removal even if standard methods fail.

## Code Quality Guidelines

- **Clean output**: Minimize console logging - only essential messages. Avoid excessive debug output that clutters the console.
- **Performance-first**: When implementing new features, consider cache efficiency, memory layout, and CPU/GPU optimization.
- **Integration over isolation**: New features should integrate cleanly with existing systems rather than being bolted on.
- **Test incrementally**: After significant changes, ensure the engine builds and runs before proceeding to the next feature.
- **Aggressive cleanup**: Remove deprecated code entirely rather than commenting it out. The project evolves rapidly and accumulating old code hurts maintainability.
- **Event-driven architecture**: Prefer event-driven systems over continuous polling for better performance and cleaner code organization.
- **NO FALLBACK CODE EVER**: NEVER write "safety net", "fallback", "just in case", or "edge case" code. If the main solution doesn't work, fix the main solution. Do not add defensive/redundant checks that paper over problems. The user will remove these immediately and finds them wasteful.

## Current Engine Architecture Status

### Core Systems (Implemented)
- **ECS Framework**: Entity-Component-System for game objects
- **Custom Physics**: Voxel-face-based collision detection with dual-mesh generation
- **Rendering**: OpenGL-based renderer with Dear ImGui dev tools
- **Input**: Camera controls (WASD+mouse, space for jump)
- **Time Effects**: Time manipulation system (keys 1-5, 0, T for various effects)
- **Networking**: ENet-based client-server architecture with unified networking paths

### Game-Specific Features (Implemented)
- **Floating Islands**: Procedurally generated voxel islands with custom collision detection
- **Player Movement**: Custom sphere-face collision with real-time network synchronization
- **Voxel Collision**: Friction-based collision detection using exposed voxel faces
- **Block Interaction**: Real-time block break/place with network validation
- **Dual-Mesh Generation**: Single-pass generation of both render and collision meshes
- **Event-Driven Lighting**: Dynamic lighting system integrated with mesh generation and day/night cycle
- **Day/Night Cycle**: Real-time sun movement with dynamic lighting updates

### Network Architecture (Implemented)
- **Unified Networking**: All modes (integrated + remote) use identical network layer
- **ENet Protocol**: Reliable UDP with automatic packet management
- **Compression**: ~98% compression ratio for world data transmission
- **Client-Server Model**: Server-authoritative with client prediction ready
- **Multiple Modes**: Integrated (local server + networked client), dedicated server, client-only

### Technical Details
- **Build System**: CMake with Visual Studio support
- **Launch Configurations**: VS Code integration with 3 run modes
- **Cross-Platform**: Windows, Linux, macOS compatibility maintained
- **Dependencies**: ENet, GLFW, Dear ImGui, OpenGL
- **Platform**: Currently Windows-focused but designed for cross-platform
- **Performance**: Optimized for 60+ FPS with multiple islands and custom collision
- **Physics Architecture**: Custom voxel-face collision system with dual-mesh generation

---

This file is for internal use by the AI assistant and can be updated as the project evolves.
