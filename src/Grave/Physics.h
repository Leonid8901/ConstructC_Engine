#pragma once
// Grave — rigid body физика.
// Фиксированный шаг (120 Гц) -> интеграция скоростей -> broadphase (sweep-and-prune) -> narrowphase
// (параллельно, спекулятивные контакты) -> острова (union-find) -> sequential impulses с warm starting,
// трение в центре пятна + кручение, split impulse для проникновений (острова параллельно)
// -> интеграция позиций -> сон островов.
#include "CE/Grave.h"
#include "CE/UniX.h"
#include "Grave/Body.h"

#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ce::core {

struct ContactPoint {
    Vec3 p;
    float depth = 0;
    Vec3 rA, rB;
    float massN = 0;
    float bias = 0;      // скоростной bias: отскок / спекулятивный зазор
    float posBias = 0;   // скорость выталкивания из проникновения (split impulse)
    float jn = 0;
    float jp = 0;
};

// Трение решается в центре пятна контакта (2 тангенты + кручение вокруг нормали), а не в каждой точке:
// у 4 копланарных точек потоковое трение вырождено, и поточечный решатель раскачивает стопки.
struct Manifold {
    uint32_t a = 0, b = 0;
    Vec3 n, t1, t2;
    float friction = 0.5f, restitution = 0;
    uint32_t count = 0;
    bool trigger = false;
    ContactPoint pts[4];
    Vec3 rA, rB;                 // плечи центра пятна
    float massT1 = 0, massT2 = 0, massTwist = 0, twistRadius = 0;
    float jt1 = 0, jt2 = 0, jtw = 0;
};

class Physics {
public:
    Physics() = default;

    uint32_t AddBody(const BodyDesc& desc, Vec3 pos, Quat rot, uint32_t entity, uint32_t gen);
    uint32_t RemoveBody(uint32_t index);  // возвращает сущность тела, переехавшего в этот слот

    void SetPosition(uint32_t b, Vec3 p);
    void SetRotation(uint32_t b, Quat q);
    void SetVelocity(uint32_t b, Vec3 v);
    void SetAngularVelocity(uint32_t b, Vec3 w);
    void AddForce(uint32_t b, Vec3 f);
    void AddImpulse(uint32_t b, Vec3 j);

    void Step(Task& task, float frameDt);
    void DispatchEvents();  // главный поток

    bool Raycast(Vec3 origin, Vec3 dir, float maxDist, RaycastHit& hit, uint32_t mask) const;
    // Перестроить дерево для лучей, если тела менялись (главный поток, вне стадий). Без него Raycast
    // остаётся корректным, но перебирает все тела.
    void RefreshQueries();
    void AddCallback(std::function<void(const ContactEvent&)> cb);

    void SetGravity(Vec3 g);
    Vec3 Gravity() const;
    void SetIterations(uint32_t n);
    void SetFixedRate(float hz);
    uint32_t BodyCount() const;
    uint32_t AwakeCount() const { return m_awake; }

private:
    struct CachedPoint {
        Vec3 p;
        float jn;
    };
    struct CachedManifold {
        uint32_t count;
        CachedPoint pts[4];
        Vec3 n;
        float jt1, jt2, jtw;
    };
    struct PendingEvent {
        uint32_t ea, ga, eb, gb;
        Vec3 point, normal;
        bool trigger;
    };

    void FixedStep(Task& task, float h);
    void Broadphase();
    void BuildIslands();
    void SolveIsland(uint32_t island, float h);
    void UpdateMass(Body& b, const BodyDesc& desc);
    void Wake(Body& b);
    bool Active(const Body& b) const;
    uint32_t Find(uint32_t i);
    void BuildRayTree();
    void BuildRayNode(uint32_t node, uint32_t begin, uint32_t end);
    bool RaycastLinear(Vec3 origin, Vec3 dir, float maxDist, uint32_t mask, float& best, const Body*& bestBody,
                       Vec3& bestN) const;
    bool RaycastTree(Vec3 origin, Vec3 dir, float maxDist, uint32_t mask, float& best, const Body*& bestBody,
                     Vec3& bestN) const;

    std::vector<Body> m_bodies;
    std::vector<uint32_t> m_sapOrder;
    std::vector<uint32_t> m_planes;
    bool m_topologyDirty = true;

    std::vector<std::pair<uint32_t, uint32_t>> m_pairs;
    std::vector<Manifold> m_narrow;
    std::vector<Manifold> m_contacts;
    std::unordered_map<uint64_t, CachedManifold> m_cache, m_newCache;
    std::unordered_set<uint64_t> m_triggerPairs, m_newTriggerPairs;

    std::vector<uint32_t> m_parent;
    std::vector<uint32_t> m_islandOf;        // корень -> индекс острова
    std::vector<uint32_t> m_islandStart;     // начало списка контактов острова
    std::vector<uint32_t> m_islandContacts;  // индексы в m_contacts, сгруппированные по островам
    std::vector<uint8_t> m_islandAwake;
    std::vector<float> m_islandMinSleep;

    // BVH по AABB тел для Raycast: строится раз в кадр после физики (RefreshQueries).
    // Узел-лист: count > 0, first — начало в m_rayItems; внутренний: count == 0, дети first и first + 1.
    struct RayNode {
        Aabb box;
        uint32_t first = 0;
        uint32_t count = 0;
    };
    std::vector<RayNode> m_rayNodes;
    std::vector<uint32_t> m_rayItems;   // индексы тел
    std::vector<uint32_t> m_rayPlanes;  // плоскости бесконечны — проверяются отдельно
    bool m_rayValid = false;

    std::vector<PendingEvent> m_events;
    std::mutex m_cbMutex;
    std::vector<std::function<void(const ContactEvent&)>> m_callbacks;

    mutable std::shared_mutex m_lock;
    Vec3 m_gravity{0, -9.81f, 0};
    uint32_t m_iterations = 10;
    float m_fixedDt = 1.0f / 120.0f;  // 120 Гц: высокие стопки стоят и засыпают (60 Гц — дешевле, но дребезжат)
    float m_accum = 0;
    uint32_t m_awake = 0;
};

} // namespace ce::core
