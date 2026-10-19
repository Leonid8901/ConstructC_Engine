# CustomEngine

Экспериментальный 3D-движок на Vulkan 1.3 / C++20. Состоит из трёх подсистем:

| Подсистема | Роль | Документ |
|---|---|---|
| **UniX** | параллельный планировщик задач с адаптивной классификацией нагрузки | [docs/UniX.md](docs/UniX.md) |
| **Grave** | физика твёрдых тел (3D) | [docs/Grave.md](docs/Grave.md) |
| **Amper** | рендер и освещение: PBR-растеризация, тени через ray query или каскадные карты теней | [docs/Amper.md](docs/Amper.md) |

Игровой код подключает один заголовок `CustomEngine.h`. Vulkan, GLFW и glm в публичный API не попадают: у движка собственные `Vec2/3/4`, `Quat`, `Mat3`, `Mat4`.

Статус: рабочий прототип (alpha). Назначение — учебный референс архитектуры, а не готовый продакшн-движок. Ограничения перечислены в разделе [Ограничения](#ограничения).

## Возможности

- Любое пользовательское действие — задача UniX с заявленными `Cost`, `Priority`, `Significance`.
- Планировщик сравнивает заявку с измеренным CPU-временем и корректирует классификацию, приоритет и частоту выполнения.
- Тела: сфера, бокс, капсула, плоскость. Острова, сон, триггеры, слои и маски, raycast, события контактов.
- Рендер: PBR (GGX), инстансинг, reverse-Z, ACES, до 256 точечных источников.
- Тени от солнца: ray query (флаг) либо 4 каскада по 2048² с PCF 3×3.
- Изменения мира из задач потокобезопасны без блокировок на стороне пользователя (отложенные команды).

## Быстрый старт

```cpp
#include <CustomEngine.h>

class MyGame : public Game {
public:
    void OnStart() override {
        Camera.SetPosition({0, 5, 14});
        Camera.LookAt({0, 1, 0});

        World.Spawn()
            .SetMesh(Mesh::Plane(80))
            .AddBody({BodyType::Static, Shape::Plane()});

        World.Spawn()
            .SetMesh(Mesh::Cube())
            .SetPosition({0, 4, 0})
            .SetColor({0.9f, 0.4f, 0.2f})
            .AddBody({BodyType::Dynamic, Shape::Box({0.5f, 0.5f, 0.5f}), 1.0f});

        UniX.CreateTask.NewTask("Quit", Cost::Low, Priority::High, [](Task&) {
            if (Input.Pressed(Key::Escape))
                Engine::Quit();
        });
    }
};

int main() {
    EngineConfig cfg;
    cfg.rayTracing = true;  // если GPU не поддерживает — остаётся растеризация
    return Engine::Run<MyGame>(cfg);
}
```

Полный пример — [`sandbox/main.cpp`](sandbox/main.cpp).

## Сборка

Требования:

- CMake ≥ 3.20, компилятор с C++20
- Vulkan SDK (заголовки, loader, `glslc`); GPU и драйвер с Vulkan 1.3
- GLFW 3.4 и Vulkan Memory Allocator 3.3.0 подтягиваются через `FetchContent`

```
cmake -S . -B build
cmake --build build --config Release
```

Windows:
```
build\Release\Sandbox.exe
```
Linux:
```
./build/Sandbox
```

Шейдеры компилируются `glslc` при сборке. Движок ищет SPIR-V сначала в `shaders/` рядом с исполняемым файлом, затем в каталоге сборки (`SHADER_DIR`). Validation layers включаются в Debug автоматически, если установлен SDK.

Ray tracing опционален: нужны расширения `VK_KHR_acceleration_structure` и `VK_KHR_ray_query`. При их отсутствии `Amper.RayTracingSupported()` возвращает `false`, `SetRayTracing(true)` ничего не включает.

### Управление в Sandbox

| Действие | Ввод |
|---|---|
| Обзор | ПКМ + мышь |
| Полёт | W A S D / Q E, Shift — быстрее |
| Выстрел шаром | ЛКМ или Space |
| Ray tracing вкл/выкл | R |
| Каскадные тени вкл/выкл (при выключенном RT) | T |
| Выход | Esc |

## Структура репозитория

```
include/                публичный API (единственное, что видит игра)
  CustomEngine.h        единый заголовок
  CE/Math.h             Vec2/3/4, Quat, Mat3, Mat4
  CE/UniX.h             Task, TaskHandle, UniX
  CE/World.h            Entity, Mesh, Material, World, Camera
  CE/Shapes.h           Shape, BodyDesc, BodyType
  CE/Grave.h            Grave
  CE/Amper.h            Amper, Light
  CE/Input.h            Input, Key, MouseButton
  CE/Engine.h           EngineConfig, Game, Engine
src/
  Engine.cpp            главный цикл кадра
  UniX/                 Scheduler (CPU), GpuLanes (очереди GPU)
  World/                хранилище сущностей, буферы отложенных команд
  Grave/                коллизии, решатель, острова
  Amper/                рендерер, реестр мешей, BLAS/TLAS, каскады теней
  Vulkan/               контекст, swapchain, обёртка VMA
  Platform/             окно (GLFW), ввод
shaders/                GLSL: mesh, mesh_rt, sky, shadow
sandbox/                демонстрационная игра
docs/                   документация по подсистемам
```

## Документация

- [docs/Architecture.md](docs/Architecture.md) — кадр, модель потоков, мир и отложенные команды, соглашения
- [docs/UniX.md](docs/UniX.md) — модель задачи, API, алгоритмы адаптации, константы
- [docs/Grave.md](docs/Grave.md) — формы, шаг симуляции, решатель, параметры
- [docs/Amper.md](docs/Amper.md) — конвейер, модель освещения, тени
- [docs/API.md](docs/API.md) — справочник по `World`, `Entity`, `Mesh`, `Camera`, `Input`, `Engine`, математике

## Проверка

Сборка и запуск выполнялись на Linux (GCC) с программной реализацией Vulkan и включёнными validation layers: ноль ошибок и предупреждений в сценариях «RT вкл», «RT выкл + каскады», изменение размера окна, ввод, стрельба, события триггера. Не проверено: Windows/MSVC, аппаратные GPU, производительность на реальной нагрузке. Сравнения планировщика с другими реализациями (Taskflow, TBB, enkiTS) не проводилось.

## Ограничения

- **UniX.** Очереди — общие `deque` на каждый уровень значимости под мьютексом; work stealing нет. Зависимости между задачами выражены стадиями с барьерами (Update → Simulate → Prepare), графа зависимостей нет. Распределение работы по GPU-очередям минимально: compute-очередь используется для сборки BLAS, transfer — для загрузки мешей.
- **Grave.** Нет CCD, шарниров, меш-коллайдеров. `Raycast` перебирает тела линейно. Высокие плотные стены (≈ 10×5 боксов) не всегда засыпают; для очень высоких стопок помогает `SetFixedRate(240)`.
- **Amper.** Нет текстур и glTF (OBJ грузится). Тени точечных источников — только с RT. Освещение без кластеризации: фрагмент перебирает все источники. Каскады рисуют все инстансы без отсечения. Буфер инстансов — в host-visible памяти.

## Лицензия

MIT-License
