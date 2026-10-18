#pragma once
// Grave — физический движок (Actor). Тела добавляются через Entity::AddBody.
#include "CE/World.h"

#include <functional>

namespace ce {

struct RaycastHit {
    Entity entity;
    Vec3 point;
    Vec3 normal;
    float distance = 0;
};

struct ContactEvent {
    Entity a, b;
    Vec3 point;
    Vec3 normal;     // от a к b
    bool trigger = false;
};

struct GraveApi {
    void SetGravity(Vec3 g) const;
    Vec3 Gravity() const;
    void SetSolverIterations(uint32_t n) const;
    void SetFixedRate(float hz) const;         // частота фиксированного шага, по умолчанию 120 (240 — для очень высоких стопок)
    // Потокобезопасен, можно вызывать из ParallelFor.
    bool Raycast(Vec3 origin, Vec3 direction, float maxDistance, RaycastHit& hit, uint32_t mask = 0xFFFFFFFFu) const;
    // Вызывается в главном потоке для каждого нового контакта/входа в триггер.
    void OnContact(std::function<void(const ContactEvent&)> callback) const;
    uint32_t BodyCount() const;
    uint32_t AwakeBodyCount() const;
};
inline constexpr GraveApi Grave{};

} // namespace ce
