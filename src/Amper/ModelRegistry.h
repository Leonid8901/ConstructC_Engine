#pragma once
// Реестр моделей glTF 2.0. Модель = набор частей (меш + материал + матрица узла в координатах модели).
// Меши и текстуры загружаются в общие реестры (MeshRegistry, TextureRegistry); модель хранит их индексы.
// Слоты фиксированы: рендер читает готовую модель без блокировок (указатель публикуется атомарно).
#include "CE/World.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ce::core {

class MeshRegistry;
class TextureRegistry;

struct ModelPart {
    uint32_t mesh = UINT32_MAX;
    Material material;
    Mat4 local;           // матрица узла относительно корня модели
    uint32_t node = 0;
};

struct ModelData {
    std::string path;
    std::vector<ModelPart> parts;
    Vec3 boundsMin{}, boundsMax{};
};

class ModelRegistry {
public:
    static constexpr uint32_t kMaxModels = 4096;

    ModelRegistry(MeshRegistry& meshes, TextureRegistry& textures);
    ~ModelRegistry();

    uint32_t Load(const std::string& path);  // повторный вызов с тем же путём — та же модель
    const ModelData* Get(uint32_t id) const {
        return id < kMaxModels ? m_models[id].load(std::memory_order_acquire) : nullptr;
    }

private:
    MeshRegistry& m_meshes;
    TextureRegistry& m_textures;
    std::unique_ptr<std::atomic<ModelData*>[]> m_models;
    std::atomic<uint32_t> m_count{0};
    std::mutex m_cacheMutex;
    std::unordered_map<std::string, uint32_t> m_cache;
};

} // namespace ce::core
