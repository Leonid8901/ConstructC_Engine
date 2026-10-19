# Справочник API

Все типы находятся в пространстве имён `ce`; `CustomEngine.h` делает `using namespace ce`.
Подсистемы с собственными документами: [UniX](UniX.md), [Grave](Grave.md), [Amper](Amper.md). Здесь — остальное.

## Содержание

- [Engine, Game, EngineConfig](#engine-game-engineconfig)
- [World](#world)
- [Entity](#entity)
- [Mesh](#mesh)
- [Material и Vertex](#material-и-vertex)
- [Camera](#camera)
- [Input](#input)
- [Математика](#математика)

## Engine, Game, EngineConfig

```cpp
struct EngineConfig {
    std::string title = "CustomEngine";
    uint32_t width = 1280, height = 720;
    bool     vsync = true;
    bool     rayTracing = false;    // начальное значение флага RT
    float    targetFps = 60.0f;     // бюджет кадра для UniX
    uint32_t workerThreads = 0;     // 0 — авто: ядра − 2 (минимум 1)
    uint32_t msaa = 4;              // сглаживание: 1 (выкл), 2, 4, 8; урезается до возможностей GPU
    uint32_t shadowMapSize = 0;     // 0 — по профилю железа
    std::string configFile = "engine.json";  // рядом с exe; "" — без файла
};

class Game {
public:
    virtual ~Game() = default;
    virtual void OnStart() {}       // создать сцену и задачи
    virtual void OnShutdown() {}
};
```

| Метод `Engine` | Описание |
|---|---|
| `static int Run(Game&, const EngineConfig& = {})` | запустить цикл; возвращает 0 при штатном завершении, 1 при исключении (сообщение пишется в лог) |
| `template <class G> static int Run(const EngineConfig& = {})` | создаёт `G` и запускает |
| `static void Quit()` | запросить завершение |
| `static double Time()` | время с запуска, с |
| `static float DeltaTime()` | длительность прошлого кадра, с |
| `static uint64_t Frame()` | номер кадра |
| `static const SystemInfo& System()` | что движок узнал о железе и какой профиль выбрал |

Значения `EngineConfig` — пожелания игры: профиль железа и `engine.json` игрока могут их изменить ([Platforms.md](Platforms.md#3-профили)). Например, `rayTracing = true` на встроенной графике останется выключенным, если игрок не включит его в `engine.json`.

```cpp
struct SystemInfo {
    std::string os, windowPlatform;            // "Linux", "Wayland"
    std::string cpuVendor, cpuName;
    uint32_t cpuCores, cpuThreads; bool cpuHybrid;
    std::string gpuVendor, gpuName, gpuType, gpuDriver;  // gpuType: discrete / integrated / software / virtual
    uint64_t gpuMemoryMB;
    std::string profile;                       // "AMD discrete"
    bool configLoaded;                         // engine.json прочитан
};
```

`OnStart` вызывается один раз до первого кадра на главном потоке. Команды мира в нём применяются сразу.

## World

```cpp
World.Spawn()    // Entity: создать сущность
World.Count()    // uint32_t: число живых сущностей
```

### Область мира (KillZ)

| Метод `World` | Описание |
|---|---|
| `SetBounds(Vec3 min, Vec3 max)` | включить область мира: параллелепипед |
| `SetKillY(float y)` | включить только нижнюю границу по Y (аналог KillZ в Unreal; у нас Y вверх) |
| `ClearBounds()` | выключить область |
| `SetOutOfBoundsAction(OutOfBounds)` | действие по умолчанию: `Destroy` (по умолчанию), `Respawn` или `None` |

Раз в кадр, после физики (на главном потоке), проверяется позиция (центр) каждой сущности. Действие для сущности:

| Флаг сущности (`Entity::SetOutOfBounds`) | Что происходит за границей |
|---|---|
| `Default` (не задан) | для динамических и кинематических тел — действие по умолчанию; сущности без тела и статические тела не трогаются |
| `None` | ничего |
| `Destroy` | сущность удаляется (`Entity::Valid()` → `false`) |
| `Respawn` | позиция и поворот — из точки возврата, линейная и угловая скорости обнуляются, тело будится |

Точка возврата по умолчанию — положение и поворот сущности в момент `AddBody` (или в момент `SetOutOfBounds(Respawn)`, если тела нет). Задать явно: `Entity::SetRespawnPoint(Vec3 position, Quat rotation = {})`; явную точку `AddBody` не перезаписывает.

```cpp
World.SetBounds({-45, -20, -45}, {45, 100, 45});      // шары, улетевшие за пол, удаляются
World.Spawn().SetMesh(Mesh::Cube()).SetPosition({0, 0.5f, 0})
    .AddBody({BodyType::Dynamic, Shape::Box({0.5f, 0.5f, 0.5f})})
    .SetOutOfBounds(OutOfBounds::Respawn);              // а этот куб вернётся на место
```

`Spawn` возвращает дескриптор немедленно. Из задач сама сущность появляется при ближайшем Flush ([Architecture.md](Architecture.md#4-мир-и-отложенные-команды)).

## Entity

Лёгкий дескриптор (индекс + поколение). Сравнивается `==` / `!=`. Методы-сеттеры возвращают `Entity&` для цепочек.

### Состояние

| Метод | Описание |
|---|---|
| `bool Valid() const` | сущность существует и поколение совпадает |
| `void Destroy()` | уничтожить (вместе с телом) |
| `uint32_t Index() const` | индекс слота |

### Трансформация

| Метод | Описание |
|---|---|
| `Vec3 Position() const` / `SetPosition(Vec3)` | позиция |
| `Quat Rotation() const` / `SetRotation(Quat)` | ориентация |
| `Vec3 Scale() const` / `SetScale(Vec3)` / `SetScale(float)` | масштаб (на коллайдер не влияет) |
| `Vec3 Forward() const` | `Rotation()`, применённая к `(0, 0, −1)` |

### Отображение

| Метод | Описание |
|---|---|
| `SetMesh(Mesh)` | меш |
| `SetMaterial(const Material&)` | материал целиком |
| `SetColor(Vec3)` | только цвет материала |
| `SetVisible(bool)` | показ/скрытие |
| `SetOutOfBounds(OutOfBounds)` | поведение за областью мира (см. [World](#область-мира-killz)) |
| `SetRespawnPoint(Vec3, Quat = {})` | точка возврата для `Respawn` |

### Физика

`AddBody`, `RemoveBody`, `HasBody`, `Velocity`, `SetVelocity`, `SetAngularVelocity`, `AddForce`, `AddImpulse` — см. [Grave.md](Grave.md#33-методы-entity-относящиеся-к-физике).

Сеттеры во время стадии откладываются; геттеры отдают состояние на начало стадии.

## Mesh

```cpp
class Mesh {
public:
    static Mesh Cube();
    static Mesh Sphere(uint32_t segments = 32);
    static Mesh Plane(float size = 1.0f);
    static Mesh Capsule(float radius = 0.5f, float halfHeight = 0.5f, uint32_t segments = 24);
    static Mesh Create(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
    static Mesh LoadOBJ(const std::string& path);
    bool Valid() const;
    uint32_t Id() const;
};
```

| Фабрика | Геометрия |
|---|---|
| `Cube()` | куб 1×1×1 |
| `Sphere(segments)` | сфера диаметром 1 |
| `Plane(size)` | плоскость `size × size` в XZ, нормаль +Y |
| `Capsule(radius, halfHeight, segments)` | капсула вдоль Y; `halfHeight` — полувысота цилиндрической части |
| `Create(vertices, indices)` | пользовательская геометрия |
| `LoadOBJ(path)` | загрузка OBJ; бросает `std::runtime_error`, если файл не загружен |

Поддерживаемое подмножество OBJ: `v`, `vn`, `f`; многоугольники разбиваются веером; без нормалей в файле используются плоские нормали. Текстурные координаты, материалы и группы игнорируются.

Примитивы кэшируются по параметрам: повторный вызов с теми же аргументами возвращает тот же меш. `Create` и `LoadOBJ` кэширования не делают. Геометрия загружается на GPU синхронно через очередь transfer; если устройство поддерживает RT, для меша сразу строится BLAS.

## Material и Vertex

```cpp
struct Vertex   { Vec3 position; Vec3 normal; };
struct Material { Vec3 color{0.8,0.8,0.8}; float roughness = 0.5f; float metallic = 0.0f; Vec3 emissive{}; };
```

Подробности модели — [Amper.md](Amper.md#2-модель-освещения).

## Camera

Глобальный объект `Camera`. В отличие от `Entity`, операции выполняются немедленно под мьютексом и безопасны из любых потоков.

| Метод | Описание |
|---|---|
| `SetPosition(Vec3)` | позиция |
| `LookAt(Vec3 target)` | направить на точку (из текущей позиции) |
| `SetAngles(float yaw, float pitch)` | углы, рад; `yaw = 0` смотрит в −Z; `pitch` ограничивается ±1,55 рад |
| `SetFov(float degrees)` | вертикальный угол обзора; диапазон 10…150, по умолчанию 60 |
| `Position()`, `Forward()`, `Right()`, `Yaw()`, `Pitch()` | чтение |

Ближняя плоскость — 0,05 м, дальняя — бесконечность (reverse-Z).

Направления: `Forward = (cos p · sin y, sin p, −cos p · cos y)`, `Right = (cos y, 0, sin y)`.

## Input

Снимок ввода делается один раз за кадр. Нажатия «защёлкиваются» и не теряются между кадрами.

| Метод | Описание |
|---|---|
| `bool Down(Key)` | клавиша удерживается |
| `bool Pressed(Key)` | нажата в этом кадре |
| `bool Released(Key)` | отпущена в этом кадре |
| `bool MouseDown(MouseButton)` | кнопка удерживается |
| `bool MousePressed(MouseButton)` | кнопка нажата в этом кадре |
| `Vec2 MousePosition()` | позиция курсора в окне, px |
| `Vec2 MouseDelta()` | сдвиг мыши за кадр |
| `float Scroll()` | прокрутка за кадр |
| `void LockCursor(bool)` | захват и скрытие курсора |

`MouseButton`: `Left`, `Right`, `Middle`.

`Key` (значения совпадают с кодами GLFW): `Space`, `Apostrophe`, `Comma`, `Minus`, `Period`, `Slash`, `Num0`…`Num9`, `A`…`Z`, `Escape`, `Enter`, `Tab`, `Backspace`, `Insert`, `Delete`, `Right`, `Left`, `Down`, `Up`, `F1`…`F12`, `LeftShift`, `LeftControl`, `LeftAlt`, `LeftSuper`, `RightShift`, `RightControl`, `RightAlt`.

## Математика

Собственные типы без зависимости от glm. Правая система координат, Y вверх.

| Тип | Описание |
|---|---|
| `Vec2` | `x, y`; `+ - * float` |
| `Vec3` | `x, y, z`; `+ - * /` (покомпонентно и на скаляр), унарный `-`, `+= -= *=`, `[i]`; `explicit Vec3(float)` |
| `Vec4` | `x, y, z, w`; конструкторы `(x,y,z,w)` и `(Vec3, w)` |
| `Quat` | `x, y, z, w`; `AxisAngle(axis, rad)`, `Euler(pitch, yaw, roll)`, `Rotate(Vec3)`, `Conjugate()`, `operator*` |
| `Mat3` | `m[row][col]`; `Diagonal`, `FromQuat`, `operator*`, `Transposed`, `Column` |
| `Mat4` | column-major `m[col*4+row]`; `Identity`, `TRS`, `PerspectiveReverseZ`, `Ortho`, `LookAt`, `operator*`, `TransformPoint` |

Свободные функции:

| Функция | Описание |
|---|---|
| `Dot(a, b)`, `Cross(a, b)` | скалярное и векторное произведение |
| `Length(v)`, `LengthSq(v)`, `Normalize(v)` | длина, квадрат длины, нормализация (нулевой вектор → нулевой) |
| `Min`, `Max`, `Abs`, `Clamp`, `Lerp` | для `Vec3`; `Clamp`, `Lerp` — и для `float` |
| `Normalize(Quat)` | нормализация кватерниона |
| `Radians(deg)`, `Degrees(rad)`, `kPi` | углы |

`Quat::Euler` применяет повороты в порядке roll (Z) → pitch (X) → yaw (Y).
