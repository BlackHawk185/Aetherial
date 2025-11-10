// ECS.cpp - Implementation of Entity Component System
#include "ECS.h"

ECSWorld g_ecs;

EntityID ECSWorld::createEntity() {
    return m_nextEntityID++;
}

EntityID ECSWorld::createEntityWithID(EntityID id) {
    // Update next ID if necessary
    if (id >= m_nextEntityID) {
        m_nextEntityID = id + 1;
    }
    return id;
}

void ECSWorld::destroyEntity(EntityID entity) {
    // Remove from all component storages
    for (auto& [typeIndex, storage] : m_componentStorages) {
        storage->removeEntity(entity);
    }
}
