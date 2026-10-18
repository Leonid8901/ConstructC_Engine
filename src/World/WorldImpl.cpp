#include "World/WorldImpl.h"
#include "Core.h"
#include "Grave/Physics.h"
#include "UniX/Scheduler.h"

#include <stdexcept>

namespace ce::core {

WorldImpl::WorldImpl(uint32_t threadSlots) {
    for (uint32_t i = 0; i < threadSlots; ++i)
        m_buffers.push_back(std::make_unique<CommandBuffer>());
}

WorldImpl::~WorldImpl() {
    for (auto& p : m_pages)
        delete[] p.load();
}

bool WorldImpl::Deferred() const {
    EngineCore* c = CorePtr();
    return Scheduler::ThreadSlot() != 0 || (c && c->scheduler && c->scheduler->InStage());
}

const EntityData* WorldImpl::TryGet(uint32_t index, uint32_t gen) const {
    if (index >= Capacity())
        return nullptr;
    const EntityData* page = m_pages[index >> kPageBits].load(std::memory_order_acquire);
    if (!page)
        return nullptr;
    const EntityData& d = page[index & (kPageSize - 1)];
    return (d.alive && d.gen == gen) ? &d : nullptr;
}

Entity WorldImpl::Spawn() {
    uint32_t index, gen;
    const bool deferred = Deferred();
    {
        std::lock_guard lk(m_allocMutex);
        if (!m_free.empty()) {
            index = m_free.back();
            m_free.pop_back();
        } else {
            index = m_next++;
            uint32_t page = index >> kPageBits;
            if (page >= kMaxPages)
                throw std::runtime_error("World: entity limit reached");
            if (!m_pages[page].load())
                m_pages[page].store(new EntityData[kPageSize], std::memory_order_release);
        }
        EntityData& d = At(index);
        gen = d.gen;
        if (!deferred) {
            d = EntityData{};
            d.gen = gen;
            d.alive = true;
            m_alive.fetch_add(1, std::memory_order_relaxed);
        }
        if (index + 1 > m_published.load())
            m_published.store(index + 1, std::memory_order_release);
    }
    if (deferred) {
        Command c{};
        c.type = CmdType::Create;
        c.index = index;
        c.gen = gen;
        Submit(c);
    }
    return Access::MakeEntity(index, gen);
}

void WorldImpl::Submit(const Command& cmd) {
    if (!Deferred()) {
        Apply(cmd);
        return;
    }
    int slot = Scheduler::ThreadSlot();
    CommandBuffer& buf = *m_buffers[static_cast<size_t>(slot) % m_buffers.size()];
    std::lock_guard lk(buf.mutex);
    buf.cmds.push_back(cmd);
}

void WorldImpl::Flush() {
    for (auto& buf : m_buffers) {
        {
            std::lock_guard lk(buf->mutex);
            m_scratch.swap(buf->cmds);
        }
        for (const Command& c : m_scratch)
            Apply(c);
        m_scratch.clear();
    }
}

void WorldImpl::Apply(const Command& c) {
    if (c.index >= Capacity())
        return;
    EntityData& d = At(c.index);
    if (d.gen != c.gen)
        return;

    if (c.type == CmdType::Create) {
        if (!d.alive) {
            d = EntityData{};
            d.gen = c.gen;
            d.alive = true;
            m_alive.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (!d.alive)
        return;

    Physics& ph = *Core().grave;
    auto removeBody = [&] {
        if (d.body == UINT32_MAX)
            return;
        uint32_t moved = ph.RemoveBody(d.body);
        if (moved != UINT32_MAX)
            At(moved).body = d.body;  // в слот удалённого тела переехало последнее
        d.body = UINT32_MAX;
    };

    switch (c.type) {
    case CmdType::Create:
        break;
    case CmdType::Destroy:
        removeBody();
        d.alive = false;
        ++d.gen;
        m_alive.fetch_sub(1, std::memory_order_relaxed);
        {
            std::lock_guard lk(m_allocMutex);
            m_free.push_back(c.index);
        }
        break;
    case CmdType::SetPos:
        d.pos = c.v;
        if (d.body != UINT32_MAX) ph.SetPosition(d.body, c.v);
        break;
    case CmdType::SetRot:
        d.rot = Normalize(c.q);
        if (d.body != UINT32_MAX) ph.SetRotation(d.body, d.rot);
        break;
    case CmdType::SetScale: d.scale = c.v; break;
    case CmdType::SetMesh: d.mesh = c.u; break;
    case CmdType::SetMaterial: d.material = c.material; break;
    case CmdType::SetColor: d.material.color = c.v; break;
    case CmdType::SetVisible: d.visible = c.u != 0; break;
    case CmdType::AddBody:
        removeBody();
        d.body = ph.AddBody(c.body, d.pos, d.rot, c.index, d.gen);
        break;
    case CmdType::RemoveBody: removeBody(); break;
    case CmdType::SetVel:
        d.velocity = c.v;
        if (d.body != UINT32_MAX) ph.SetVelocity(d.body, c.v);
        break;
    case CmdType::SetAngVel:
        if (d.body != UINT32_MAX) ph.SetAngularVelocity(d.body, c.v);
        break;
    case CmdType::AddForce:
        if (d.body != UINT32_MAX) ph.AddForce(d.body, c.v);
        break;
    case CmdType::AddImpulse:
        if (d.body != UINT32_MAX) ph.AddImpulse(d.body, c.v);
        break;
    }
}

} // namespace ce::core
