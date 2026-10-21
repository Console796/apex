// Include Vulkan first: Qt's Vulkan headers otherwise define VK_NO_PROTOTYPES.
#include <functional>
#include <vulkan/vulkan.h>
#include <syncstream> // Before Qt's emit macro.

#include <QApplication>
#include <QCommandLineParser>
#include <QCursor>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMetaEnum>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QScreen>
#include <QSplitter>
#include <QTimer>
#include <QVulkanInstance>
#include <QWindow>

#ifdef SPIKE_ADS
#include <DockManager.h>
#include <DockWidget.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

static void log(int id, const QString &message)
{
    auto line = message;
    line.replace('\n', "\\n").replace('\r', "\\r");
    std::osyncstream(std::cout) << "viewport=" << id << ' ' << line.toStdString() << std::endl;
}

static void check(VkResult result, const char *operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": VkResult=" + std::to_string(result));
}

// Own the native instance separately: setVkInstance() leaves ownership with us.
struct NativeInstance {
    VkInstance value = VK_NULL_HANDLE;
    ~NativeInstance() { if (value) vkDestroyInstance(value, nullptr); }
};

class Viewport final : public QWindow {
public:
    Viewport(int id, QVulkanInstance &instance) : id_(id), instance_(instance)
    {
        setSurfaceType(QSurface::VulkanSurface);
        setVulkanInstance(&instance_);
        setTitle(QString("Viewport %1").arg(id_));
        connect(this, &QWindow::screenChanged, this, [this] { scale("screen-change"); dirty_ = true; });
        connect(&timer_, &QTimer::timeout, this, [this] {
            if (!isExposed() || failed_) return;
            try { render(); }
            catch (const std::exception &e) {
                failed_ = true;
                log(id_, QString("error=%1").arg(e.what()));
                QCoreApplication::exit(1);
            }
        });
        elapsed_.start();
        timer_.start(16);
    }

    ~Viewport() override { releaseCapture(); destroyDevice(); }
    uint64_t frames() const { return frames_; }

protected:
    bool event(QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Show: scale("show"); break;
        case QEvent::DevicePixelRatioChange: scale("dpr-change"); dirty_ = true; break;
        case QEvent::Resize:
            dirty_ = true;
            log(id_, QString("resize pixels=%1x%2").arg(pixelSize().width()).arg(pixelSize().height()));
            break;
        case QEvent::FocusIn: log(id_, "focus-in"); break;
        case QEvent::FocusOut: log(id_, "focus-out"); releaseCapture(); break;
        case QEvent::Hide: releaseCapture(); break;
        case QEvent::Enter: log(id_, "mouse-enter"); break;
        case QEvent::Leave: log(id_, "mouse-leave"); break;
        case QEvent::KeyPress:
        case QEvent::KeyRelease: {
            const auto *key = static_cast<QKeyEvent *>(event);
            log(id_, QString("key-%1 key=%2 text=%3 repeat=%4")
                .arg(event->type() == QEvent::KeyPress ? "press" : "release")
                .arg(key->key()).arg(key->text().toHtmlEscaped()).arg(key->isAutoRepeat()));
            return true;
        }
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseMove: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            const bool press = event->type() == QEvent::MouseButtonPress;
            const bool release = event->type() == QEvent::MouseButtonRelease;
            log(id_, QString("mouse-%1 button=%2 x=%3 y=%4")
                .arg(press ? "press" : release ? "release" : "move")
                .arg(static_cast<int>(mouse->button())).arg(mouse->position().x()).arg(mouse->position().y()));
            if (press) { requestActivate(); if (onPress_) onPress_(); }
            if (press && mouse->button() == Qt::RightButton) {
                captured_ = true;
                last_ = mouse->position();
                setCursor(Qt::BlankCursor);
                grabbed_ = setMouseGrabEnabled(true);
                log(id_, QString("capture-start grab=%1 relative=%2")
                    .arg(grabbed_).arg(warping() ? "centre-warp" : "local-position-difference"));
                if (!grabbed_)
                    log(id_, "capture-limited reason=backend-rejected-grab unbounded-relative-motion=unavailable");
                if (warping()) warp();
            } else if (release && mouse->button() == Qt::RightButton) {
                releaseCapture();
            } else if (!press && !release && captured_) {
                const QPointF delta = mouse->position() - last_;
                if (!delta.isNull()) {
                    log(id_, QString("capture-delta dx=%1 dy=%2").arg(delta.x()).arg(delta.y()));
                    if (warping()) warp();
                    else last_ = mouse->position();
                }
            }
            return true;
        }
        case QEvent::PlatformSurface:
            log(id_, QString("platform-surface type=%1").arg(static_cast<int>(static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType())));
            if (static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType()
                == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
                releaseCapture();
                destroyDevice(); // Qt owns VkSurfaceKHR; release its swapchain first.
            }
            break;
        default: break;
        }
        return QWindow::event(event);
    }

public:
    int id() const { return id_; }
    void setOnPress(std::function<void()> fn) { onPress_ = std::move(fn); }
private:
    QSize pixelSize() const { return QSize(qRound(width() * devicePixelRatio()), qRound(height() * devicePixelRatio())); }
    bool warping() const { return QGuiApplication::platformName() == "xcb"; }
    void warp()
    {
        last_ = QPointF(width() / 2, height() / 2);
        QCursor::setPos(mapToGlobal(last_.toPoint()));
    }
public:
    void releaseCapture()
    {
        if (!captured_) return;
        captured_ = false;
        if (grabbed_) setMouseGrabEnabled(false);
        grabbed_ = false;
        unsetCursor();
        log(id_, "capture-end");
    }
    void scale(const char *reason)
    {
        log(id_, QString("scale reason=%1 dpr=%2 screen=%3 screen-dpr=%4 logical-dpi=%5")
            .arg(reason).arg(devicePixelRatio()).arg(screen() ? screen()->name() : "none")
            .arg(screen() ? screen()->devicePixelRatio() : 0)
            .arg(screen() ? screen()->logicalDotsPerInch() : 0));
    }

    void createDevice()
    {
        surface_ = QVulkanInstance::surfaceForWindow(this);
        if (!surface_) throw std::runtime_error("Qt did not create a Vulkan surface");
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance_.vkInstance(), &count, nullptr), "enumerate devices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance_.vkInstance(), &count, devices.data()), "enumerate devices");
        for (auto candidate : devices) {
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (uint32_t family = 0; family < familyCount; ++family) {
                VkBool32 present = false;
                check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface_, &present), "surface support");
                if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present
                    && instance_.supportsPresent(candidate, family, this)) {
                    physical_ = candidate;
                    family_ = family;
                    break;
                }
            }
            if (physical_) break;
        }
        if (!physical_) throw std::runtime_error("no graphics/present queue");
        VkPhysicalDeviceDriverProperties driver{};
        driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        // Driver properties are core in 1.2; use the extension only if advertised otherwise.
        VkPhysicalDeviceProperties basic{};
        vkGetPhysicalDeviceProperties(physical_, &basic);
        uint32_t extensionCount = 0;
        check(vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extensionCount, nullptr), "device extensions");
        std::vector<VkExtensionProperties> extensions(extensionCount);
        check(vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extensionCount, extensions.data()), "device extensions");
        const bool driverInfo = basic.apiVersion >= VK_API_VERSION_1_2
            || std::ranges::any_of(extensions, [](const auto &e) { return QByteArray(e.extensionName) == VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME; });
        if (driverInfo) properties.pNext = &driver;
        vkGetPhysicalDeviceProperties2(physical_, &properties);
        log(id_, QString("device=%1 driver=%2 driver-info=%3 driver-version=%4")
            .arg(basic.deviceName).arg(driverInfo ? driver.driverName : "unavailable")
            .arg(driverInfo ? driver.driverInfo : "unavailable").arg(basic.driverVersion));

        const float priority = 1.0F;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        const char *extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queueInfo;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = &extension;
        check(vkCreateDevice(physical_, &info, nullptr, &device_), "create device");
        vkGetDeviceQueue(device_, family_, 0, &queue_);
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family_;
        check(vkCreateCommandPool(device_, &poolInfo, nullptr, &pool_), "create command pool");
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = pool_;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device_, &allocation, &command_), "allocate command buffer");
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &acquired_), "create acquire semaphore");
        dirty_ = true;
    }

    void destroySwapchain()
    {
        for (auto semaphore : finished_) vkDestroySemaphore(device_, semaphore, nullptr);
        finished_.clear();
        images_.clear();
        // After Qt silently destroyed the surface (and its wl_display queue) the swapchain
        // cannot be destroyed without crashing in libwayland; abandon it.
        if (swapchain_ && !surfaceLost_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    void destroyDevice()
    {
        if (!device_) return;
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        if (acquired_) vkDestroySemaphore(device_, acquired_, nullptr);
        if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
        physical_ = VK_NULL_HANDLE;
        acquired_ = VK_NULL_HANDLE;
        pool_ = VK_NULL_HANDLE;
        surface_ = VK_NULL_HANDLE;
        log(id_, "device-destroyed");
    }

    bool recreate()
    {
        const QSize pixels = pixelSize();
        if (pixels.isEmpty()) return false;
        check(vkDeviceWaitIdle(device_), "wait before recreate");
        VkSurfaceCapabilitiesKHR caps{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), "surface capabilities");
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            throw std::runtime_error("surface does not support transfer clears");
        extent_ = caps.currentExtent;
        if (extent_.width == std::numeric_limits<uint32_t>::max()) {
            extent_.width = std::clamp(static_cast<uint32_t>(pixels.width()), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent_.height = std::clamp(static_cast<uint32_t>(pixels.height()), caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (!extent_.width || !extent_.height) return false;
        uint32_t count = 0;
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr), "surface formats");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data()), "surface formats");
        if (formats.empty()) throw std::runtime_error("no surface formats");
        auto format = formats.front();
        for (auto candidate : formats)
            if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) format = candidate;
        if (format.format == VK_FORMAT_UNDEFINED) format.format = VK_FORMAT_B8G8R8A8_UNORM;
        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount) imageCount = std::min(imageCount, caps.maxImageCount);
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (auto candidate : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                               VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
            if (caps.supportedCompositeAlpha & candidate) { alpha = candidate; break; }
        }
        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface_;
        info.minImageCount = imageCount;
        info.imageFormat = format.format;
        info.imageColorSpace = format.colorSpace;
        info.imageExtent = extent_;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = alpha;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        info.oldSwapchain = swapchain_;
        VkSwapchainKHR replacement = VK_NULL_HANDLE;
        check(vkCreateSwapchainKHR(device_, &info, nullptr, &replacement), "create swapchain");
        destroySwapchain();
        swapchain_ = replacement;
        check(vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr), "swapchain images");
        images_.resize(count);
        check(vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data()), "swapchain images");
        finished_.resize(count, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (auto &semaphore : finished_)
            check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore), "create present semaphore");
        dirty_ = false;
        log(id_, QString("swapchain pixels=%1x%2 images=%3").arg(extent_.width).arg(extent_.height).arg(count));
        return true;
    }

    void render()
    {
        // Reparenting recreates the native window; a changed or missing VkSurfaceKHR means
        // everything built on the old one must go before the new one is used.
        const VkSurfaceKHR current = QVulkanInstance::surfaceForWindow(this);
        if (device_ && current != surface_) {
            log(id_, "surface-changed (no SurfaceAboutToBeDestroyed was delivered)");
            surfaceLost_ = true;
            destroyDevice();
            surfaceLost_ = false;
        }
        if (!current) return;
        if (!device_) createDevice();
        if (dirty_ && !recreate()) return;
        // Intentionally one submission at a time. Present semaphores are per image:
        // reacquiring that image guarantees the previous presentation consumed it.
        check(vkQueueWaitIdle(queue_), "wait frame");
        uint32_t index = 0;
        auto result = vkAcquireNextImageKHR(device_, swapchain_, 0, acquired_, VK_NULL_HANDLE, &index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) { dirty_ = true; log(id_, "acquire-out-of-date"); return; }
        if (result == VK_NOT_READY || result == VK_TIMEOUT) return;
        if (result == VK_SUBOPTIMAL_KHR) dirty_ = true;
        else check(result, "acquire image");
        check(vkResetCommandBuffer(command_, 0), "reset commands");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command_, &begin), "begin commands");
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // discard previous clear
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images_[index];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        const float pulse = 0.25F + 0.18F * std::sin(static_cast<float>(elapsed_.elapsed()) / 600.0F);
        const VkClearColorValue color = {{id_ == 1 ? 0.65F : pulse, pulse, id_ == 2 ? 0.65F : 0.12F, 1.0F}};
        vkCmdClearColorImage(command_, images_[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        check(vkEndCommandBuffer(command_), "end commands");
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired_;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &finished_[index];
        check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "submit");
        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &finished_[index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &index;
        instance_.presentAboutToBeQueued(this);
        result = vkQueuePresentKHR(queue_, &present);
        instance_.presentQueued(this);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) { dirty_ = true; log(id_, "present-out-of-date"); return; }
        if (result == VK_SUBOPTIMAL_KHR) dirty_ = true;
        else check(result, "present");
        ++frames_;
        if (frames_ == 1 || frames_ % 60 == 0) log(id_, QString("presented frames=%1").arg(frames_));
    }

private:
    int id_;
    std::function<void()> onPress_;
    QVulkanInstance &instance_;
    QTimer timer_;
    QElapsedTimer elapsed_;
    bool dirty_ = true;
    bool failed_ = false;
    bool captured_ = false;
    bool grabbed_ = false;
    QPointF last_;
    uint64_t frames_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    bool surfaceLost_ = false;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> finished_;
};

// Logs focus and keyboard events seen by anything else in the app (main window, containers),
// so key routing can be told apart from "keys never reached the viewport". A right-button
// release delivered to a different window than the press still ends every capture.
class InputProbe final : public QObject {
public:
    explicit InputProbe(std::vector<Viewport *> viewports) : viewports_(std::move(viewports)) {}

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::FocusIn:
        case QEvent::FocusOut:
        case QEvent::WindowActivate:
        case QEvent::WindowDeactivate:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            if (!dynamic_cast<Viewport *>(object))
                log(0, QString("app-event %1 target=%2 name=%3")
                    .arg(QMetaEnum::fromType<QEvent::Type>().valueToKey(event->type()), object->metaObject()->className(), object->objectName()));
            break;
        case QEvent::MouseButtonRelease:
            if (static_cast<QMouseEvent *>(event)->button() == Qt::RightButton)
                for (auto *viewport : viewports_) viewport->releaseCapture();
            break;
        default: break;
        }
        return false;
    }

private:
    std::vector<Viewport *> viewports_;
};

// On Wayland, keyboard focus stays on the top-level surface and the embedded native window
// never sees a key. Forward the container's keys to its Viewport so routing can be judged.
class KeyForwarder final : public QObject {
public:
    KeyForwarder(QWidget *container, Viewport *viewport) : QObject(container), viewport_(viewport)
    {
        container->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) return false;
        const auto *key = static_cast<QKeyEvent *>(event);
        QKeyEvent copy(key->type(), key->key(), key->modifiers(), key->text(), key->isAutoRepeat());
        QCoreApplication::sendEvent(viewport_, &copy);
        return true;
    }

private:
    Viewport *viewport_;
};

int main(int argc, char **argv)
{
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &message) {
        log(0, QString("qt severity=%1 message=%2").arg(static_cast<int>(type)).arg(message));
    });
    QApplication app(argc, argv);
    log(0, "platform=" + QGuiApplication::platformName());
    QCommandLineParser parser;
    parser.setApplicationDescription("Two native Vulkan windows inside Qt Widgets; right mouse holds capture.");
    parser.addHelpOption();
    parser.addOption({"layout", "splitter or dock", "layout", "splitter"});
    parser.addOption({"probe-grab", "After show, try QWindow::setMouseGrabEnabled on each viewport, log the result and exit."});
    parser.addOption({"no-float", "Dock layout: make dock widgets non-floatable."});
    parser.addOption({"smoke", "Resize once and exit after 2 seconds; require 10 presents per viewport."});
    parser.process(app);
    const auto layout = parser.value("layout");
    if (layout != "splitter" && layout != "dock") { log(0, "error=invalid layout"); return 1; }

    try {
        NativeInstance native;
        QVulkanInstance instance;
        const auto available = instance.supportedExtensions();
        std::vector<const char *> extensions = {VK_KHR_SURFACE_EXTENSION_NAME};
        const auto platform = QGuiApplication::platformName();
        if (platform.startsWith("wayland")) extensions.push_back("VK_KHR_wayland_surface");
        else if (platform == "xcb") { extensions.push_back("VK_KHR_xcb_surface"); extensions.push_back("VK_KHR_xlib_surface"); }
        else throw std::runtime_error("this spike needs wayland or xcb (offscreen has no Vulkan surface)");
        for (const char *extension : extensions)
            if (!available.contains(extension)) throw std::runtime_error(std::string("missing extension ") + extension);
        if (available.contains(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        const char *validation = "VK_LAYER_KHRONOS_validation";
        const bool validated = instance.supportedLayers().contains(validation);
        log(0, QString("validation=%1").arg(validated ? "enabled" : "unavailable"));
        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "wayland-vulkan-spike";
        application.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &application;
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        info.enabledLayerCount = validated ? 1 : 0;
        info.ppEnabledLayerNames = validated ? &validation : nullptr;
        check(vkCreateInstance(&info, nullptr, &native.value), "create native instance");
        instance.setVkInstance(native.value);
        instance.setApiVersion(QVersionNumber(1, 2));
        if (!instance.create()) throw std::runtime_error("QVulkanInstance::create failed");
        log(0, "shared-instance=created layout=" + layout);

        QMainWindow window;
        window.setWindowTitle("Wayland / Vulkan spike — right mouse captures");
        auto *first = new Viewport(1, instance);
        auto *second = new Viewport(2, instance);
        InputProbe probe({first, second});
        app.installEventFilter(&probe);
        auto *left = QWidget::createWindowContainer(first);
        auto *right = QWidget::createWindowContainer(second);
        new KeyForwarder(left, first);
        new KeyForwarder(right, second);
        first->setOnPress([left] { left->setFocus(Qt::MouseFocusReason); });
        second->setOnPress([right] { right->setFocus(Qt::MouseFocusReason); });
        for (auto *container : {left, right}) {
            container->setFocusPolicy(Qt::StrongFocus);
            container->setMinimumSize(160, 120);
        }
        if (layout == "splitter") {
            auto *splitter = new QSplitter;
            splitter->addWidget(left);
            splitter->addWidget(right);
            window.setCentralWidget(splitter);
        } else {
#ifdef SPIKE_ADS
            auto *manager = new ads::CDockManager(&window);
            auto *a = new ads::CDockWidget(manager, "Viewport 1");
            auto *b = new ads::CDockWidget(manager, "Viewport 2");
            a->setWidget(left);
            b->setWidget(right);
            if (parser.isSet("no-float")) {
                a->setFeature(ads::CDockWidget::DockWidgetFloatable, false);
                b->setFeature(ads::CDockWidget::DockWidgetFloatable, false);
            }
            auto *area = manager->addDockWidget(ads::LeftDockWidgetArea, a);
            manager->addDockWidget(ads::RightDockWidgetArea, b, area);
            log(0, "docks=qt-ads");
#else
            auto *a = new QDockWidget("Viewport 1", &window);
            auto *b = new QDockWidget("Viewport 2", &window);
            a->setWidget(left);
            b->setWidget(right);
            window.addDockWidget(Qt::LeftDockWidgetArea, a);
            window.addDockWidget(Qt::RightDockWidgetArea, b);
            window.setDockOptions(QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);
            log(0, "docks=qt");
#endif
        }
        window.resize(1000, 600);
        window.show();
        if (parser.isSet("smoke")) {
            QTimer::singleShot(700, &window, [&window] { window.resize(1080, 640); });
            QTimer::singleShot(2000, &window, [&app] { app.exit(0); });
        }
        if (parser.isSet("probe-grab")) {
            QTimer::singleShot(800, &window, [&] {
                for (auto *viewport : {first, second}) {
                    viewport->requestActivate();
                    const bool granted = viewport->setMouseGrabEnabled(true);
                    log(viewport->id(), QString("probe-grab active=%1 granted=%2 focused=%3")
                        .arg(viewport->isActive()).arg(granted)
                        .arg(QGuiApplication::focusWindow() == viewport));
                    viewport->setMouseGrabEnabled(false);
                }
                app.exit(0);
            });
        }
        const int status = app.exec();
        if (parser.isSet("smoke")) {
            constexpr uint64_t minimumFrames = 10;
            const bool passed = status == 0 && first->frames() >= minimumFrames && second->frames() >= minimumFrames;
            log(1, QString("smoke frames=%1 minimum=%2").arg(first->frames()).arg(minimumFrames));
            log(2, QString("smoke frames=%1 minimum=%2").arg(second->frames()).arg(minimumFrames));
            log(0, QString("smoke=%1").arg(passed ? "pass" : "fail"));
            return passed ? 0 : 1;
        }
        return status; // window/containers die before QVulkanInstance, then native instance.
    } catch (const std::exception &e) {
        log(0, QString("error=%1").arg(e.what()));
        return 1;
    }
}
