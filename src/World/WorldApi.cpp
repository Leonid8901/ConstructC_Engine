// Публичный фасад сцены: Entity, World, Camera
#include "Core.h"
#include "Grave/Physics.h"
#include "World/WorldImpl.h"

#include <cmath>

namespace ce {

using core::CmdType;
using core::Command;
using core::Core;

namespace {

core::WorldImpl& W() { return *Core().world; }

Command Cmd(CmdType t, const Entity& e) {
    Command c{};
    c.type = t;
    c.index = core::Access::Index(e);
    c.gen = core::Access::Gen(e);
    return c;
}

} // namespace

// ---------------- Entity ----------------

bool Entity::Valid() const { return W().TryGet(m_index, m_gen) != nullptr; }
void Entity::Destroy() { W().Submit(Cmd(CmdType::Destroy, *this)); }

Vec3 Entity::Position() const {
    auto* d = W().TryGet(m_index, m_gen);
    return d ? d->pos : Vec3{};
}
Quat Entity::Rotation() const {
    auto* d = W().TryGet(m_index, m_gen);
    return d ? d->rot : Quat{};
}
Vec3 Entity::Scale() const {
    auto* d = W().TryGet(m_index, m_gen);
    return d ? d->scale : Vec3{1, 1, 1};
}
Vec3 Entity::Forward() const { return Rotation().Rotate({0, 0, -1}); }
Vec3 Entity::Velocity() const {
    auto* d = W().TryGet(m_index, m_gen);
    return d ? d->velocity : Vec3{};
}
bool Entity::HasBody() const {
    auto* d = W().TryGet(m_index, m_gen);
    return d && d->body != UINT32_MAX;
}

#define CE_ENTITY_SET(Method, Type, Arg, CmdT, Field) \
    Entity& Entity::Method(Type Arg) {                \
        Command c = Cmd(CmdT, *this);                 \
        c.Field = Arg;                                \
        W().Submit(c);                                \
        return *this;                                 \
    }

CE_ENTITY_SET(SetPosition, Vec3, p, CmdType::SetPos, v)
CE_ENTITY_SET(SetRotation, Quat, q, CmdType::SetRot, q)
CE_ENTITY_SET(SetScale, Vec3, s, CmdType::SetScale, v)
CE_ENTITY_SET(SetColor, Vec3, col, CmdType::SetColor, v)
CE_ENTITY_SET(SetVelocity, Vec3, vel, CmdType::SetVel, v)
CE_ENTITY_SET(SetAngularVelocity, Vec3, w, CmdType::SetAngVel, v)
CE_ENTITY_SET(AddForce, Vec3, f, CmdType::AddForce, v)
CE_ENTITY_SET(AddImpulse, Vec3, j, CmdType::AddImpulse, v)
#undef CE_ENTITY_SET

Entity& Entity::SetMesh(Mesh mesh) {
    Command c = Cmd(CmdType::SetMesh, *this);
    c.u = mesh.Id();
    W().Submit(c);
    return *this;
}

Entity& Entity::SetMaterial(const Material& m) {
    Command c = Cmd(CmdType::SetMaterial, *this);
    c.material = m;
    W().Submit(c);
    return *this;
}

Entity& Entity::SetVisible(bool v) {
    Command c = Cmd(CmdType::SetVisible, *this);
    c.u = v ? 1 : 0;
    W().Submit(c);
    return *this;
}

Entity& Entity::AddBody(const BodyDesc& desc) {
    Command c = Cmd(CmdType::AddBody, *this);
    c.body = desc;
    W().Submit(c);
    return *this;
}

Entity& Entity::RemoveBody() {
    W().Submit(Cmd(CmdType::RemoveBody, *this));
    return *this;
}

// ---------------- World ----------------

Entity WorldApi::Spawn() const { return W().Spawn(); }
uint32_t WorldApi::Count() const { return W().AliveCount(); }

// ---------------- Camera ----------------

namespace {
core::CameraState& Cam() { return *Core().camera; }
Vec3 ForwardFrom(float yaw, float pitch) {
    return {std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw)};
}
} // namespace

void CameraApi::SetPosition(Vec3 p) const {
    std::lock_guard lk(Cam().mutex);
    Cam().pos = p;
}

void CameraApi::LookAt(Vec3 target) const {
    std::lock_guard lk(Cam().mutex);
    Vec3 d = Normalize(target - Cam().pos);
    Cam().pitch = std::asin(Clamp(d.y, -1.0f, 1.0f));
    Cam().yaw = std::atan2(d.x, -d.z);
}

void CameraApi::SetAngles(float yaw, float pitch) const {
    std::lock_guard lk(Cam().mutex);
    Cam().yaw = yaw;
    Cam().pitch = Clamp(pitch, -1.55f, 1.55f);
}

void CameraApi::SetFov(float degrees) const {
    std::lock_guard lk(Cam().mutex);
    Cam().fovDeg = Clamp(degrees, 10.0f, 150.0f);
}

Vec3 CameraApi::Position() const {
    std::lock_guard lk(Cam().mutex);
    return Cam().pos;
}

Vec3 CameraApi::Forward() const {
    std::lock_guard lk(Cam().mutex);
    return ForwardFrom(Cam().yaw, Cam().pitch);
}

Vec3 CameraApi::Right() const {
    std::lock_guard lk(Cam().mutex);
    return {std::cos(Cam().yaw), 0, std::sin(Cam().yaw)};
}

float CameraApi::Yaw() const {
    std::lock_guard lk(Cam().mutex);
    return Cam().yaw;
}

float CameraApi::Pitch() const {
    std::lock_guard lk(Cam().mutex);
    return Cam().pitch;
}

} // namespace ce
