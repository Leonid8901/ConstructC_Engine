#pragma once
// CPU-сторона UniX.
//
// Кадр = последовательность стадий (Update -> Simulate -> Prepare). Внутри стадии все задачи
// выполняются параллельно, между стадиями — барьер и точка синхронизации мира.
//
// Типы потоков (лейны):
//   Main       — главный поток: Sys-задачи (окно, ввод), отправка кадра в GPU; помогает воркерам.
//   Core       — воркеры, которые сначала берут Sys/Engine/Actor работу, потом пользовательскую.
//   General    — воркеры, которые сначала берут пользовательские задачи (Task), потом остальное.
//   Background — фоновый поток для одноразовых Extreme-задач, растянутых на несколько кадров.
// Соотношение Core/General UniX пересчитывает по фактической нагрузке.
//
// Задача «перекидывается» только между вызовами — это безопасная точка: её состояние живёт
// в TaskRecord, а не в потоке, поэтому следующий тик может уйти в другой лейн.
#include "CE/UniX.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ce::core {

enum class Stage : uint8_t { Update, Simulate, Prepare, Count };
enum class LaneType : uint8_t { Core, General, Background };

// Линейный аллокатор памяти задачи на один вызов
class Arena {
public:
    void Reset();
    void* Alloc(size_t bytes, size_t align);

private:
    std::unique_ptr<std::byte[]> m_data;
    size_t m_capacity = 0;
    size_t m_offset = 0;
    size_t m_requested = 0;
    std::vector<std::unique_ptr<std::byte[]>> m_overflow;
};

struct TaskRecord {
    std::string name;
    Cost declared = Cost::Low;
    Priority priority = Priority::Normal;
    Significance sig = Significance::Task;
    Stage stage = Stage::Update;
    TaskFn fn;
    bool once = false;
    bool async = false;
    bool pinMain = false;

    std::atomic<bool> alive{true};
    std::atomic<bool> paused{false};

    // Разгрузка
    float dt = 0;
    float pendingDt = 0;
    uint32_t throttle = 1;
    uint32_t frameCounter = 0;
    uint64_t lastRunFrame = UINT64_MAX;

    // Измерения и коэффициент полезности
    std::atomic<int64_t> chunkNs{0};
    int64_t waitNs = 0;
    int64_t lastCpuNs = 0;
    float emaMs = -1;
    float loadRatio = 1;
    float utility = 1;
    Cost effective = Cost::Low;
    Cost candidate = Cost::Low;
    uint32_t candidateFrames = 0;
    Priority effectivePriority = Priority::Normal;

    Arena arena;

    void* state = nullptr;
    size_t stateHash = 0;
    size_t stateAlign = 0;
    void (*stateDtor)(void*) = nullptr;

    ~TaskRecord();
};

class Scheduler {
public:
    Scheduler(uint32_t workerThreads, float targetFps);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    static uint32_t ResolveWorkerCount(uint32_t requested);
    uint32_t ThreadSlots() const { return m_workerCount + 2; }  // main + воркеры + фон
    static int ThreadSlot();

    std::shared_ptr<TaskRecord> Create(std::string name, Cost cost, Priority priority, Significance sig, Stage stage,
                                       TaskFn fn, bool once, bool pinMain = false);

    void BeginFrame(double time, float dt, uint64_t frame);
    void RunStage(Stage stage);
    void EndFrame();
    void NoteWait(int64_t ns) { m_waitNs.fetch_add(ns, std::memory_order_relaxed); }
    void AddMainWork(int64_t ns) { m_stageNs += ns; }  // работа главного потока вне стадий (Render)

    void ParallelFor(TaskRecord* rec, uint32_t count, void (*fn)(void*, uint32_t, uint32_t), void* ctx);

    bool InStage() const { return m_inStage.load(std::memory_order_acquire); }
    bool ShuttingDown() const { return m_stop.load(std::memory_order_acquire); }
    double Time() const { return m_time; }
    uint64_t Frame() const { return m_frame; }
    float TargetFrameMs() const { return m_targetMs.load(); }
    void SetTargetFps(float fps) { m_targetMs.store(1000.0f / (fps > 1 ? fps : 1)); }
    float FrameWorkMs() const { return m_lastWorkMs.load(); }
    uint32_t WorkerCount() const { return m_workerCount; }
    UniXApi::LaneInfo Lanes() const;

private:
    struct Job {
        void (*run)(Job&) = nullptr;
        void* data = nullptr;
        uint32_t begin = 0, end = 0;
        std::atomic<int32_t>* counter = nullptr;
        TaskRecord* rec = nullptr;
    };
    struct JobQueue {
        std::mutex mutex;
        std::deque<Job> jobs;
    };

    struct AsyncCtx {
        Scheduler* self;
        std::shared_ptr<TaskRecord> rec;
    };
    static void RunTaskJob(Job& job);
    static void RunAsyncJob(Job& job);
    static void RunChunkJob(Job& job);

    void ExecuteTask(TaskRecord& rec);
    void WorkerLoop(uint32_t slot);
    void BackgroundLoop(uint32_t slot);
    bool TryRunOne(LaneType lane, int maxBucket = 3);
    void RunJob(Job& job, int bucket);
    void Push(int bucket, const Job& job, bool front);
    void Notify();
    void WaitAndHelp(std::atomic<int32_t>& counter, int maxBucket);
    LaneType CurrentLane() const;

    void UpdateStats(TaskRecord& rec);
    void Rebalance();
    void AdjustThrottling();
    float Budget(Cost c) const;

    uint32_t m_workerCount;
    std::vector<std::thread> m_threads;
    std::unique_ptr<std::atomic<LaneType>[]> m_workerLane;
    uint32_t m_coreWorkers = 1;

    std::array<JobQueue, 4> m_buckets;   // по значимости: Sys, Engine, Actor, Task
    JobQueue m_background;
    std::atomic<int32_t> m_pendingJobs{0};
    std::mutex m_sleepMutex;
    std::condition_variable m_sleepCv;
    std::mutex m_bgMutex;
    std::condition_variable m_bgCv;
    std::atomic<bool> m_stop{false};

    std::mutex m_createMutex;
    std::vector<std::shared_ptr<TaskRecord>> m_pending;
    std::array<std::vector<std::shared_ptr<TaskRecord>>, static_cast<size_t>(Stage::Count)> m_tasks;
    std::vector<TaskRecord*> m_due;

    std::atomic<bool> m_inStage{false};
    double m_time = 0;
    float m_dt = 0;
    uint64_t m_frame = 0;

    std::atomic<float> m_targetMs{16.6667f};
    std::atomic<int64_t> m_waitNs{0};
    int64_t m_stageNs = 0;
    std::atomic<float> m_lastWorkMs{0};
    float m_workEma = 0;
    uint32_t m_overFrames = 0, m_underFrames = 0;

    std::array<std::atomic<int64_t>, 4> m_bucketNs{};
    uint32_t m_rebalanceCounter = 0;
};

} // namespace ce::core
