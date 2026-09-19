// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone diagnostic only; never link this ioctl interposer into the host or APK.
// It loads a copied Android Vulkan HAL, queues only its own bounded transfer work,
// and verifies real retirement after each counter/submit/zero-wait experiment.
// Optional poll-repair replaces timeout=0 with a RETIRED timestamp query only in
// this process. It is an experiment, not a production driver replacement.
// Usage: probe /absolute/path/to/vulkan.ad07xx.so [original|poll-repair] [stacks]
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <hardware/hwvulkan.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static bool poll_repair, trace_stacks;
static const char* phase = "setup";
static unsigned waits;
static uint64_t now() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
extern "C" int ioctl(int fd, int req, ...) {
    va_list ap;
    va_start(ap, req);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (req != 0x400c0907UL)
        return syscall(SYS_ioctl, fd, req, arg);
    struct Wait {
        uint32_t context, timestamp, timeout;
    };
    auto w = *static_cast<Wait*>(arg);
    const auto begin = now();
    int r;
    uint32_t retired = 0;
    if (poll_repair && w.timeout == 0) {
        struct Read {
            uint32_t context, type, timestamp;
        } query{w.context, 2, 0};
        r = syscall(SYS_ioctl, fd, 0xc00c0916UL, &query);
        retired = query.timestamp;
        // KGSL timestamps wrap at 32 bits; compare with the driver's signed delta.
        if (r == 0 && int32_t(query.timestamp - w.timestamp) < 0) {
            r = -1;
            errno = ETIMEDOUT;
        }
    } else
        r = syscall(SYS_ioctl, fd, req, arg);
    int error = r < 0 ? errno : 0;
    const auto elapsed = now() - begin;
    Dl_info info{};
    dladdr(__builtin_return_address(0), &info);
    if (waits++ < 120)
        printf("IOCTL phase=%s context=%u timestamp=%u timeout=%u retired=%u result=%d errno=%d "
               "elapsed_us=%.3f caller=%s+0x%llx repair=%d\n",
               phase, w.context, w.timestamp, w.timeout, retired, r, error, double(elapsed) / 1000,
               info.dli_fname ? info.dli_fname : "?",
               (unsigned long long)((uintptr_t)__builtin_return_address(0) -
                                    (uintptr_t)info.dli_fbase),
               poll_repair && w.timeout == 0);
    if (trace_stacks && w.timeout == 0 &&
        (!strcmp(phase, "counter-query") || !strcmp(phase, "second-submit"))) {
        pthread_attr_t attr;
        void* base = nullptr;
        size_t size = 0;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            pthread_attr_getstack(&attr, &base, &size);
            pthread_attr_destroy(&attr);
        }
        auto fp = (uintptr_t*)__builtin_frame_address(0);
        for (unsigned depth = 0; base && depth < 16; depth++) {
            auto addr = (uintptr_t)fp;
            if ((addr & 15) || addr < (uintptr_t)base || addr > (uintptr_t)base + size - 16)
                break;
            auto next = fp[0], pc = fp[1];
            Dl_info frame{};
            if (!dladdr((void*)pc, &frame))
                break;
            printf("STACK phase=%s module=%s offset=0x%llx\n", phase,
                   frame.dli_fname ? frame.dli_fname : "?",
                   (unsigned long long)(pc - (uintptr_t)frame.dli_fbase));
            if (next <= addr)
                break;
            fp = (uintptr_t*)next;
        }
    }
    errno = error;
    return r;
}
#define CK(x)                                                                                      \
    do {                                                                                           \
        VkResult r = (x);                                                                          \
        if (r != VK_SUCCESS) {                                                                     \
            printf("FAIL %s: %d\n", #x, r);                                                        \
            exit(3);                                                                               \
        }                                                                                          \
    } while (0)
#define FN(n)                                                                                      \
    auto n = (PFN_##n)gpa(inst, #n);                                                               \
    if (!n) {                                                                                      \
        printf("MISSING %s\n", #n);                                                                \
        return 4;                                                                                  \
    }
int main(int argc, char** argv) {
    if (argc < 2)
        return 2;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    alarm(20);
    poll_repair = argc > 2 && !strcmp(argv[2], "poll-repair");
    trace_stacks = argc > 3 && !strcmp(argv[3], "stacks");
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        puts(dlerror());
        return 2;
    }
    auto mod = (hw_module_t*)dlsym(lib, "HMI");
    hw_device_t* hw{};
    if (!mod || mod->methods->open(mod, "vk0", &hw)) {
        puts("HAL open failed");
        return 2;
    }
    auto hal = (hwvulkan_device_t*)hw;
    auto gpa = hal->GetInstanceProcAddr;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "timeline-diagnostic";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst{};
    CK(hal->CreateInstance(&ici, nullptr, &inst));
    FN(vkEnumeratePhysicalDevices);
    FN(vkGetPhysicalDeviceProperties);
    FN(vkGetPhysicalDeviceMemoryProperties);
    FN(vkGetPhysicalDeviceQueueFamilyProperties);
    FN(vkCreateDevice);
    FN(vkGetDeviceProcAddr);
    FN(vkDestroyInstance);
    uint32_t n = 1;
    VkPhysicalDevice gpu{};
    CK(vkEnumeratePhysicalDevices(inst, &n, &gpu));
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("DEVICE %s vendor=%x device=%x repair=%d\n", props.deviceName, props.vendorID,
           props.deviceID, poll_repair);
    VkQueueFamilyProperties families[16];
    n = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, families);
    uint32_t family = 0;
    while (family < n && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        ++family;
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timeline.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &timeline;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    VkDevice dev{};
    CK(vkCreateDevice(gpu, &dci, nullptr, &dev));
#undef FN
#define FN(n)                                                                                      \
    auto n = (PFN_##n)vkGetDeviceProcAddr(dev, #n);                                                \
    if (!n) {                                                                                      \
        puts("missing " #n);                                                                       \
        return 4;                                                                                  \
    }
    FN(vkGetDeviceQueue);
    FN(vkCreateBuffer);
    FN(vkGetBufferMemoryRequirements);
    FN(vkAllocateMemory);
    FN(vkBindBufferMemory);
    FN(vkCreateCommandPool);
    FN(vkAllocateCommandBuffers);
    FN(vkBeginCommandBuffer);
    FN(vkEndCommandBuffer);
    FN(vkCmdFillBuffer);
    FN(vkCmdPipelineBarrier);
    FN(vkCreateSemaphore);
    FN(vkQueueSubmit);
    FN(vkGetSemaphoreCounterValue);
    FN(vkWaitSemaphores);
    FN(vkDeviceWaitIdle);
    FN(vkDestroySemaphore);
    FN(vkDestroyCommandPool);
    FN(vkDestroyBuffer);
    FN(vkFreeMemory);
    FN(vkDestroyDevice);
    VkQueue q{};
    vkGetDeviceQueue(dev, family, 0, &q);
    const VkDeviceSize size = 16 * 1024 * 1024;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf{};
    CK(vkCreateBuffer(dev, &bi, nullptr, &buf));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);
    uint32_t type = 0;
    while (type < mem.memoryTypeCount && !(req.memoryTypeBits & (1u << type)))
        ++type;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory allocation{};
    CK(vkAllocateMemory(dev, &mai, nullptr, &allocation));
    CK(vkBindBufferMemory(dev, buf, allocation, 0));
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = family;
    VkCommandPool pool{};
    CK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 2;
    VkCommandBuffer cmd[2];
    CK(vkAllocateCommandBuffers(dev, &cai, cmd));
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CK(vkBeginCommandBuffer(cmd[0], &cbi));
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    for (unsigned i = 0; i < 128; i++) {
        vkCmdFillBuffer(cmd[0], buf, 0, size, i);
        vkCmdPipelineBarrier(cmd[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    CK(vkEndCommandBuffer(cmd[0]));
    CK(vkBeginCommandBuffer(cmd[1], &cbi));
    CK(vkEndCommandBuffer(cmd[1]));
    for (unsigned test = 0; test < 3; test++) {
        VkSemaphoreTypeCreateInfo st{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sci.pNext = &st;
        VkSemaphore sem{};
        CK(vkCreateSemaphore(dev, &sci, nullptr, &sem));
        uint64_t value = 1;
        VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        ti.signalSemaphoreValueCount = 1;
        ti.pSignalSemaphoreValues = &value;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.pNext = &ti;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd[0];
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sem;
        phase = "initial-submit";
        auto a = now();
        CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
        printf("API %s elapsed_us=%.3f\n", phase, double(now() - a) / 1000);
        if (test == 0) {
            phase = "counter-query";
            uint64_t observed = 0;
            a = now();
            CK(vkGetSemaphoreCounterValue(dev, sem, &observed));
            printf("API %s elapsed_us=%.3f observed=%llu\n", phase, double(now() - a) / 1000,
                   (unsigned long long)observed);
        } else if (test == 1) {
            phase = "second-submit";
            value = 2;
            si.pCommandBuffers = &cmd[1];
            a = now();
            CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
            printf("API %s elapsed_us=%.3f\n", phase, double(now() - a) / 1000);
        }
        if (test == 2) {
            phase = "zero-wait";
            VkSemaphoreWaitInfo poll{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            poll.semaphoreCount = 1;
            poll.pSemaphores = &sem;
            poll.pValues = &value;
            a = now();
            auto result = vkWaitSemaphores(dev, &poll, 0);
            printf("API %s elapsed_us=%.3f result=%d\n", phase, double(now() - a) / 1000, result);
            if (result != VK_SUCCESS && result != VK_TIMEOUT) {
                puts("BAD ZERO WAIT");
                return 5;
            }
        }
        phase = "finite-retirement";
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &sem;
        wi.pValues = &value;
        a = now();
        CK(vkWaitSemaphores(dev, &wi, 2000000000ULL));
        printf("API %s elapsed_us=%.3f value=%llu\n", phase, double(now() - a) / 1000,
               (unsigned long long)value);
        uint64_t observed = 0;
        CK(vkGetSemaphoreCounterValue(dev, sem, &observed));
        if (observed != value) {
            puts("BAD RETIREMENT");
            return 5;
        }
        vkDestroySemaphore(dev, sem, nullptr);
    }
    CK(vkDeviceWaitIdle(dev));
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyBuffer(dev, buf, nullptr);
    vkFreeMemory(dev, allocation, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    hw->close(hw);
    printf("PROBE_COMPLETED retirement_verified=3 diagnostic_poll_repair=%d\n", poll_repair);
}
