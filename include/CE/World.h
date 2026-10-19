#pragma once
// Сцена: сущности, меши, материалы, камера.
// Все записи из задач потокобезопасны: внутри стадий они откладываются и применяются
// в точке синхронизации кадра. Чтение отдаёт состояние на начало стадии.
#include "CE/Math.h"
#include "CE/Shapes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ce {

namespace core { struct Access; }

struct Vertex {
    Vec3 position;
    Vec3 normal;
};

struct Material {
    Vec3 color{0.8f, 0.8f, 0.8f};
    float roughness = 0.5f;
    float metallic = 0.0f;
    Vec3 emissive{};
};

// Примитивы единичного размера: куб 1x1x1, сфера диаметром 1, плоскость size x size.
class Mesh {
public:
    Mesh() = default;
    static Mesh Cube();
    static Mesh Sphere(uint32_t segments = 32);
    static Mesh Plane(float size = 1.0f);
    static Mesh Capsule(float radius = 0.5f, float halfHeight = 0.5f, uint32_t segments = 24);
    static Mesh Create(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
    static Mesh LoadOBJ(const std::string& path);

    bool Valid() const { return m_id != UINT32_MAX; }
    uint32_t Id() const { return m_id; }

private:
    friend struct core::Access;
    uint32_t m_id = UINT32_MAX;
};

// Что делать с сущностью, вылетевшей за область мира (см. WorldApi::SetBounds / SetKillY).
enum class OutOfBounds : uint8_t {
    Default,  // только для сущности: действие по умолчанию из World.SetOutOfBoundsAction (для движущихся тел)
    None,     // не трогать
    Destroy,  // удалить сущность
    Respawn,  // вернуть в точку возврата: положение и поворот, нулевые скорости
};

class Entity {
public:
    Entity() = default;

    bool Valid() const;
    void Destroy();

    Vec3 Position() const;
    Quat Rotation() const;
    Vec3 Scale() const;
    Vec3 Forward() const;
    Entity& SetPosition(Vec3 p);
    Entity& SetRotation(Quat q);
    Entity& SetScale(Vec3 s);
    Entity& SetScale(float s) { return SetScale(Vec3(s)); }

    Entity& SetMesh(Mesh mesh);
    Entity& SetMaterial(const Material& m);
    Entity& SetColor(Vec3 c);
    Entity& SetVisible(bool v);

    // Поведение за границами мира. Без вызова сущность с динамическим/кинематическим телом получает действие
    // по умолчанию из World.SetOutOfBoundsAction, остальные сущности не трогаются.
    // Respawn без явной точки возвращает в положение, где сущность была при AddBody (или при включении Respawn).
    Entity& SetOutOfBounds(OutOfBounds action);
    Entity& SetRespawnPoint(Vec3 position, Quat rotation = Quat{});

    Entity& AddBody(const BodyDesc& desc);
    Entity& RemoveBody();
    bool HasBody() const;
    Vec3 Velocity() const;
    Entity& SetVelocity(Vec3 v);
    Entity& SetAngularVelocity(Vec3 w);
    Entity& AddForce(Vec3 f);
    Entity& AddImpulse(Vec3 j);

    uint32_t Index() const { return m_index; }
    bool operator==(const Entity& o) const { return m_index == o.m_index && m_gen == o.m_gen; }
    bool operator!=(const Entity& o) const { return !(*this == o); }

private:
    friend struct core::Access;
    uint32_t m_index = UINT32_MAX;
    uint32_t m_gen = 0;
};

struct WorldApi {
    Entity Spawn() const;
    uint32_t Count() const;

    // Область мира. Сущность, центр которой вышел за неё, получает действие OutOfBounds
    // (проверка раз в кадр после физики). По умолчанию область выключена.
    void SetBounds(Vec3 min, Vec3 max) const;
    // Только нижняя граница по Y («KillZ» из Unreal; у нас Y вверх). Остальные границы не меняются.
    void SetKillY(float y) const;
    void ClearBounds() const;
    // Действие по умолчанию для сущностей с движущимися телами; по умолчанию Destroy. Допустимы None/Destroy/Respawn.
    void SetOutOfBoundsAction(OutOfBounds action) const;
};
inline constexpr WorldApi World{};

struct CameraApi {
    void SetPosition(Vec3 p) const;
    void LookAt(Vec3 target) const;
    void SetAngles(float yaw, float pitch) const;  // радианы; yaw=0 смотрит в -Z
    void SetFov(float degrees) const;
    Vec3 Position() const;
    Vec3 Forward() const;
    Vec3 Right() const;
    float Yaw() const;
    float Pitch() const;
};
inline constexpr CameraApi Camera{};

} // namespace ce
