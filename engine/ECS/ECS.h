// ECS.h - Structure-of-Arrays Entity Component System for MMORPG
#pragma once
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "Math/Vec3.h"

using EntityID = uint32_t;
constexpr EntityID INVALID_ENTITY = 0;

// Base component storage interface
class ComponentStorageBase
{
   public:
    virtual ~ComponentStorageBase() = default;
    virtual void removeEntity(EntityID entity) = 0;
    virtual size_t size() const = 0;
};

// SoA Component storage template
template <typename T>
class ComponentStorage : public ComponentStorageBase
{
   public:
    std::vector<EntityID> entities;
    std::vector<T> components;
    std::unordered_map<EntityID, size_t> entityToIndex;

    T* addComponent(EntityID entity, const T& component = T{})
    {
        if (hasComponent(entity))
            return getComponent(entity);

        size_t index = entities.size();
        entities.push_back(entity);
        components.push_back(component);
        entityToIndex[entity] = index;
        return &components[index];
    }

    T* getComponent(EntityID entity)
    {
        auto it = entityToIndex.find(entity);
        if (it == entityToIndex.end())
            return nullptr;
        return &components[it->second];
    }

    bool hasComponent(EntityID entity) const
    {
        return entityToIndex.find(entity) != entityToIndex.end();
    }

    void removeEntity(EntityID entity) override
    {
        auto it = entityToIndex.find(entity);
        if (it == entityToIndex.end())
            return;

        size_t index = it->second;
        size_t lastIndex = entities.size() - 1;

        if (index != lastIndex)
        {
            // Swap with last element
            entities[index] = entities[lastIndex];
            components[index] = components[lastIndex];
            entityToIndex[entities[index]] = index;
        }

        entities.pop_back();
        components.pop_back();
        entityToIndex.erase(entity);
    }

    size_t size() const override
    {
        return entities.size();
    }
};

// Core ECS World
class ECSWorld
{
   public:
    EntityID createEntity();
    EntityID createEntityWithID(EntityID id);  // Create entity with specific ID (for network sync)
    void destroyEntity(EntityID entity);

    template <typename T>
    T* addComponent(EntityID entity, const T& component = T{})
    {
        return getStorage<T>()->addComponent(entity, component);
    }

    template <typename T>
    T* getComponent(EntityID entity)
    {
        return getStorage<T>()->getComponent(entity);
    }

    template <typename T>
    ComponentStorage<T>* getStorage()
    {
        std::type_index typeIndex = std::type_index(typeid(T));
        auto it = m_componentStorages.find(typeIndex);

        if (it == m_componentStorages.end())
        {
            auto storage = std::make_unique<ComponentStorage<T>>();
            auto* ptr = storage.get();
            m_componentStorages[typeIndex] = std::move(storage);
            return ptr;
        }

        return static_cast<ComponentStorage<T>*>(it->second.get());
    }

   private:
    EntityID m_nextEntityID = 1;
    std::unordered_map<std::type_index, std::unique_ptr<ComponentStorageBase>> m_componentStorages;
};

// Core Components for MMORPG
struct TransformComponent
{
    Vec3 position{0, 0, 0};
    Vec3 rotation{0, 0, 0};  // Euler angles
    Vec3 scale{1, 1, 1};
};

struct VelocityComponent
{
    Vec3 velocity{0, 0, 0};
    Vec3 acceleration{0, 0, 0};
};

struct VoxelChunkComponent
{
    static constexpr int CHUNK_SIZE = 32;
    uint8_t* voxelData = nullptr;  // 32x32x32 voxel types
    bool needsRemesh = true;
    uint32_t meshVertexCount = 0;
    float* meshVertices = nullptr;  // Generated mesh data
};

// Global ECS instance
extern ECSWorld g_ecs;
