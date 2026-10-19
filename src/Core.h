#pragma once
// Внутреннее ядро: доступ к подсистемам для фасадов публичного API.
#include "CE/Amper.h"
#include "CE/Engine.h"
#include "CE/UniX.h"
#include "CE/World.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>

namespace ce::core {

class Window;
class InputState;
class VulkanContext;
class Gpu;
class GpuLanes;
class Scheduler;
class WorldImpl;
class Physics;
class Renderer;
struct CameraState;

struct EngineCore {
    Window* window = nullptr;
    InputState* input = nullptr;
    VulkanContext* vk = nullptr;
    Gpu* gpu = nullptr;
    GpuLanes* lanes = nullptr;
    Scheduler* scheduler = nullptr;
    WorldImpl* world = nullptr;
    Physics* grave = nullptr;
    Renderer* amper = nullptr;
    CameraState* camera = nullptr;
    EngineConfig config;  // итоговая: после профиля железа и engine.json
    SystemInfo system;
    std::atomic<bool> quit{false};
    // atomic: читаются из фоновых задач, пока главный поток начинает кадр
    std::atomic<double> time{0};
    std::atomic<float> dt{0};
    std::atomic<uint64_t> frame{0};
};

EngineCore* CorePtr();
void SetCore(EngineCore* core);

inline EngineCore& Core() {
    EngineCore* c = CorePtr();
    if (!c)
        throw std::runtime_error("CustomEngine: API engine is used outside of Engine::Run");
    return *c;
}

void Log(const std::string& message);

// Доступ к приватным полям публичных хэндлов.
struct Access {
    static Entity MakeEntity(uint32_t index, uint32_t gen) {
        Entity e;
        e.m_index = index;
        e.m_gen = gen;
        return e;
    }
    static uint32_t Index(const Entity& e) { return e.m_index; }
    static uint32_t Gen(const Entity& e) { return e.m_gen; }

    static Mesh MakeMesh(uint32_t id) {
        Mesh m;
        m.m_id = id;
        return m;
    }
    static Texture MakeTexture(uint32_t id) {
        Texture t;
        t.m_id = id;
        return t;
    }
    static Model MakeModel(uint32_t id) {
        Model m;
        m.m_id = id;
        return m;
    }

    static Light MakeLight(uint32_t index, uint32_t gen) {
        Light l;
        l.m_index = index;
        l.m_gen = gen;
        return l;
    }
    static uint32_t Index(const Light& l) { return l.m_index; }
    static uint32_t Gen(const Light& l) { return l.m_gen; }

    static TaskHandle MakeHandle(std::shared_ptr<TaskRecord> rec) {
        TaskHandle h;
        h.m_rec = std::move(rec);
        return h;
    }
    static TaskRecord* Record(const TaskHandle& h) { return h.m_rec.get(); }
};

} // namespace ce::core
