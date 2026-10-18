#include "Vulkan/VulkanContext.h"
#include "Core.h"
#include "Platform/Window.h"
#include "Vulkan/VkCheck.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>

namespace ce::core {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr const char* kRtExtensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                         VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                         VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};

#ifdef NDEBUG
constexpr bool kWantValidation = false;
#else
constexpr bool kWantValidation = true;
#endif

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const char* tag = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "[VK ERROR] " : "[VK WARN] ";
    std::cerr << tag << data->pMessage << '\n';
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT DebugMessengerInfo() {
    VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = DebugCallback;
    return info;
}

bool HasLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(),
                       [&](const VkLayerProperties& l) { return std::strcmp(l.layerName, name) == 0; });
}

std::vector<VkExtensionProperties> DeviceExtensions(VkPhysicalDevice d) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(d, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(d, nullptr, &count, exts.data());
    return exts;
}

bool HasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    return std::any_of(exts.begin(), exts.end(),
                       [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

} // namespace

VulkanContext::VulkanContext(const Window& window) {
    CreateInstance();
    SetupDebugMessenger();
    m_surface = window.CreateSurface(m_instance);
    PickPhysicalDevice();
    CreateDevice();
}

VulkanContext::~VulkanContext() {
    if (m_device)
        vkDestroyDevice(m_device, nullptr);
    if (m_surface)
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_debugMessenger) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy)
            destroy(m_instance, m_debugMessenger, nullptr);
    }
    if (m_instance)
        vkDestroyInstance(m_instance, nullptr);
}

std::vector<uint32_t> VulkanContext::UniqueFamilies() const {
    std::set<uint32_t> s{m_families.graphics, m_families.present, m_families.compute, m_families.transfer};
    return {s.begin(), s.end()};
}

void VulkanContext::CreateInstance() {
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_3)
        throw std::runtime_error("Vulkan loader does not support 1.3");

    m_validation = kWantValidation && HasLayer(kValidationLayer);
    if (kWantValidation && !m_validation)
        Log("Validation layer not found (install Vulkan SDK), running without it");

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "CustomEngine";
    app.pEngineName = "CustomEngine";
    app.apiVersion = VK_API_VERSION_1_3;

    auto extensions = Window::RequiredInstanceExtensions();
    if (m_validation)
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT debugInfo = DebugMessengerInfo();
    if (m_validation) {
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = &kValidationLayer;
        info.pNext = &debugInfo;
    }
    VK_CHECK(vkCreateInstance(&info, nullptr, &m_instance));
}

void VulkanContext::SetupDebugMessenger() {
    if (!m_validation)
        return;
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
    if (!create)
        return;
    VkDebugUtilsMessengerCreateInfoEXT info = DebugMessengerInfo();
    VK_CHECK(create(m_instance, &info, nullptr, &m_debugMessenger));
}

bool VulkanContext::IsDeviceSuitable(VkPhysicalDevice device, QueueFamilies& out) const {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    if (props.apiVersion < VK_API_VERSION_1_3)
        return false;
    if (!HasExtension(DeviceExtensions(device), VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        return false;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(device, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2)
        return false;

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

    QueueFamilies r;
    for (uint32_t i = 0; i < familyCount; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &present);
        bool graphics = families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
        if (graphics && present) {
            r.graphics = r.present = i;
            break;
        }
        if (graphics && r.graphics == UINT32_MAX) r.graphics = i;
        if (present && r.present == UINT32_MAX) r.present = i;
    }
    if (r.graphics == UINT32_MAX || r.present == UINT32_MAX)
        return false;

    // GPU-лейны UniX: ищем выделенные compute и transfer семьи
    r.compute = r.graphics;
    r.transfer = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i) {
        VkQueueFlags f = families[i].queueFlags;
        if ((f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT) && r.compute == r.graphics)
            r.compute = i;
        if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) &&
            r.transfer == UINT32_MAX)
            r.transfer = i;
    }
    if (r.transfer == UINT32_MAX)
        r.transfer = r.compute;

    uint32_t formatCount = 0, modeCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, nullptr);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &modeCount, nullptr);
    if (formatCount == 0 || modeCount == 0)
        return false;

    out = r;
    return true;
}

void VulkanContext::PickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0)
        throw std::runtime_error("No Vulkan GPUs found");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    int best = -1;
    for (VkPhysicalDevice d : devices) {
        QueueFamilies families;
        if (!IsDeviceSuitable(d, families))
            continue;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 1000
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 100
                                                                                 : 1;
        if (score > best) {
            best = score;
            m_physicalDevice = d;
            m_families = families;
        }
    }
    if (!m_physicalDevice)
        throw std::runtime_error("No GPU with Vulkan 1.3 + dynamicRendering + synchronization2");

    // Поддержка ray query
    auto exts = DeviceExtensions(m_physicalDevice);
    bool rtExts = std::all_of(std::begin(kRtExtensions), std::end(kRtExtensions),
                              [&](const char* e) { return HasExtension(exts, e); });
    if (rtExts) {
        VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        as.pNext = &rq;
        f12.pNext = &as;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(m_physicalDevice, &f2);
        m_rtSupported = rq.rayQuery && as.accelerationStructure && f12.bufferDeviceAddress;
    }

    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    if (m_rtSupported)
        p2.pNext = &m_asProps;
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &p2);
    m_props = p2.properties;

    Log(std::string("GPU: ") + m_props.deviceName + " | Vulkan " + std::to_string(VK_API_VERSION_MAJOR(m_props.apiVersion)) +
        "." + std::to_string(VK_API_VERSION_MINOR(m_props.apiVersion)) +
        " | ray query: " + (m_rtSupported ? "yes" : "no"));
}

void VulkanContext::CreateDevice() {
    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    for (uint32_t family : UniqueFamilies()) {
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = family;
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        queueInfos.push_back(q);
    }

    VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rq.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    as.accelerationStructure = VK_TRUE;
    as.pNext = &rq;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    if (m_rtSupported)
        f13.pNext = &as;

    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = m_rtSupported ? VK_TRUE : VK_FALSE;
    f12.pNext = &f13;

    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;

    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (m_rtSupported)
        extensions.insert(extensions.end(), std::begin(kRtExtensions), std::end(kRtExtensions));

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &f2;
    info.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    info.pQueueCreateInfos = queueInfos.data();
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    VK_CHECK(vkCreateDevice(m_physicalDevice, &info, nullptr, &m_device));
}

} // namespace ce::core
