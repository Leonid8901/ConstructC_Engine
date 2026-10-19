// Пример игры на CustomEngine: ни Vulkan, ни glm, ни GLFW здесь не видно.
// Запуск: Sandbox [модель.glb|.gltf] — модель встанет на пол перед стеной.
//
// Управление: ПКМ + мышь — обзор, WASD/QE — полёт (Shift — быстрее),
//             ЛКМ/Space — выстрел шаром, R — ray tracing вкл/выкл,
//             T — карты теней вкл/выкл (работают при выключенном RT), M — сглаживание (MSAA 1/2/4/8),
//             Esc — выход.
#include <CustomEngine.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static std::string g_modelPath;  // Sandbox путь/к/модели.glb — показать свою модель

class Demo : public Game {
public:
    void OnStart() override {
        Amper.SetSun({-0.45f, -1.0f, -0.35f}, {1.0f, 0.95f, 0.85f}, 3.2f);
        Amper.SetAmbient({0.45f, 0.55f, 0.7f}, 0.35f);
        Camera.SetPosition({0, 5, 16});
        Camera.LookAt({0, 2, 0});

        // Область мира: всё, что вылетело за пол (80x80) или провалилось, — удаляется (шары),
        // а объекты сцены с флагом Respawn возвращаются на свои места.
        World.SetBounds({-45, -20, -45}, {45, 100, 45});
        World.SetOutOfBoundsAction(OutOfBounds::Destroy);

        // Пол: текстура-шахматка, собранная в памяти (Texture::Create), повтор каждые 2 м
        std::vector<uint8_t> checker(64 * 64 * 4);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const uint8_t v = ((x / 32) + (y / 32)) % 2 ? 120 : 90;
                uint8_t* p = &checker[(y * 64 + x) * 4];
                p[0] = v; p[1] = static_cast<uint8_t>(v + 4); p[2] = static_cast<uint8_t>(v + 8); p[3] = 255;
            }
        Material floor;
        floor.color = {1, 1, 1};
        floor.roughness = 0.85f;
        floor.baseColorMap = Texture::Create(checker.data(), 64, 64);
        floor.uvScale = {0.5f, 0.5f};
        World.Spawn()
            .SetMesh(Mesh::Plane(80))
            .SetMaterial(floor)
            .AddBody({BodyType::Static, Shape::Plane()});

        // Своя модель из командной строки: ставим её на пол перед стеной
        if (!g_modelPath.empty()) {
            try {
                Model m = Model::Load(g_modelPath);
                const Vec3 lo = m.BoundsMin(), hi = m.BoundsMax();
                const float size = std::max(hi.x - lo.x, std::max(hi.y - lo.y, hi.z - lo.z));
                const float s = size > 0 ? 2.5f / size : 1.0f;  // вписываем в 2,5 м
                World.Spawn().SetModel(m).SetScale(s).SetPosition({0, -lo.y * s, 4});
                std::printf("[game] модель %s: частей %u\n", g_modelPath.c_str(), m.PartCount());
            } catch (const std::exception& e) {
                std::printf("[game] не удалось загрузить модель: %s\n", e.what());
            }
        }

        // Стена из кубов
        const Mesh cube = Mesh::Cube();
        for (int y = 0; y < 8; ++y)
            for (int x = -2; x <= 2; ++x)
                World.Spawn()
                    .SetMesh(cube)
                    .SetPosition({x * 1.02f, 0.5f + y * 1.0f, 0})
                    .SetColor({0.85f - 0.07f * y, 0.35f + 0.08f * (x + 2), 0.25f + 0.06f * y})
                    .AddBody({BodyType::Dynamic, Shape::Box({0.5f, 0.5f, 0.5f}), 1.0f, 0.6f})
                    .SetOutOfBounds(OutOfBounds::Respawn);

        // Металлический шар и капсулы
        World.Spawn()
            .SetMesh(Mesh::Sphere())
            .SetScale(2.0f)
            .SetPosition({-6, 1, 3})
            .SetMaterial({{0.95f, 0.8f, 0.4f}, 0.25f, 1.0f})
            .AddBody({BodyType::Dynamic, Shape::Sphere(1.0f), 5.0f})
            .SetOutOfBounds(OutOfBounds::Respawn);
        const Mesh capsule = Mesh::Capsule(0.35f, 0.5f);
        for (int i = 0; i < 4; ++i)
            World.Spawn()
                .SetMesh(capsule)
                .SetPosition({5.0f + i * 0.9f, 1.0f + i * 1.5f, 2.0f})
                .SetRotation(Quat::AxisAngle({0, 0, 1}, 0.4f * i))
                .SetColor({0.3f, 0.7f, 0.9f})
                .AddBody({BodyType::Dynamic, Shape::Capsule(0.35f, 0.5f), 1.0f})
                .SetOutOfBounds(OutOfBounds::Respawn);

        // Триггер-зона: сообщает, когда в неё что-то влетает
        m_zone = World.Spawn()
                     .SetPosition({0, 1, -6})
                     .AddBody({BodyType::Static, Shape::Box({3, 1, 1}), 0, 0.5f, 0, 0, 0, true});
        Grave.OnContact([this](const ContactEvent& e) {
            if (e.trigger && (e.a == m_zone || e.b == m_zone))
                std::printf("[game] что-то влетело в триггер-зону\n");
        });

        // Светильник, облетающий сцену (с RT-тенями)
        m_lamp = Amper.AddPointLight({0, 4, 4}, {1.0f, 0.55f, 0.25f}, 40.0f, 14.0f, true);
        m_lampBall = World.Spawn()
                         .SetMesh(Mesh::Sphere(16))
                         .SetScale(0.3f)
                         .SetMaterial({{1, 0.6f, 0.3f}, 0.5f, 0, {4.0f, 2.2f, 1.0f}});

        // ---------------- Задачи UniX ----------------

        UniX.CreateTask.NewTask("CameraFly", Cost::Low, Priority::High, [](Task& t) {
            bool& locked = t.State<bool>();
            bool look = Input.MouseDown(MouseButton::Right);
            if (look != locked) {
                Input.LockCursor(look);
                locked = look;
            }
            if (look) {
                Vec2 d = Input.MouseDelta();
                Camera.SetAngles(Camera.Yaw() + d.x * 0.0025f, Camera.Pitch() - d.y * 0.0025f);
            }
            float speed = (Input.Down(Key::LeftShift) ? 20.0f : 7.0f) * t.DeltaTime();
            Vec3 p = Camera.Position(), f = Camera.Forward(), r = Camera.Right();
            if (Input.Down(Key::W)) p += f * speed;
            if (Input.Down(Key::S)) p -= f * speed;
            if (Input.Down(Key::D)) p += r * speed;
            if (Input.Down(Key::A)) p -= r * speed;
            if (Input.Down(Key::E)) p.y += speed;
            if (Input.Down(Key::Q)) p.y -= speed;
            Camera.SetPosition(p);
        });

        const Mesh ball = Mesh::Sphere();
        UniX.CreateTask.NewTask("Shooter", Cost::Low, Priority::Normal, [ball](Task&) {
            if (!Input.MousePressed(MouseButton::Left) && !Input.Pressed(Key::Space))
                return;
            Vec3 f = Camera.Forward();
            World.Spawn()
                .SetMesh(ball)
                .SetScale(0.6f)
                .SetPosition(Camera.Position() + f * 1.5f)
                .SetMaterial({{0.9f, 0.9f, 0.95f}, 0.15f, 1.0f})
                .AddBody({BodyType::Dynamic, Shape::Sphere(0.3f), 3.0f, 0.4f, 0.3f})
                .SetVelocity(f * 30.0f);
        });

        UniX.CreateTask.NewTask("LampOrbit", Cost::Low, Priority::Low, [this](Task& t) {
            float a = static_cast<float>(t.Time()) * 0.8f;
            Vec3 p{std::cos(a) * 6.0f, 3.5f, std::sin(a) * 6.0f};
            m_lamp.SetPosition(p);
            m_lampBall.SetPosition(p);
        });

        // Тяжёлая задача: «радар» — 8192 луча по сфере вокруг камеры каждый кадр.
        // ParallelFor режется UniX по эффективной стоимости задачи.
        m_radar = UniX.CreateTask.NewTask("RayTraceTask", Cost::High, Priority::Normal, [](Task& t) {
            constexpr uint32_t kRays = 8192;
            const Vec3 origin = Camera.Position();
            std::atomic<uint32_t> hits{0};
            t.ParallelFor(kRays, [&](uint32_t begin, uint32_t end) {
                uint32_t local = 0;
                for (uint32_t i = begin; i < end; ++i) {
                    float y = 1.0f - 2.0f * (i + 0.5f) / kRays;  // точки Фибоначчи на сфере
                    float r = std::sqrt(1.0f - y * y);
                    float phi = i * 2.39996323f;
                    RaycastHit hit;
                    if (Grave.Raycast(origin, {r * std::cos(phi), y, r * std::sin(phi)}, 60.0f, hit))
                        ++local;
                }
                hits.fetch_add(local, std::memory_order_relaxed);
            });
            t.State<uint32_t>() = hits.load();
        });

        UniX.CreateTask.NewTask("Controls", Cost::Low, Priority::High, [](Task&) {
            if (Input.Pressed(Key::R)) {
                Amper.SetRayTracing(!Amper.RayTracing());
                std::printf("[game] ray tracing: %s\n", Amper.RayTracing() ? "on" : (Amper.RayTracingSupported() ? "off" : "не поддерживается GPU"));
            }
            if (Input.Pressed(Key::T)) {
                Amper.SetShadows(!Amper.Shadows());
                std::printf("[game] карты теней: %s\n", Amper.Shadows() ? "on" : "off");
            }
            if (Input.Pressed(Key::M)) {  // 1 -> 2 -> 4 -> 8 -> 1 (в пределах возможностей GPU)
                uint32_t next = Amper.Antialiasing() * 2;
                while (next <= Amper.MaxAntialiasing() && !Amper.AntialiasingSupported(next))
                    next *= 2;  // бывают пропуски: например, только 1x и 4x
                if (next > Amper.MaxAntialiasing())
                    next = 1;
                Amper.SetAntialiasing(next);
                std::printf("[game] MSAA: %ux\n", next);
            }
            if (Input.Pressed(Key::Escape))
                Engine::Quit();
        });

        // Статистика планировщика раз в 2 с
        UniX.CreateTask.NewTask("Stats", Cost::Low, Priority::Low, [this](Task& t) {
            double& next = t.State<double>();
            if (t.Time() < next)
                return;
            next = t.Time() + 2.0;
            auto lanes = UniX.Lanes();
            auto s = m_radar.GetStats();
            static const char* kCost[] = {"Low", "Medium", "High", "Extreme"};
            std::printf("[UniX] CPU %.2f/%.1f ms | lanes core=%u general=%u bg=%u | RayTraceTask %.2f ms, util %.2f, eff %s | bodies %u awake %u\n",
                        UniX.FrameWorkMs(), UniX.TargetFrameMs(), lanes.core, lanes.general, lanes.background, s.avgMs,
                        s.utility, kCost[static_cast<int>(s.effective)], Grave.BodyCount(), Grave.AwakeBodyCount());
        });

        // Одноразовая Extreme-задача уходит в фоновый лейн и не тормозит кадры
        UniX.CreateTask.Once("BuildColumns", Cost::Extreme, Priority::Low, [](Task& t) {
            const Mesh column = Mesh::Capsule(0.6f, 2.0f, 32);
            for (int i = 0; i < 10 && !t.Cancelled(); ++i) {
                float a = i * (2.0f * kPi / 10.0f);
                World.Spawn()
                    .SetMesh(column)
                    .SetPosition({std::cos(a) * 14.0f, 2.6f, std::sin(a) * 14.0f})
                    .SetMaterial({{0.75f, 0.72f, 0.68f}, 0.7f})
                    .AddBody({BodyType::Static, Shape::Capsule(0.6f, 2.0f)});
            }
            std::printf("[game] колонны построены в фоне\n");
        });
    }

private:
    Entity m_zone;
    Entity m_lampBall;
    Light m_lamp;
    TaskHandle m_radar;
};

int main(int argc, char** argv) {
    if (argc > 1)
        g_modelPath = argv[1];
    EngineConfig config;
    config.title = "CustomEngine Sandbox";
    config.rayTracing = true;  // если GPU не умеет — останется растеризация
    return Engine::Run<Demo>(config);
}
