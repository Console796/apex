#include "render/context.hpp"

#include "core/log.hpp"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace apex::render {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr std::size_t kMaxStoredErrors = 64;

bool containsInsensitive(std::string_view haystack, std::string_view needle) {
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                       [&](char a, char b) { return lower(a) == lower(b); }) != haystack.end();
}

bool loaderReady() {
    static const bool ready = volkInitialize() == VK_SUCCESS;
    return ready;
}

std::vector<VkExtensionProperties> instanceExtensions() {
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> result(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, result.data());
    result.resize(count);
    return result;
}

bool hasExtension(const std::vector<VkExtensionProperties>& list, std::string_view name) {
    return std::ranges::any_of(list, [&](const auto& e) { return name == e.extensionName; });
}

bool layerAvailable(const char* name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    layers.resize(count);
    return std::ranges::any_of(layers, [&](const auto& l) { return std::strcmp(l.layerName, name) == 0; });
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* user) {
    auto* log = static_cast<ValidationLog*>(user);
    const char* text = data && data->pMessage ? data->pMessage : "(no message)";
    const bool isError = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if (isError) {
        apex::log::error("vulkan validation: {}", text);
    } else {
        apex::log::warn("vulkan validation: {}", text);
    }
    log->record(isError, text);
    return VK_FALSE;
}

struct Candidate {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    std::uint32_t family = 0;
    int score = 0;
};

// Empty string if the device satisfies the baseline, otherwise the first missing requirement.
std::string checkBaseline(VkPhysicalDevice device, const VkPhysicalDeviceProperties& props) {
    if (props.apiVersion < VK_API_VERSION_1_3) return "Vulkan 1.3 not supported";
    VkPhysicalDeviceVulkan13Features f13 = vkStruct<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    VkPhysicalDeviceVulkan12Features f12 = vkStruct<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    VkPhysicalDeviceFeatures2 f2 = vkStruct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    f12.pNext = &f13;
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(device, &f2);
    if (!f13.dynamicRendering) return "dynamicRendering";
    if (!f13.synchronization2) return "synchronization2";
    if (!f12.descriptorIndexing) return "descriptorIndexing";
    if (!f12.runtimeDescriptorArray) return "runtimeDescriptorArray";
    if (!f12.descriptorBindingPartiallyBound) return "descriptorBindingPartiallyBound";
    if (!f12.descriptorBindingSampledImageUpdateAfterBind) return "descriptorBindingSampledImageUpdateAfterBind";
    if (!f12.shaderSampledImageArrayNonUniformIndexing) return "shaderSampledImageArrayNonUniformIndexing";
    if (!f12.bufferDeviceAddress) return "bufferDeviceAddress";
    if (!f2.features.textureCompressionBC) return "textureCompressionBC";
    return {};
}

bool deviceHasExtension(VkPhysicalDevice device, const char* name) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> list(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, list.data());
    list.resize(count);
    return hasExtension(list, name);
}

} // namespace

std::size_t ValidationLog::errorCount() const {
    std::scoped_lock lock(mutex_);
    return errorCount_;
}
std::size_t ValidationLog::warningCount() const {
    std::scoped_lock lock(mutex_);
    return warningCount_;
}
std::vector<std::string> ValidationLog::errors() const {
    std::scoped_lock lock(mutex_);
    return errors_;
}
void ValidationLog::clear() {
    std::scoped_lock lock(mutex_);
    errorCount_ = warningCount_ = 0;
    errors_.clear();
}
void ValidationLog::record(bool isError, std::string message) {
    std::scoped_lock lock(mutex_);
    if (isError) {
        ++errorCount_;
        if (errors_.size() < kMaxStoredErrors) errors_.push_back(std::move(message));
    } else {
        ++warningCount_;
    }
}

bool Context::instanceExtensionSupported(const char* name) {
    return loaderReady() && hasExtension(instanceExtensions(), name);
}

Result<std::unique_ptr<Context>> Context::create(const ContextOptions& options) {
    if (!loaderReady()) return std::unexpected(Error{ErrorCode::Unsupported, "Vulkan loader not found"});
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Vulkan 1.3 instance not available"});
    }

    std::unique_ptr<Context> ctx(new Context());
    ctx->validation_ = std::make_unique<ValidationLog>();

    bool wantValidation = options.validation;
    if (const char* env = std::getenv("APEX_VK_VALIDATION"); env && *env) wantValidation = env[0] != '0';

    const auto available = instanceExtensions();
    std::vector<const char*> extensions;
    for (const auto& name : options.instanceExtensions) {
        if (!hasExtension(available, name)) {
            return std::unexpected(Error{ErrorCode::Unsupported, "instance extension missing: " + name});
        }
        extensions.push_back(name.c_str());
    }
    std::vector<const char*> layers;
    bool messenger = false;
    if (wantValidation) {
        if (!layerAvailable(kValidationLayer)) {
            log::warn("Vulkan validation requested but {} is not installed", kValidationLayer);
        } else {
            layers.push_back(kValidationLayer);
            if (hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
                if (std::ranges::none_of(extensions, [](const char* e) {
                        return std::strcmp(e, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
                    })) {
                    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                }
                messenger = true;
            }
        }
    }
    ctx->info_.validation = !layers.empty();

    VkApplicationInfo app = vkStruct<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.pApplicationName = "Apex";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = vkStruct<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    APEX_VK_TRY(vkCreateInstance(&ici, nullptr, &ctx->instance_), "vkCreateInstance");
    volkLoadInstance(ctx->instance_);

    if (messenger) {
        VkDebugUtilsMessengerCreateInfoEXT mci = vkStruct<VkDebugUtilsMessengerCreateInfoEXT>(VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = debugCallback;
        mci.pUserData = ctx->validation_.get();
        APEX_VK_TRY(vkCreateDebugUtilsMessengerEXT(ctx->instance_, &mci, nullptr, &ctx->messenger_),
                    "vkCreateDebugUtilsMessengerEXT");
    }

    // Physical device selection.
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(ctx->instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(ctx->instance_, &count, devices.data());
    devices.resize(count);
    if (devices.empty()) return std::unexpected(Error{ErrorCode::NotFound, "no Vulkan physical devices"});

    std::string filter = options.deviceNameFilter;
    if (const char* env = std::getenv("APEX_VK_DEVICE"); env && *env) filter = env;

    Candidate best;
    std::string rejected;
    for (VkPhysicalDevice device : devices) {
        Candidate c;
        c.device = device;
        vkGetPhysicalDeviceProperties(device, &c.props);
        const std::string name = c.props.deviceName;
        if (!filter.empty() && !containsInsensitive(name, filter)) continue;
        if (const std::string missing = checkBaseline(device, c.props); !missing.empty()) {
            rejected += " [" + name + ": missing " + missing + "]";
            continue;
        }
        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
        bool found = false;
        for (std::uint32_t i = 0; i < familyCount; ++i) {
            constexpr VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
            if ((families[i].queueFlags & need) == need) {
                c.family = i;
                found = true;
                break;
            }
        }
        if (!found) {
            rejected += " [" + name + ": no graphics+compute queue]";
            continue;
        }
        switch (c.props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: c.score = 4; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: c.score = 3; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: c.score = 2; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: c.score = 1; break;
        default: c.score = 0; break;
        }
        c.score += 1; // Any acceptable device beats the empty candidate.
        if (c.score > best.score) best = c;
    }
    if (!best.device) {
        std::string message = filter.empty() ? "no suitable Vulkan device" : "no Vulkan device matches '" + filter + "'";
        return std::unexpected(Error{filter.empty() ? ErrorCode::Unsupported : ErrorCode::NotFound,
                                     message + rejected});
    }
    ctx->physical_ = best.device;
    ctx->family_ = best.family;
    ctx->info_.name = best.props.deviceName;
    ctx->info_.type = best.props.deviceType;
    ctx->info_.apiVersion = best.props.apiVersion;
    ctx->info_.vendorId = best.props.vendorID;
    ctx->info_.deviceId = best.props.deviceID;
    ctx->info_.rayQuery = deviceHasExtension(best.device, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    ctx->info_.accelerationStructure =
        deviceHasExtension(best.device, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    // VK_KHR_swapchain requires VK_KHR_surface on the instance, i.e. a UI/surface context.
    const bool surfaceEnabled = std::ranges::any_of(extensions, [](const char* e) {
        return std::strcmp(e, VK_KHR_SURFACE_EXTENSION_NAME) == 0;
    });
    ctx->info_.swapchain = surfaceEnabled && deviceHasExtension(best.device, VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(best.device, &supported);
    ctx->info_.anisotropy = supported.samplerAnisotropy == VK_TRUE;
    ctx->info_.fillModeNonSolid = supported.fillModeNonSolid == VK_TRUE;

    // Logical device with exactly the baseline (plus anisotropy when present).
    VkPhysicalDeviceVulkan13Features f13 = vkStruct<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = vkStruct<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    f12.pNext = &f13;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = vkStruct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    f2.pNext = &f12;
    f2.features.textureCompressionBC = VK_TRUE;
    f2.features.samplerAnisotropy = supported.samplerAnisotropy;
    f2.features.fillModeNonSolid = supported.fillModeNonSolid;

    std::vector<const char*> deviceExtensions;
    if (ctx->info_.swapchain) deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = vkStruct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    qci.queueFamilyIndex = ctx->family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci = vkStruct<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
    dci.ppEnabledExtensionNames = deviceExtensions.data();
    APEX_VK_TRY(vkCreateDevice(ctx->physical_, &dci, nullptr, &ctx->device_), "vkCreateDevice");
    volkLoadDevice(ctx->device_);
    vkGetDeviceQueue(ctx->device_, ctx->family_, 0, &ctx->queue_);

    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    aci.physicalDevice = ctx->physical_;
    aci.device = ctx->device_;
    aci.instance = ctx->instance_;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.pVulkanFunctions = &functions;
    APEX_VK_TRY(vmaCreateAllocator(&aci, &ctx->allocator_), "vmaCreateAllocator");

    VkCommandPoolCreateInfo pci = vkStruct<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = ctx->family_;
    APEX_VK_TRY(vkCreateCommandPool(ctx->device_, &pci, nullptr, &ctx->pool_), "vkCreateCommandPool");
    VkFenceCreateInfo fci = vkStruct<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    APEX_VK_TRY(vkCreateFence(ctx->device_, &fci, nullptr, &ctx->fence_), "vkCreateFence");

    log::info("Vulkan device: {} (api {}.{}, validation {}, ray_query {}, accel_struct {})",
              ctx->info_.name, VK_API_VERSION_MAJOR(ctx->info_.apiVersion),
              VK_API_VERSION_MINOR(ctx->info_.apiVersion), ctx->info_.validation,
              ctx->info_.rayQuery, ctx->info_.accelerationStructure);
    return ctx;
}

Context::~Context() {
    if (device_) vkDeviceWaitIdle(device_);
    if (fence_) vkDestroyFence(device_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    if (allocator_) vmaDestroyAllocator(allocator_);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

void Context::waitIdle() const {
    vkDeviceWaitIdle(device_);
}

Result<void> Context::submitAndWait(VkCommandBuffer commands) {
    VkCommandBufferSubmitInfo cb = vkStruct<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
    cb.commandBuffer = commands;
    VkSubmitInfo2 submit = vkStruct<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cb;
    APEX_VK_TRY(vkQueueSubmit2(queue_, 1, &submit, fence_), "vkQueueSubmit2");
    const VkResult waited = vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &fence_);
    APEX_VK_TRY(waited, "vkWaitForFences");
    return {};
}

Result<void> Context::oneShot(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo ai = vkStruct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    APEX_VK_TRY(vkAllocateCommandBuffers(device_, &ai, &cb), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi = vkStruct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Result<void> result{};
    if (const VkResult r = vkBeginCommandBuffer(cb, &bi); r != VK_SUCCESS) {
        result = std::unexpected(vkError(r, "vkBeginCommandBuffer"));
    } else {
        record(cb);
        if (const VkResult e = vkEndCommandBuffer(cb); e != VK_SUCCESS) {
            result = std::unexpected(vkError(e, "vkEndCommandBuffer"));
        } else {
            result = submitAndWait(cb);
        }
    }
    vkFreeCommandBuffers(device_, pool_, 1, &cb);
    return result;
}

} // namespace apex::render
