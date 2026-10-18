#pragma once
// Хранилище сущностей.
// Страницы по 4096 сущностей никогда не перемещаются в памяти, поэтому чтение из любого потока
// безопасно даже во время роста мира. Все изменения внутри стадий UniX откладываются в
// per-thread буферы команд и применяются в точке синхронизации (главный поток).
#include "CE/World.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace ce::core {

struct EntityData {
    Vec3 pos;
    Quat rot;
    Vec3 scale{1, 1, 1};
    Vec3 velocity;
    Material material;
    uint32_t mesh = UINT32_MAX;
    uint32_t body = UINT32_MAX;
    uint32_t gen = 0;
    bool alive = false;
    bool visible = true;
};

enum class CmdType : uint8_t {
    Create, Destroy, SetPos, SetRot, SetScale, SetMesh, SetMaterial, SetColor, SetVisible,
    AddBody, RemoveBody, SetVel, SetAngVel, AddForce, AddImpulse
};

struct Command {
    CmdType type;
    uint32_t index;
    uint32_t gen;
    Vec3 v;
    Quat q;
    uint32_t u = 0;
    Material material;
    BodyDesc body;
};

struct CameraState {
    std::mutex mutex;
    Vec3 pos{0, 2, 8};
    float yaw = 0;
    float pitch = 0;
    float fovDeg = 60;
};

class WorldImpl {
public:
    static constexpr uint32_t kPageBits = 12;
    static constexpr uint32_t kPageSize = 1u << kPageBits;
    static constexpr uint32_t kMaxPages = 1024;  // до ~4M сущностей

    explicit WorldImpl(uint32_t threadSlots);
    ~WorldImpl();

    Entity Spawn();
    void Submit(const Command& cmd);  // применить сразу или отложить
    void Flush();                     // главный поток, вне стадий

    EntityData& At(uint32_t index) { return m_pages[index >> kPageBits].load(std::memory_order_acquire)[index & (kPageSize - 1)]; }
    const EntityData* TryGet(uint32_t index, uint32_t gen) const;
    uint32_t Capacity() const { return m_published.load(std::memory_order_acquire); }
    uint32_t AliveCount() const { return m_alive.load(std::memory_order_relaxed); }

private:
    struct CommandBuffer {
        std::mutex mutex;
        std::vector<Command> cmds;
    };

    bool Deferred() const;
    void Apply(const Command& cmd);

    std::array<std::atomic<EntityData*>, kMaxPages> m_pages{};
    std::atomic<uint32_t> m_published{0};
    std::atomic<uint32_t> m_alive{0};
    std::mutex m_allocMutex;
    std::vector<uint32_t> m_free;
    uint32_t m_next = 0;
    std::vector<std::unique_ptr<CommandBuffer>> m_buffers;
    std::vector<Command> m_scratch;
};

} // namespace ce::core
