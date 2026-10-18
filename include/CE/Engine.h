#pragma once
#include <cstdint>
#include <string>

namespace ce {

struct EngineConfig {
    std::string title = "CustomEngine";
    uint32_t width = 1280;
    uint32_t height = 720;
    bool vsync = true;
    bool rayTracing = false;   // стартовое значение флага Amper RT
    float targetFps = 60.0f;   // бюджет кадра для UniX
    uint32_t workerThreads = 0; // 0 = авто (ядра - 2)
};

class Game {
public:
    virtual ~Game() = default;
    virtual void OnStart() {}     // создать сцену и задачи
    virtual void OnShutdown() {}
};

struct Engine {
    static int Run(Game& game, const EngineConfig& config = {});
    template <class G>
    static int Run(const EngineConfig& config = {}) {
        G game;
        return Run(game, config);
    }
    static void Quit();
    static double Time();
    static float DeltaTime();
    static uint64_t Frame();
};

} // namespace ce
