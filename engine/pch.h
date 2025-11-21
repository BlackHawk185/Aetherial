// Precompiled header for MMORPG Engine
#pragma once

// Standard library
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Prevent Windows headers from defining min/max macros that break std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// ENet networking
#include <enet/enet.h>

// Vulkan (for layout tracker)
#include <vulkan/vulkan.h>
#include "Rendering/Vulkan/VulkanLayoutTracker.h"

// Engine-wide macros and config
// Project headers should be included in individual source files after pch.h
// to avoid circular dependencies. Add only stable, external headers here.

// Undefine dangerous macros just in case
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
