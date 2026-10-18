#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace ce::core {

class Window;

struct QueueFamilies {
    uint32_t graphics = UINT32_MAX;
    uint32_t present = UINT32_MAX;
    uint32_t compute = UINT32_MAX;   // выделенная async-compute семья, иначе = graphics
    uint32_t transfer = UINT32_MAX;  // выделенная DMA-семья, иначе compute/graphics
};

// Instance, debug messenger, surface, physical/logical device.
// Требует Vulkan 1.3 (dynamic rendering + synchronization2); ray query — опционально.
class VulkanContext {
public:
    explicit VulkanContext(const Window& window);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    VkInstance Instance() const { return m_instance; }
    VkSurfaceKHR Surface() const { return m_surface; }
    VkPhysicalDevice PhysicalDevice() const { return m_physicalDevice; }
    VkDevice Device() const { return m_device; }
    const QueueFamilies& Families() const { return m_families; }
    std::vector<uint32_t> UniqueFamilies() const;
    const VkPhysicalDeviceProperties& Properties() const { return m_props; }

    bool RayTracingSupported() const { return m_rtSupported; }
    const VkPhysicalDeviceAccelerationStructurePropertiesKHR& AsProperties() const { return m_asProps; }

private:
    void CreateInstance();
    void SetupDebugMessenger();
    void PickPhysicalDevice();
    void CreateDevice();
    bool IsDeviceSuitable(VkPhysicalDevice device, QueueFamilies& out) const;

    bool m_validation = false;
    bool m_rtSupported = false;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    QueueFamilies m_families;
    VkPhysicalDeviceProperties m_props{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR m_asProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
};

} // namespace ce::core
