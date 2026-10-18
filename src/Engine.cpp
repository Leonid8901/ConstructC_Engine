// Точка входа движка и главный цикл кадра.
//
//  Sys     (главный поток)   события окна, снимок ввода
//  Update  (UniX, параллельно) пользовательские задачи                 -> Flush мира
//  Simulate(UniX)             Grave.Step (Actor), внутри ParallelFor    -> события контактов -> Flush
//  Prepare (UniX)             Amper.Prepare (Actor), внутри ParallelFor
//  Render  (главный поток)   запись и отправка кадра в GPU (Engine)
//  EndFrame                  UniX: статистика, коэффициенты полезности, разгрузка, ребаланс лейнов
#include "Amper/Renderer.h"
#include "CE/Engine.h"
#include "CE/Input.h"
#include "Core.h"
#include "Grave/Physics.h"
#include "Platform/Input.h"
#include "Platform/Window.h"
#include "UniX/GpuLanes.h"
#include "UniX/Scheduler.h"
#include "Vulkan/Gpu.h"
#include "Vulkan/VulkanContext.h"
#include "World/WorldImpl.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // только для UTF-8 в консоли
#endif

namespace ce {

namespace core {

namespace {
std::atomic<EngineCore*> g_core{nullptr};
std::mutex g_logMutex;
} // namespace

EngineCore* CorePtr() { return g_core.load(std::memory_order_acquire); }
void SetCore(EngineCore* c) { g_core.store(c, std::memory_order_release); }

void Log(const std::string& message) {
    std::lock_guard lk(g_logMutex);
    std::cout << "[CE] " << message << std::endl;
}

} // namespace core

int Engine::Run(Game& game, const EngineConfig& cfg) {
    using namespace core;
    if (CorePtr()) {
        Log("Engine::Run: движок уже запущен");
        return 1;
    }
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try {
        EngineCore ec;
        ec.config = cfg;

        InputState input;
        Window window(cfg.width, cfg.height, cfg.title, &input);
        VulkanContext vk(window);
        Gpu gpu(vk);
        GpuLanes lanes(vk);
        const uint32_t workers = Scheduler::ResolveWorkerCount(cfg.workerThreads);
        WorldImpl world(workers + 2);
        Physics grave;
        CameraState camera;
        Renderer amper(vk, gpu, lanes, window, cfg);

        ec.input = &input;
        ec.window = &window;
        ec.vk = &vk;
        ec.gpu = &gpu;
        ec.lanes = &lanes;
        ec.world = &world;
        ec.grave = &grave;
        ec.camera = &camera;
        ec.amper = &amper;
        SetCore(&ec);
        struct CoreReset {
            ~CoreReset() { SetCore(nullptr); }
        } coreReset;

        // Планировщик создаётся последним и уничтожается первым: потоки останавливаются
        // до того, как исчезнут мир, физика и рендер, с которыми работают задачи.
        Scheduler scheduler(workers, cfg.targetFps);
        ec.scheduler = &scheduler;
        Log(std::string("UniX GPU lanes: ") + lanes.Description());
        Log(std::string("Amper ray tracing: ") + (amper.RtSupported() ? (amper.RayTracing() ? "on" : "off (flag)") : "not supported"));

        // Внутренние задачи уровня Actor — пользователь такие создать не может
        scheduler.Create("Grave.Step", Cost::High, Priority::High, Significance::Actor, Stage::Simulate,
                    [&grave](Task& t) { grave.Step(t, t.DeltaTime()); }, false);
        scheduler.Create("Amper.Prepare", Cost::Medium, Priority::High, Significance::Actor, Stage::Prepare,
                    [&amper](Task& t) { amper.Prepare(t); }, false);

        game.OnStart();
        world.Flush();

        using clock = std::chrono::steady_clock;
        const auto start = clock::now();
        auto prev = start, titleTimer = start;
        uint32_t frames = 0;

        while (!window.ShouldClose() && !ec.quit.load()) {
            window.PollEvents();
            input.NewFrame(window);

            int w = 0, h = 0;
            window.FramebufferSize(w, h);
            if (w == 0 || h == 0) {  // свёрнуто
                window.WaitEvents();
                prev = clock::now();
                continue;
            }

            const auto now = clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - prev).count(), 0.25f);
            prev = now;
            ec.time = std::chrono::duration<double>(now - start).count();
            ec.dt = dt;
            ++ec.frame;

            scheduler.BeginFrame(ec.time, dt, ec.frame);
            scheduler.RunStage(Stage::Update);
            world.Flush();
            scheduler.RunStage(Stage::Simulate);
            grave.DispatchEvents();
            world.Flush();
            scheduler.RunStage(Stage::Prepare);

            const auto r0 = clock::now();
            amper.Render();
            scheduler.AddMainWork(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - r0).count());
            scheduler.EndFrame();

            ++frames;
            const float elapsed = std::chrono::duration<float>(now - titleTimer).count();
            if (elapsed >= 0.5f) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s | %.0f FPS | CPU %.2f ms | RT %s | bodies %u (awake %u) | draws %u",
                              cfg.title.c_str(), frames / elapsed, scheduler.FrameWorkMs(),
                              amper.RayTracing() ? "on" : "off", grave.BodyCount(), grave.AwakeCount(),
                              amper.DrawCalls());
                window.SetTitle(buf);
                frames = 0;
                titleTimer = now;
            }
        }

        vkDeviceWaitIdle(vk.Device());
        game.OnShutdown();
        return 0;
    } catch (const std::exception& e) {
        Log(std::string("Fatal: ") + e.what());
        return 1;
    }
}

void Engine::Quit() {
    if (auto* c = core::CorePtr())
        c->quit.store(true);
}
double Engine::Time() { return core::Core().time; }
float Engine::DeltaTime() { return core::Core().dt; }
uint64_t Engine::Frame() { return core::Core().frame; }

// ---------------- Input ----------------

namespace {
core::InputState& In() { return *core::Core().input; }
} // namespace

bool InputApi::Down(Key k) const { return In().Down(static_cast<int>(k)); }
bool InputApi::Pressed(Key k) const { return In().Pressed(static_cast<int>(k)); }
bool InputApi::Released(Key k) const { return In().Released(static_cast<int>(k)); }
bool InputApi::MouseDown(MouseButton b) const { return In().MouseDown(static_cast<int>(b)); }
bool InputApi::MousePressed(MouseButton b) const { return In().MousePressed(static_cast<int>(b)); }
Vec2 InputApi::MousePosition() const { return In().MousePosition(); }
Vec2 InputApi::MouseDelta() const { return In().MouseDelta(); }
float InputApi::Scroll() const { return In().Scroll(); }
void InputApi::LockCursor(bool locked) const { In().RequestCursorLock(locked); }

} // namespace ce
