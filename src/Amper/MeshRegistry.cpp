#include "Amper/MeshRegistry.h"
#include "Core.h"
#include "UniX/GpuLanes.h"
#include "Vulkan/VkCheck.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace ce::core {

MeshRegistry::MeshRegistry(Gpu& gpu, GpuLanes& lanes)
    : m_gpu(gpu), m_lanes(lanes), m_meshes(std::make_unique<MeshGpu[]>(kMaxMeshes)) {}

MeshRegistry::~MeshRegistry() {
    uint32_t n = Count();
    for (uint32_t i = 0; i < n; ++i) {
        MeshGpu& m = m_meshes[i];
        if (m.blas)
            m_gpu.Rt().destroyAs(m_gpu.Device(), m.blas, nullptr);
        m_gpu.Destroy(m.blasBuffer);
        m_gpu.Destroy(m.vertices);
        m_gpu.Destroy(m.indices);
    }
}

uint32_t MeshRegistry::Create(const Vertex* vertices, size_t vertexCount, const uint32_t* indices, size_t indexCount) {
    if (vertexCount == 0 || indexCount == 0 || indexCount % 3 != 0)
        throw std::invalid_argument("Mesh::Create: пустой меш или число индексов не кратно 3");

    uint32_t id = m_count.load();
    // слот резервируется атомарно; меш станет видимым рендеру только после ready=true
    while (true) {
        if (id >= kMaxMeshes)
            throw std::runtime_error("Mesh limit reached");
        if (m_count.compare_exchange_weak(id, id + 1))
            break;
    }
    MeshGpu& mesh = m_meshes[id];

    const VkDeviceSize vbSize = vertexCount * sizeof(Vertex);
    const VkDeviceSize ibSize = indexCount * sizeof(uint32_t);
    VkBufferUsageFlags extra = 0;
    if (m_gpu.RayTracing())
        extra = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

    mesh.vertices = m_gpu.CreateBuffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra,
                                       MemUsage::GpuOnly, true);
    mesh.indices = m_gpu.CreateBuffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra,
                                      MemUsage::GpuOnly, true);
    mesh.vertexCount = static_cast<uint32_t>(vertexCount);
    mesh.indexCount = static_cast<uint32_t>(indexCount);

    Buffer staging = m_gpu.CreateBuffer(vbSize + ibSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemUsage::Upload);
    std::memcpy(staging.mapped, vertices, vbSize);
    std::memcpy(static_cast<char*>(staging.mapped) + vbSize, indices, ibSize);

    m_lanes.Immediate(GpuLane::Transfer, [&](VkCommandBuffer cmd) {
        VkBufferCopy v{0, 0, vbSize};
        VkBufferCopy i{vbSize, 0, ibSize};
        vkCmdCopyBuffer(cmd, staging.buffer, mesh.vertices.buffer, 1, &v);
        vkCmdCopyBuffer(cmd, staging.buffer, mesh.indices.buffer, 1, &i);
    });
    m_gpu.Destroy(staging);

    if (m_gpu.RayTracing())
        BuildBlas(mesh);

    m_newResources.store(true);
    mesh.ready.store(true, std::memory_order_release);
    return id;
}

void MeshRegistry::BuildBlas(MeshGpu& mesh) {
    const RtFunctions& rt = m_gpu.Rt();
    VkDevice dev = m_gpu.Device();

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& tri = geom.geometry.triangles;
    tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tri.vertexData.deviceAddress = mesh.vertices.address;
    tri.vertexStride = sizeof(Vertex);
    tri.maxVertex = mesh.vertexCount - 1;
    tri.indexType = VK_INDEX_TYPE_UINT32;
    tri.indexData.deviceAddress = mesh.indices.address;

    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geom;

    const uint32_t primitives = mesh.indexCount / 3;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    rt.buildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primitives, &sizes);

    mesh.blasBuffer = m_gpu.CreateBuffer(sizes.accelerationStructureSize,
                                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                         MemUsage::GpuOnly, true);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = mesh.blasBuffer.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VK_CHECK(rt.createAs(dev, &ci, nullptr, &mesh.blas));

    Buffer scratch = m_gpu.CreateBuffer(sizes.buildScratchSize,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                        MemUsage::GpuOnly, false, m_gpu.ScratchAlignment());
    build.dstAccelerationStructure = mesh.blas;
    build.scratchData.deviceAddress = scratch.address;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primitives;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;

    m_lanes.Immediate(GpuLane::Compute, [&](VkCommandBuffer cmd) {
        // данные пришли из transfer-очереди: делаем их видимыми для сборки
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
        rt.cmdBuild(cmd, 1, &build, &ranges);
    });
    m_gpu.Destroy(scratch);

    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = mesh.blas;
    mesh.blasAddress = rt.asAddress(dev, &ai);
}

uint32_t MeshRegistry::Primitive(const std::string& key,
                                 const std::function<void(std::vector<Vertex>&, std::vector<uint32_t>&)>& gen) {
    std::lock_guard lk(m_primitiveMutex);
    auto it = m_primitives.find(key);
    if (it != m_primitives.end())
        return it->second;
    std::vector<Vertex> v;
    std::vector<uint32_t> i;
    gen(v, i);
    uint32_t id = Create(v.data(), v.size(), i.data(), i.size());
    m_primitives.emplace(key, id);
    return id;
}

// ---------------- генерация примитивов (CCW снаружи, нормали наружу) ----------------

void MeshRegistry::GenCube(std::vector<Vertex>& v, std::vector<uint32_t>& idx) {
    struct Face { Vec3 n, u, w; };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
    };
    for (const Face& f : faces) {
        uint32_t base = static_cast<uint32_t>(v.size());
        const float s[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (auto& c : s)
            v.push_back({(f.n + f.u * c[0] + f.w * c[1]) * 0.5f, f.n});
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

namespace {

// Сетка «широта/долгота»; offsetY сдвигает верхнюю/нижнюю половину (для капсулы)
void GenLatLong(std::vector<Vertex>& v, std::vector<uint32_t>& idx, float radius, float halfHeight,
                uint32_t segments, uint32_t rings) {
    const uint32_t half = rings / 2;
    std::vector<float> thetas;
    for (uint32_t r = 0; r <= half; ++r)
        thetas.push_back(kPi * 0.5f * r / half);
    if (halfHeight > 0)
        thetas.push_back(kPi * 0.5f);  // дубль экватора для цилиндрической части
    for (uint32_t r = 1; r <= half; ++r)
        thetas.push_back(kPi * 0.5f + kPi * 0.5f * r / half);

    const uint32_t rowCount = static_cast<uint32_t>(thetas.size());
    for (uint32_t r = 0; r < rowCount; ++r) {
        float th = thetas[r];
        bool top = halfHeight > 0 ? (r <= half) : (th <= kPi * 0.5f);
        for (uint32_t s = 0; s <= segments; ++s) {
            float ph = 2 * kPi * s / segments;
            Vec3 n{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
            Vec3 p = n * radius;
            p.y += top ? halfHeight : -halfHeight;
            v.push_back({p, n});
        }
    }
    const uint32_t stride = segments + 1;
    for (uint32_t r = 0; r + 1 < rowCount; ++r)
        for (uint32_t s = 0; s < segments; ++s) {
            uint32_t a = r * stride + s, b = (r + 1) * stride + s, c = b + 1, d = a + 1;
            idx.insert(idx.end(), {a, d, c, a, c, b});
        }
}

} // namespace

void MeshRegistry::GenSphere(std::vector<Vertex>& v, std::vector<uint32_t>& idx, uint32_t segments) {
    segments = std::max(segments, 8u);
    GenLatLong(v, idx, 0.5f, 0.0f, segments, (segments / 2) & ~1u);
}

void MeshRegistry::GenCapsule(std::vector<Vertex>& v, std::vector<uint32_t>& idx, float radius, float halfHeight,
                              uint32_t segments) {
    segments = std::max(segments, 8u);
    GenLatLong(v, idx, radius, halfHeight, segments, (segments / 2) & ~1u);
}

void MeshRegistry::GenPlane(std::vector<Vertex>& v, std::vector<uint32_t>& idx, float size) {
    float h = size * 0.5f;
    Vec3 n{0, 1, 0};
    v = {{{-h, 0, -h}, n}, {{-h, 0, h}, n}, {{h, 0, h}, n}, {{h, 0, -h}, n}};
    idx = {0, 1, 2, 0, 2, 3};
}

// Минимальный OBJ: v, vn, f (треугольники и n-угольники веером). Без нормалей — плоские нормали.
bool MeshRegistry::LoadObj(const std::string& path, std::vector<Vertex>& out, std::vector<uint32_t>& idx) {
    std::ifstream file(path);
    if (!file)
        return false;
    std::vector<Vec3> pos, nrm;
    std::map<std::pair<int, int>, uint32_t> cache;
    bool hasNormals = true;
    std::string line;
    std::vector<std::pair<int, int>> face;
    while (std::getline(file, line)) {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "v") {
            Vec3 p; ss >> p.x >> p.y >> p.z; pos.push_back(p);
        } else if (tag == "vn") {
            Vec3 n; ss >> n.x >> n.y >> n.z; nrm.push_back(Normalize(n));
        } else if (tag == "f") {
            face.clear();
            std::string tok;
            while (ss >> tok) {
                int vi = 0, ni = 0;
                size_t s1 = tok.find('/');
                vi = std::stoi(tok.substr(0, s1));
                if (s1 != std::string::npos) {
                    size_t s2 = tok.find('/', s1 + 1);
                    if (s2 != std::string::npos && s2 + 1 < tok.size())
                        ni = std::stoi(tok.substr(s2 + 1));
                }
                if (vi < 0) vi = static_cast<int>(pos.size()) + vi + 1;
                if (ni < 0) ni = static_cast<int>(nrm.size()) + ni + 1;
                if (ni == 0) hasNormals = false;
                face.emplace_back(vi - 1, ni - 1);
            }
            for (size_t k = 1; k + 1 < face.size(); ++k)
                for (auto key : {face[0], face[k], face[k + 1]}) {
                    if (!hasNormals) {  // без нормалей — каждая вершина своя (плоское затенение)
                        out.push_back({pos.at(key.first), Vec3{}});
                        idx.push_back(static_cast<uint32_t>(out.size() - 1));
                        continue;
                    }
                    auto it = cache.find(key);
                    if (it == cache.end()) {
                        out.push_back({pos.at(key.first), nrm.at(key.second)});
                        it = cache.emplace(key, static_cast<uint32_t>(out.size() - 1)).first;
                    }
                    idx.push_back(it->second);
                }
        }
    }
    if (!hasNormals)
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            Vertex& a = out[idx[i]];
            Vertex& b = out[idx[i + 1]];
            Vertex& c = out[idx[i + 2]];
            Vec3 n = Normalize(Cross(b.position - a.position, c.position - a.position));
            a.normal = b.normal = c.normal = n;
        }
    return !idx.empty();
}

} // namespace ce::core
