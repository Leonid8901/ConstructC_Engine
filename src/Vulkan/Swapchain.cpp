#include "Vulkan/Swapchain.h"
#include "Platform/Window.h"
#include "Vulkan/VkCheck.h"
#include "Vulkan/VulkanContext.h"

#include <algorithm>

namespace ce::core {

namespace {

VkSurfaceFormatKHR ChooseFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB})
        for (const auto& f : formats)
            if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return f;
    return formats[0];
}

VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes, bool vsync) {
    if (vsync)
        return VK_PRESENT_MODE_FIFO_KHR;  // гарантирован спецификацией
    for (auto want : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
        if (std::find(modes.begin(), modes.end(), want) != modes.end())
            return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

} // namespace

Swapchain::Swapchain(const VulkanContext& ctx, const Window& window, bool vsync)
    : m_ctx(ctx), m_window(window), m_vsync(vsync) {
    Create(VK_NULL_HANDLE);
}

Swapchain::~Swapchain() {
    DestroyViews();
    if (m_swapchain)
        vkDestroySwapchainKHR(m_ctx.Device(), m_swapchain, nullptr);
}

void Swapchain::Recreate() {
    int w = 0, h = 0;
    m_window.FramebufferSize(w, h);
    while (w == 0 || h == 0) {
        m_window.WaitEvents();
        m_window.FramebufferSize(w, h);
    }
    vkDeviceWaitIdle(m_ctx.Device());
    DestroyViews();
    VkSwapchainKHR old = m_swapchain;
    Create(old);
    vkDestroySwapchainKHR(m_ctx.Device(), old, nullptr);
}

void Swapchain::Create(VkSwapchainKHR oldSwapchain) {
    VkPhysicalDevice gpu = m_ctx.PhysicalDevice();
    VkSurfaceKHR surface = m_ctx.Surface();

    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, formats.data());
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &count, modes.data());

    VkSurfaceFormatKHR format = ChooseFormat(formats);

    if (caps.currentExtent.width != UINT32_MAX) {
        m_extent = caps.currentExtent;
    } else {
        int w, h;
        m_window.FramebufferSize(w, h);
        m_extent.width = std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width);
        m_extent.height = std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = m_extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = ChoosePresentMode(modes, m_vsync);
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;

    const QueueFamilies& qf = m_ctx.Families();
    uint32_t indices[] = {qf.graphics, qf.present};
    if (qf.graphics != qf.present) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = indices;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VK_CHECK(vkCreateSwapchainKHR(m_ctx.Device(), &info, nullptr, &m_swapchain));
    m_format = format.format;

    vkGetSwapchainImagesKHR(m_ctx.Device(), m_swapchain, &count, nullptr);
    m_images.resize(count);
    vkGetSwapchainImagesKHR(m_ctx.Device(), m_swapchain, &count, m_images.data());

    m_views.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = m_images[i];
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = m_format;
        v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(m_ctx.Device(), &v, nullptr, &m_views[i]));
    }
}

void Swapchain::DestroyViews() {
    for (VkImageView v : m_views)
        vkDestroyImageView(m_ctx.Device(), v, nullptr);
    m_views.clear();
}

} // namespace ce::core
