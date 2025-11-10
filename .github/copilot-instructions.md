# AI Assistant Directives

## Core Principles
- **Minimal code**: Write only what's needed. No defensive checks, fallbacks, or "just in case" logic.
- **Delete aggressively**: Remove dead code immediately. Never comment out or deprecate.
- **Performance-first**: SoA layout for entity storage. Cache-friendly, SIMD-ready.
- **Server authority**: All state changes validated server-side, even in integrated mode.
- **Event-driven**: No polling loops. React to events.
- **Cross-platform**: Windows/Linux/macOS. No platform-specific hacks.

## Code Generation Rules
1. Write the solution that works
2. No null checks unless actively crashing
3. No try-catch unless handling expected failures
4. No fallback paths
5. Minimal logging (errors only)
6. No unused variables, imports, or functions
7. Data-driven design for moddability (JSON configs, external assets)

## Post-Generation Checklist
After writing code, immediately:
1. Delete unused code
2. Remove defensive checks
3. Strip excessive logging
4. Verify minimal solution

## Industry Standard Violations
Alert user when doing:
- Non-standard architecture patterns
- Performance anti-patterns
- Platform-specific hacks
- Departures from ECS/data-oriented design

## Architecture Notes
- Custom voxel-face collision (no physics engine)
- ENet networking with ~98% compression
- OpenGL + Dear ImGui
- CMake build system
