#include "Grave/Physics.h"
#include "Core.h"
#include "Grave/Collision.h"
#include "World/WorldImpl.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace ce::core {

namespace {

constexpr float kSleepLinear = 0.02f;
constexpr float kSleepAngular = 0.04f;
constexpr float kSleepTime = 0.5f;
constexpr float kBaumgarte = 0.2f;
constexpr float kSlop = 0.01f;
constexpr float kMaxBiasVelocity = 4.0f;
constexpr float kBounceThreshold = 1.0f;
constexpr int kMaxSteps = 8;

uint64_t PairKey(uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); }

void TangentBasis(Vec3 n, Vec3& t1, Vec3& t2) {
    t1 = std::fabs(n.x) >= 0.57735f ? Normalize(Vec3{n.y, -n.x, 0}) : Normalize(Vec3{0, n.z, -n.y});
    t2 = Cross(n, t1);
}

void UpdateInertiaWorld(Body& b) {
    if (!b.Dynamic() || b.lockRot) {
        b.invInertiaWorld = Mat3::Diagonal(Vec3(0.0f));
        return;
    }
    Mat3 R = Mat3::FromQuat(b.rot);
    b.invInertiaWorld = R * Mat3::Diagonal(b.invInertiaLocal) * R.Transposed();
}

inline Vec3 VelocityAt(const Body& b, Vec3 r) { return b.v + Cross(b.w, r); }

inline void ApplyImpulse(Body& b, Vec3 j, Vec3 r) {
    if (!b.Dynamic())
        return;  // статику и кинематику не трогаем: их делят острова, писать в них нельзя
    b.v += j * b.invMass;
    b.w += b.invInertiaWorld * Cross(r, j);
}

inline Vec3 PseudoVelocityAt(const Body& b, Vec3 r) { return b.pv + Cross(b.pw, r); }

inline void ApplyPseudo(Body& b, Vec3 j, Vec3 r) {
    if (!b.Dynamic())
        return;
    b.pv += j * b.invMass;
    b.pw += b.invInertiaWorld * Cross(r, j);
}

inline void ApplyAngular(Body& b, Vec3 L) {
    if (b.Dynamic())
        b.w += b.invInertiaWorld * L;
}

inline float EffectiveMass(const Body& A, const Body& B, Vec3 rA, Vec3 rB, Vec3 dir) {
    float k = A.invMass + B.invMass + Dot(Cross(A.invInertiaWorld * Cross(rA, dir), rA) +
                                          Cross(B.invInertiaWorld * Cross(rB, dir), rB), dir);
    return k > 1e-9f ? 1.0f / k : 0.0f;
}

} // namespace

// ---------------- тела ----------------

void Physics::UpdateMass(Body& b, const BodyDesc& d) {
    if (b.type != BodyType::Dynamic) {
        b.invMass = 0;
        b.invInertiaLocal = Vec3(0.0f);
        UpdateInertiaWorld(b);
        return;
    }
    float m = d.mass > 0 ? d.mass : 1.0f;
    b.invMass = 1.0f / m;
    Vec3 I;
    Vec3 e = d.shape.size;
    switch (d.shape.type) {
    case Shape::Type::Sphere: I = Vec3(0.4f * m * e.x * e.x); break;
    case Shape::Type::Box: I = {m / 3 * (e.y * e.y + e.z * e.z), m / 3 * (e.x * e.x + e.z * e.z), m / 3 * (e.x * e.x + e.y * e.y)}; break;
    case Shape::Type::Capsule: {
        Vec3 h{e.x, e.y + e.x, e.x};  // аппроксимация боксом
        I = {m / 3 * (h.y * h.y + h.z * h.z), m / 3 * (h.x * h.x + h.z * h.z), m / 3 * (h.x * h.x + h.y * h.y)};
        break;
    }
    case Shape::Type::Plane: I = Vec3(1.0f); break;
    }
    b.invInertiaLocal = d.lockRotation ? Vec3(0.0f) : Vec3{1 / I.x, 1 / I.y, 1 / I.z};
    UpdateInertiaWorld(b);
}

uint32_t Physics::AddBody(const BodyDesc& d, Vec3 pos, Quat rot, uint32_t entity, uint32_t gen) {
    std::unique_lock lk(m_lock);
    Body b;
    b.pos = pos;
    b.rot = Normalize(rot);
    b.shape = d.shape;
    b.type = d.shape.type == Shape::Type::Plane ? BodyType::Static : d.type;
    b.friction = d.friction;
    b.restitution = d.restitution;
    b.linDamp = d.linearDamping;
    b.angDamp = d.angularDamping;
    b.trigger = d.trigger;
    b.lockRot = d.lockRotation;
    b.layer = d.layer;
    b.mask = d.mask;
    b.entity = entity;
    b.entityGen = gen;
    UpdateMass(b, d);
    b.aabb = ComputeAabb(b);
    m_bodies.push_back(b);
    m_topologyDirty = true;
    return static_cast<uint32_t>(m_bodies.size() - 1);
}

uint32_t Physics::RemoveBody(uint32_t index) {
    std::unique_lock lk(m_lock);
    if (index >= m_bodies.size())
        return UINT32_MAX;
    uint32_t last = static_cast<uint32_t>(m_bodies.size() - 1);
    uint32_t moved = UINT32_MAX;
    if (index != last) {
        m_bodies[index] = m_bodies[last];
        moved = m_bodies[index].entity;
    }
    m_bodies.pop_back();
    m_topologyDirty = true;
    m_cache.clear();  // индексы тел сдвинулись — старый кэш недействителен
    m_triggerPairs.clear();
    return moved;
}

void Physics::Wake(Body& b) {
    b.sleeping = false;
    b.sleepTimer = 0;
}

#define CE_BODY_OP(Method, Type, Body_)              \
    void Physics::Method(uint32_t i, Type val) {      \
        std::unique_lock lk(m_lock);                  \
        if (i >= m_bodies.size()) return;             \
        Body& b = m_bodies[i];                        \
        Body_;                                        \
        Wake(b);                                      \
    }

CE_BODY_OP(SetPosition, Vec3, { b.pos = val; b.aabb = ComputeAabb(b); })
CE_BODY_OP(SetRotation, Quat, { b.rot = Normalize(val); UpdateInertiaWorld(b); b.aabb = ComputeAabb(b); })
CE_BODY_OP(SetVelocity, Vec3, { b.v = val; })
CE_BODY_OP(SetAngularVelocity, Vec3, { if (!b.lockRot) b.w = val; })
CE_BODY_OP(AddForce, Vec3, { b.force += val; })
CE_BODY_OP(AddImpulse, Vec3, { b.v += val * b.invMass; })
#undef CE_BODY_OP

void Physics::SetGravity(Vec3 g) {
    std::unique_lock lk(m_lock);
    m_gravity = g;
    for (Body& b : m_bodies)
        Wake(b);
}
Vec3 Physics::Gravity() const {
    std::shared_lock lk(m_lock);
    return m_gravity;
}
void Physics::SetIterations(uint32_t n) {
    std::unique_lock lk(m_lock);
    m_iterations = std::clamp(n, 1u, 64u);
}
void Physics::SetFixedRate(float hz) {
    std::unique_lock lk(m_lock);
    m_fixedDt = 1.0f / std::clamp(hz, 15.0f, 1000.0f);
}
uint32_t Physics::BodyCount() const {
    std::shared_lock lk(m_lock);
    return static_cast<uint32_t>(m_bodies.size());
}

void Physics::AddCallback(std::function<void(const ContactEvent&)> cb) {
    std::lock_guard lk(m_cbMutex);
    m_callbacks.push_back(std::move(cb));
}

// ---------------- шаг ----------------

void Physics::Step(Task& task, float frameDt) {
    std::unique_lock lk(m_lock);
    m_events.clear();
    m_accum += std::min(frameDt, 0.1f);
    int steps = 0;
    while (m_accum >= m_fixedDt && steps < kMaxSteps) {
        FixedStep(task, m_fixedDt);
        m_accum -= m_fixedDt;
        ++steps;
    }
    if (steps == kMaxSteps)
        m_accum = 0;  // защита от «спирали смерти»
    if (steps == 0)
        return;

    // Запись результата в мир (в стадии Simulate никто другой мир не пишет)
    WorldImpl& world = *Core().world;
    std::atomic<uint32_t> awake{0};
    task.ParallelFor(static_cast<uint32_t>(m_bodies.size()), [&](uint32_t begin, uint32_t end) {
        uint32_t local = 0;
        for (uint32_t i = begin; i < end; ++i) {
            Body& b = m_bodies[i];
            b.force = Vec3{};
            b.torque = Vec3{};
            if (b.type == BodyType::Static || b.sleeping)
                continue;
            ++local;
            EntityData& d = world.At(b.entity);
            if (d.alive && d.gen == b.entityGen) {
                d.pos = b.pos;
                d.rot = b.rot;
                d.velocity = b.v;
            }
        }
        awake.fetch_add(local, std::memory_order_relaxed);
    });
    m_awake = awake.load();
}

bool Physics::Active(const Body& b) const {
    if (b.type == BodyType::Dynamic)
        return !b.sleeping;
    if (b.type == BodyType::Kinematic)
        return LengthSq(b.v) > 0 || LengthSq(b.w) > 0;
    return false;
}

void Physics::Broadphase() {
    if (m_topologyDirty) {
        m_sapOrder.clear();
        m_planes.clear();
        for (uint32_t i = 0; i < m_bodies.size(); ++i)
            (m_bodies[i].Plane() ? m_planes : m_sapOrder).push_back(i);
        std::sort(m_sapOrder.begin(), m_sapOrder.end(),
                  [&](uint32_t a, uint32_t b) { return m_bodies[a].aabb.min.x < m_bodies[b].aabb.min.x; });
        m_topologyDirty = false;
    } else {
        // Insertion sort: порядок между шагами почти не меняется -> почти O(n)
        for (size_t i = 1; i < m_sapOrder.size(); ++i) {
            uint32_t key = m_sapOrder[i];
            float kx = m_bodies[key].aabb.min.x;
            size_t j = i;
            while (j > 0 && m_bodies[m_sapOrder[j - 1]].aabb.min.x > kx) {
                m_sapOrder[j] = m_sapOrder[j - 1];
                --j;
            }
            m_sapOrder[j] = key;
        }
    }

    auto filter = [&](const Body& A, const Body& B) {
        if (!(Active(A) || Active(B)))
            return false;
        if (!(A.Dynamic() || B.Dynamic() || A.trigger || B.trigger))
            return false;
        return (A.layer & B.mask) && (B.layer & A.mask);
    };

    m_pairs.clear();
    const size_t n = m_sapOrder.size();
    for (size_t i = 0; i < n; ++i) {
        const uint32_t ia = m_sapOrder[i];
        const Body& A = m_bodies[ia];
        for (size_t j = i + 1; j < n; ++j) {
            const uint32_t ib = m_sapOrder[j];
            const Body& B = m_bodies[ib];
            if (B.aabb.min.x > A.aabb.max.x)
                break;
            if (A.aabb.max.y < B.aabb.min.y || B.aabb.max.y < A.aabb.min.y || A.aabb.max.z < B.aabb.min.z ||
                B.aabb.max.z < A.aabb.min.z)
                continue;
            if (filter(A, B))
                m_pairs.emplace_back(std::min(ia, ib), std::max(ia, ib));
        }
    }

    for (uint32_t ip : m_planes) {
        const Body& P = m_bodies[ip];
        Vec3 pn = P.rot.Rotate({0, 1, 0});
        float pd = Dot(pn, P.pos);
        for (uint32_t ib : m_sapOrder) {
            const Body& B = m_bodies[ib];
            if (!filter(P, B))
                continue;
            Vec3 c = (B.aabb.min + B.aabb.max) * 0.5f, h = (B.aabb.max - B.aabb.min) * 0.5f;
            if (Dot(pn, c) - pd - Dot(Abs(pn), h) <= 0)
                m_pairs.emplace_back(std::min(ip, ib), std::max(ip, ib));
        }
    }
}

uint32_t Physics::Find(uint32_t i) {
    while (m_parent[i] != i) {
        m_parent[i] = m_parent[m_parent[i]];
        i = m_parent[i];
    }
    return i;
}

void Physics::BuildIslands() {
    const uint32_t n = static_cast<uint32_t>(m_bodies.size());
    m_parent.resize(n);
    for (uint32_t i = 0; i < n; ++i)
        m_parent[i] = i;
    for (const Manifold& m : m_contacts)
        if (m_bodies[m.a].Dynamic() && m_bodies[m.b].Dynamic()) {
            uint32_t ra = Find(m.a), rb = Find(m.b);
            if (ra != rb) m_parent[ra] = rb;
        }

    // пробуждение: остров, где есть хоть одно бодрствующее тело, просыпается целиком
    m_islandAwake.assign(n, 0);
    for (uint32_t i = 0; i < n; ++i)
        if (m_bodies[i].Dynamic() && !m_bodies[i].sleeping)
            m_islandAwake[Find(i)] = 1;
    for (uint32_t i = 0; i < n; ++i)
        if (m_bodies[i].Dynamic() && m_bodies[i].sleeping && m_islandAwake[Find(i)])
            Wake(m_bodies[i]);

    // группировка контактов по островам (counting sort)
    m_islandOf.assign(n, UINT32_MAX);
    std::vector<uint32_t> counts;
    std::vector<uint32_t> contactIsland(m_contacts.size());
    for (size_t c = 0; c < m_contacts.size(); ++c) {
        const Manifold& m = m_contacts[c];
        uint32_t root = Find(m_bodies[m.a].Dynamic() ? m.a : m.b);
        if (m_islandOf[root] == UINT32_MAX) {
            m_islandOf[root] = static_cast<uint32_t>(counts.size());
            counts.push_back(0);
        }
        contactIsland[c] = m_islandOf[root];
        ++counts[m_islandOf[root]];
    }
    m_islandStart.assign(counts.size() + 1, 0);
    for (size_t i = 0; i < counts.size(); ++i)
        m_islandStart[i + 1] = m_islandStart[i] + counts[i];
    m_islandContacts.resize(m_contacts.size());
    std::vector<uint32_t> cursor(m_islandStart.begin(), m_islandStart.end() - 1);
    for (size_t c = 0; c < m_contacts.size(); ++c)
        m_islandContacts[cursor[contactIsland[c]]++] = static_cast<uint32_t>(c);
}

void Physics::SolveIsland(uint32_t island, float h) {
    const uint32_t begin = m_islandStart[island], end = m_islandStart[island + 1];

    // Подготовка: эффективные массы, bias, warm start
    for (uint32_t k = begin; k < end; ++k) {
        Manifold& m = m_contacts[m_islandContacts[k]];
        Body& A = m_bodies[m.a];
        Body& B = m_bodies[m.b];
        Vec3 center{};
        for (uint32_t i = 0; i < m.count; ++i) {
            ContactPoint& cp = m.pts[i];
            center += cp.p;
            cp.rA = cp.p - A.pos;
            cp.rB = cp.p - B.pos;
            cp.massN = EffectiveMass(A, B, cp.rA, cp.rB, m.n);
            float vn = Dot(VelocityAt(B, cp.rB) - VelocityAt(A, cp.rA), m.n);
            if (cp.depth < 0) {
                cp.bias = cp.depth / h;  // спекулятивный контакт: можно сблизиться на зазор, не больше
                cp.posBias = 0;
            } else {
                cp.bias = vn < -kBounceThreshold ? -m.restitution * vn : 0.0f;
                cp.posBias = std::min(kBaumgarte / h * std::max(cp.depth - kSlop, 0.0f), kMaxBiasVelocity);
            }
            cp.jp = 0;
        }
        center = center / static_cast<float>(m.count);
        m.twistRadius = 0;
        for (uint32_t i = 0; i < m.count; ++i)
            m.twistRadius += Length(m.pts[i].p - center);
        m.twistRadius /= static_cast<float>(m.count);
        m.rA = center - A.pos;
        m.rB = center - B.pos;
        m.massT1 = EffectiveMass(A, B, m.rA, m.rB, m.t1);
        m.massT2 = EffectiveMass(A, B, m.rA, m.rB, m.t2);
        float kt = Dot(m.n, A.invInertiaWorld * m.n) + Dot(m.n, B.invInertiaWorld * m.n);
        m.massTwist = kt > 1e-9f ? 1.0f / kt : 0.0f;

        // warm start
        for (uint32_t i = 0; i < m.count; ++i) {
            ContactPoint& cp = m.pts[i];
            ApplyImpulse(A, -m.n * cp.jn, cp.rA);
            ApplyImpulse(B, m.n * cp.jn, cp.rB);
        }
        Vec3 jt = m.t1 * m.jt1 + m.t2 * m.jt2;
        ApplyImpulse(A, -jt, m.rA);
        ApplyImpulse(B, jt, m.rB);
        ApplyAngular(A, -m.n * m.jtw);
        ApplyAngular(B, m.n * m.jtw);
    }

    for (uint32_t it = 0; it < m_iterations; ++it) {
        for (uint32_t k = begin; k < end; ++k) {
            Manifold& m = m_contacts[m_islandContacts[k]];
            Body& A = m_bodies[m.a];
            Body& B = m_bodies[m.b];

            // Трение в центре пятна: лимит — по суммарному нормальному импульсу
            float sumJn = 0;
            for (uint32_t i = 0; i < m.count; ++i)
                sumJn += m.pts[i].jn;
            float maxF = m.friction * sumJn;

            Vec3 dv = VelocityAt(B, m.rB) - VelocityAt(A, m.rA);
            float l1 = -Dot(dv, m.t1) * m.massT1;
            float n1 = Clamp(m.jt1 + l1, -maxF, maxF);
            l1 = n1 - m.jt1;
            m.jt1 = n1;
            ApplyImpulse(A, -m.t1 * l1, m.rA);
            ApplyImpulse(B, m.t1 * l1, m.rB);

            dv = VelocityAt(B, m.rB) - VelocityAt(A, m.rA);
            float l2 = -Dot(dv, m.t2) * m.massT2;
            float n2 = Clamp(m.jt2 + l2, -maxF, maxF);
            l2 = n2 - m.jt2;
            m.jt2 = n2;
            ApplyImpulse(A, -m.t2 * l2, m.rA);
            ApplyImpulse(B, m.t2 * l2, m.rB);

            // кручение вокруг нормали
            float maxTw = maxF * m.twistRadius;
            float lt = -Dot(B.w - A.w, m.n) * m.massTwist;
            float nt = Clamp(m.jtw + lt, -maxTw, maxTw);
            lt = nt - m.jtw;
            m.jtw = nt;
            ApplyAngular(A, -m.n * lt);
            ApplyAngular(B, m.n * lt);

            // нормаль в каждой точке
            for (uint32_t i = 0; i < m.count; ++i) {
                ContactPoint& cp = m.pts[i];
                dv = VelocityAt(B, cp.rB) - VelocityAt(A, cp.rA);
                float ln = (-Dot(dv, m.n) + cp.bias) * cp.massN;
                float nn = std::max(cp.jn + ln, 0.0f);
                ln = nn - cp.jn;
                cp.jn = nn;
                ApplyImpulse(A, -m.n * ln, cp.rA);
                ApplyImpulse(B, m.n * ln, cp.rB);
            }
        }
    }

    // Split impulse: выталкивание из проникновения на псевдо-скоростях
    const uint32_t splitIters = std::max(4u, m_iterations / 2);
    for (uint32_t it = 0; it < splitIters; ++it) {
        for (uint32_t k = begin; k < end; ++k) {
            Manifold& m = m_contacts[m_islandContacts[k]];
            Body& A = m_bodies[m.a];
            Body& B = m_bodies[m.b];
            for (uint32_t i = 0; i < m.count; ++i) {
                ContactPoint& cp = m.pts[i];
                if (cp.posBias <= 0)
                    continue;
                float dvp = Dot(PseudoVelocityAt(B, cp.rB) - PseudoVelocityAt(A, cp.rA), m.n);
                float lp = (cp.posBias - dvp) * cp.massN;
                float np = std::max(cp.jp + lp, 0.0f);
                lp = np - cp.jp;
                cp.jp = np;
                ApplyPseudo(A, -m.n * lp, cp.rA);
                ApplyPseudo(B, m.n * lp, cp.rB);
            }
        }
    }
}

void Physics::FixedStep(Task& task, float h) {
    const uint32_t n = static_cast<uint32_t>(m_bodies.size());
    if (n == 0)
        return;
    const Vec3 g = m_gravity;

    // 1. Скорости + AABB
    task.ParallelFor(n, [&](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) {
            Body& b = m_bodies[i];
            if (b.Dynamic() && !b.sleeping) {
                b.v += (g + b.force * b.invMass) * h;
                b.w += b.invInertiaWorld * b.torque * h;
                b.v *= 1.0f / (1.0f + h * b.linDamp);
                b.w *= 1.0f / (1.0f + h * b.angDamp);
                if (b.lockRot) b.w = Vec3{};
            }
            if (!b.Plane())
                b.aabb = ComputeAabb(b);
        }
    });

    // 2. Broadphase
    Broadphase();

    // 3. Narrowphase (параллельно, у каждой пары свой слот)
    m_narrow.resize(m_pairs.size());
    task.ParallelFor(static_cast<uint32_t>(m_pairs.size()), [&](uint32_t begin, uint32_t end) {
        for (uint32_t p = begin; p < end; ++p) {
            Manifold& m = m_narrow[p];
            m.count = 0;
            auto [ia, ib] = m_pairs[p];
            const Body& A = m_bodies[ia];
            const Body& B = m_bodies[ib];
            ManifoldResult r;
            if (!Collide(A, B, r))
                continue;
            m.a = ia;
            m.b = ib;
            m.n = r.normal;
            TangentBasis(m.n, m.t1, m.t2);
            m.friction = std::sqrt(A.friction * B.friction);
            m.restitution = std::max(A.restitution, B.restitution);
            m.trigger = A.trigger || B.trigger;
            m.count = r.count;
            m.jt1 = m.jt2 = m.jtw = 0;
            auto cached = m_cache.find(PairKey(ia, ib));
            const bool warm = cached != m_cache.end() && Dot(cached->second.n, m.n) > 0.95f;
            if (warm) {
                m.jt1 = cached->second.jt1 * 0.95f;
                m.jt2 = cached->second.jt2 * 0.95f;
                m.jtw = cached->second.jtw * 0.95f;
            }
            for (uint32_t i = 0; i < r.count; ++i) {
                ContactPoint& cp = m.pts[i];
                cp = ContactPoint{};
                cp.p = r.points[i];
                cp.depth = r.depths[i];
                if (warm) {  // warm starting по ближайшей точке прошлого шага
                    for (uint32_t c = 0; c < cached->second.count; ++c) {
                        const CachedPoint& old = cached->second.pts[c];
                        if (LengthSq(old.p - cp.p) < 0.0016f) {
                            cp.jn = old.jn * 0.95f;
                            break;
                        }
                    }
                }
            }
        }
    });

    // 4. Компактизация, события
    m_contacts.clear();
    m_newTriggerPairs.clear();
    for (const Manifold& m : m_narrow) {
        if (m.count == 0)
            continue;
        uint64_t key = PairKey(m.a, m.b);
        const Body& A = m_bodies[m.a];
        const Body& B = m_bodies[m.b];
        bool fresh;
        if (m.trigger) {
            m_newTriggerPairs.insert(key);
            fresh = !m_triggerPairs.count(key);
        } else {
            m_contacts.push_back(m);
            fresh = !m_cache.count(key);
        }
        if (fresh)
            m_events.push_back({A.entity, A.entityGen, B.entity, B.entityGen, m.pts[0].p, m.n, m.trigger});
    }
    m_triggerPairs.swap(m_newTriggerPairs);

    // 5. Острова и решатель (острова независимы -> параллельно)
    BuildIslands();
    const uint32_t islands = static_cast<uint32_t>(m_islandStart.size()) - 1;
    task.ParallelFor(islands, [&](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i)
            SolveIsland(i, h);
    });

    // 6. Позиции, таймеры сна
    task.ParallelFor(n, [&](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) {
            Body& b = m_bodies[i];
            if (b.type == BodyType::Static || b.sleeping)
                continue;
            b.pos += (b.v + b.pv) * h;
            Vec3 wt = b.w + b.pw;
            b.pv = Vec3{};
            b.pw = Vec3{};
            if (LengthSq(wt) > 0) {
                Quat dq{wt.x, wt.y, wt.z, 0};
                Quat q = dq * b.rot;
                b.rot = Normalize(Quat{b.rot.x + q.x * 0.5f * h, b.rot.y + q.y * 0.5f * h,
                                       b.rot.z + q.z * 0.5f * h, b.rot.w + q.w * 0.5f * h});
                UpdateInertiaWorld(b);
            }
            if (b.Dynamic()) {
                bool still = LengthSq(b.v) < kSleepLinear * kSleepLinear * 25 && LengthSq(b.w) < kSleepAngular * kSleepAngular * 25;
                b.sleepTimer = still ? b.sleepTimer + h : 0.0f;
            }
        }
    });

    // 7. Сон островов: засыпают, только если все тела острова давно неподвижны
    m_islandMinSleep.assign(n, 1e9f);
    for (uint32_t i = 0; i < n; ++i)
        if (m_bodies[i].Dynamic()) {
            uint32_t r = Find(i);
            m_islandMinSleep[r] = std::min(m_islandMinSleep[r], m_bodies[i].sleepTimer);
        }
    for (uint32_t i = 0; i < n; ++i) {
        Body& b = m_bodies[i];
        if (b.Dynamic() && !b.sleeping && m_islandMinSleep[Find(i)] > kSleepTime) {
            b.sleeping = true;
            b.v = Vec3{};
            b.w = Vec3{};
        }
    }

    // 8. Кэш импульсов для warm starting следующего шага
    m_newCache.clear();
    for (const Manifold& m : m_contacts) {
        CachedManifold cm{};
        cm.count = m.count;
        cm.n = m.n;
        cm.jt1 = m.jt1;
        cm.jt2 = m.jt2;
        cm.jtw = m.jtw;
        for (uint32_t i = 0; i < m.count; ++i)
            cm.pts[i] = {m.pts[i].p, m.pts[i].jn};
        m_newCache[PairKey(m.a, m.b)] = cm;
    }
    m_cache.swap(m_newCache);
}

void Physics::DispatchEvents() {
    std::vector<std::function<void(const ContactEvent&)>> cbs;
    {
        std::lock_guard lk(m_cbMutex);
        cbs = m_callbacks;
    }
    if (cbs.empty())
        return;
    for (const PendingEvent& e : m_events) {
        ContactEvent ev;
        ev.a = Access::MakeEntity(e.ea, e.ga);
        ev.b = Access::MakeEntity(e.eb, e.gb);
        ev.point = e.point;
        ev.normal = e.normal;
        ev.trigger = e.trigger;
        for (auto& cb : cbs)
            cb(ev);
    }
}

bool Physics::Raycast(Vec3 origin, Vec3 dir, float maxDist, RaycastHit& hit, uint32_t mask) const {
    std::shared_lock lk(m_lock);
    dir = Normalize(dir);
    if (LengthSq(dir) == 0)
        return false;
    float best = maxDist;
    const Body* bestBody = nullptr;
    Vec3 bestN;
    for (const Body& b : m_bodies) {
        if (b.trigger || !(b.layer & mask))
            continue;
        if (!b.Plane()) {  // быстрый отсев по AABB (slab test)
            float tmin = 0, tmax = best;
            bool miss = false;
            for (int a = 0; a < 3 && !miss; ++a) {
                if (std::fabs(dir[a]) < 1e-8f) {
                    miss = origin[a] < b.aabb.min[a] || origin[a] > b.aabb.max[a];
                    continue;
                }
                float inv = 1.0f / dir[a];
                float t1 = (b.aabb.min[a] - origin[a]) * inv, t2 = (b.aabb.max[a] - origin[a]) * inv;
                if (t1 > t2) std::swap(t1, t2);
                tmin = std::max(tmin, t1);
                tmax = std::min(tmax, t2);
                miss = tmin > tmax;
            }
            if (miss)
                continue;
        }
        float t;
        Vec3 nrm;
        if (RaycastBody(b, origin, dir, best, t, nrm) && t <= best) {
            best = t;
            bestBody = &b;
            bestN = nrm;
        }
    }
    if (!bestBody)
        return false;
    hit.entity = Access::MakeEntity(bestBody->entity, bestBody->entityGen);
    hit.distance = best;
    hit.point = origin + dir * best;
    hit.normal = bestN;
    return true;
}

} // namespace ce::core
