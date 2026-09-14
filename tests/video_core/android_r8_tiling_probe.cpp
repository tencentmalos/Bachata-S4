// SPDX-License-Identifier: GPL-2.0-or-later
// Isolated Android compute probe. Uses an explicit private loader; no system fallback.
#define VK_NO_PROTOTYPES
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <dlfcn.h>
#include <vulkan/vulkan.h>
#define FUNCS(F)                                                                                   \
    F(CreateInstance)                                                                              \
    F(EnumeratePhysicalDevices) F(GetPhysicalDeviceProperties)                                     \
        F(GetPhysicalDeviceQueueFamilyProperties) F(GetPhysicalDeviceMemoryProperties)             \
            F(GetPhysicalDeviceFeatures2) F(CreateDevice) F(GetDeviceQueue) F(CreateBuffer)        \
                F(GetBufferMemoryRequirements) F(AllocateMemory) F(BindBufferMemory) F(MapMemory)  \
                    F(CreateShaderModule) F(CreateDescriptorSetLayout) F(CreatePipelineLayout) F(  \
                        CreateComputePipelines) F(CreateDescriptorPool) F(AllocateDescriptorSets)  \
                        F(UpdateDescriptorSets) F(CreateCommandPool) F(AllocateCommandBuffers)     \
                            F(BeginCommandBuffer) F(CmdBindPipeline) F(CmdBindDescriptorSets)      \
                                F(CmdDispatch) F(CmdPipelineBarrier) F(EndCommandBuffer)           \
                                    F(QueueSubmit) F(QueueWaitIdle) F(DeviceWaitIdle)              \
                                        F(UnmapMemory) F(DestroyBuffer) F(FreeMemory)              \
                                            F(DestroyPipeline) F(DestroyPipelineLayout)            \
                                                F(DestroyShaderModule) F(DestroyDescriptorPool)    \
                                                    F(DestroyDescriptorSetLayout)                  \
                                                        F(DestroyCommandPool) F(DestroyDevice)     \
                                                            F(DestroyInstance)
#define DECL(n) PFN_vk##n vk##n;
FUNCS(DECL)
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        VkResult r = (x);                                                                          \
        if (r != VK_SUCCESS) {                                                                     \
            printf("FAIL %s = %d\n", #x, r);                                                       \
            return 2;                                                                              \
        }                                                                                          \
    } while (0)
std::vector<unsigned char> Read(const char* p) {
    std::ifstream s(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(s), {}};
}
int main(int argc, char** argv) {
    if (argc != 5 && argc != 6)
        return 2;
    const bool tiler = argc == 6;
    auto* shim = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!shim) {
        puts(dlerror());
        return 2;
    }
    auto factory = (void* (*)())dlsym(shim, "RenderDoc_LoadAndroidVulkan");
    auto* lib = factory ? factory() : nullptr;
    if (!lib)
        return 2;
    auto gpa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gpa(nullptr, "vkCreateInstance");
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, nullptr, &inst));
#define LOAD(n)                                                                                    \
    vk##n = (PFN_vk##n)gpa(inst, "vk" #n);                                                         \
    if (!vk##n)                                                                                    \
        vk##n = (PFN_vk##n)gpa(nullptr, "vk" #n);                                                  \
    if (!vk##n) {                                                                                  \
        puts("MISSING " #n);                                                                       \
        return 2;                                                                                  \
    }
    FUNCS(LOAD)
    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, nullptr));
    std::vector<VkPhysicalDevice> pds(n);
    CHECK(vkEnumeratePhysicalDevices(inst, &n, pds.data()));
    auto pd = pds.at(0);
    VkPhysicalDeviceProperties prop;
    vkGetPhysicalDeviceProperties(pd, &prop);
    printf("GPU %s\n", prop.deviceName);
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    v11.pNext = &v12;
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &v11;
    vkGetPhysicalDeviceFeatures2(pd, &f);
    printf("storage8=%d storage16=%d int8=%d int16=%d scalar=%d\n", v12.storageBuffer8BitAccess,
           v11.storageBuffer16BitAccess, v12.shaderInt8, f.features.shaderInt16,
           v12.scalarBlockLayout);
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
    uint32_t qi = 0;
    while (!(qs.at(qi).queueFlags & VK_QUEUE_COMPUTE_BIT))
        ++qi;
    float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qi;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qi, 0, &queue);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    auto raw = Read(argv[3]), expected = Read(argv[4]), spv = Read(argv[2]);
    if (raw.size() != 1048576 || expected.size() != raw.size() || spv.empty())
        return 2;
    VkBuffer buffers[3];
    VkDeviceMemory mem[3];
    void* mapped[3];
    for (int i = 0; i < 3; i++) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = i == 2 ? 272 : raw.size();
        bci.usage =
            i == 2 ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        CHECK(vkCreateBuffer(dev, &bci, nullptr, &buffers[i]));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, buffers[i], &req);
        uint32_t mi = 0;
        for (; mi < mp.memoryTypeCount; mi++)
            if ((req.memoryTypeBits & (1 << mi)) &&
                (mp.memoryTypes[mi].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                break;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = mi;
        CHECK(vkAllocateMemory(dev, &mai, nullptr, &mem[i]));
        CHECK(vkBindBufferMemory(dev, buffers[i], mem[i], 0));
        CHECK(vkMapMemory(dev, mem[i], 0, VK_WHOLE_SIZE, 0, &mapped[i]));
        memset(mapped[i], i == (tiler ? 0 : 1) ? 0xa5 : 0, bci.size);
    }
    memcpy(mapped[tiler ? 1 : 0], raw.data(), raw.size());
    uint32_t params[68] = {0, 1, 1, 0, 1048576, 1024, 1024, 0};
    memcpy(mapped[2], params, sizeof(params));
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (int i = 0; i < 3; i++)
        bindings[i] = {uint32_t(i),
                       i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                              : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                       1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = 3;
    slci.pBindings = bindings;
    VkDescriptorSetLayout sl;
    CHECK(vkCreateDescriptorSetLayout(dev, &slci, nullptr, &sl));
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &sl;
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spv.size();
    smci.pCode = (const uint32_t*)spv.data();
    VkShaderModule sm;
    CHECK(vkCreateShaderModule(dev, &smci, nullptr, &sm));
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  nullptr,
                  0,
                  VK_SHADER_STAGE_COMPUTE_BIT,
                  sm,
                  "main",
                  nullptr};
    cpci.layout = pl;
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(dev, {}, 1, &cpci, nullptr, &pipeline));
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = ps;
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(dev, &dpci, nullptr, &pool));
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &sl;
    VkDescriptorSet ds;
    CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));
    VkDescriptorBufferInfo dbi[3]{};
    VkWriteDescriptorSet writes[3]{};
    for (int i = 0; i < 3; i++) {
        dbi[i] = {buffers[i], 0, i == 2 ? sizeof(params) : raw.size()};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     nullptr,
                     ds,
                     uint32_t(i),
                     0,
                     1,
                     bindings[i].descriptorType,
                     nullptr,
                     &dbi[i],
                     nullptr};
    }
    vkUpdateDescriptorSets(dev, 3, writes, 0, nullptr);
    VkCommandPoolCreateInfo poolci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolci.queueFamilyIndex = qi;
    VkCommandPool cp;
    CHECK(vkCreateCommandPool(dev, &poolci, nullptr, &cp));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cp;
    cai.commandBufferCount = 1;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cb, &cbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
    vkCmdDispatch(cb, 1048576 / (tiler ? 64 : 256), 1, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &mb, 0, nullptr, 0, nullptr);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    CHECK(vkQueueSubmit(queue, 1, &si, {}));
    CHECK(vkQueueWaitIdle(queue));
    size_t diffs = 0, nz = 0;
    for (size_t i = 0; i < raw.size(); i++) {
        diffs += ((unsigned char*)mapped[tiler ? 0 : 1])[i] != expected[i];
        nz += ((unsigned char*)mapped[tiler ? 0 : 1])[i] != 0;
    }
    printf("MISMATCH=%zu NONZERO=%zu\n", diffs, nz);
    vkDestroyCommandPool(dev, cp, nullptr);
    vkDestroyPipeline(dev, pipeline, nullptr);
    vkDestroyShaderModule(dev, sm, nullptr);
    vkDestroyDescriptorPool(dev, pool, nullptr);
    vkDestroyPipelineLayout(dev, pl, nullptr);
    vkDestroyDescriptorSetLayout(dev, sl, nullptr);
    for (int i = 0; i < 3; i++) {
        vkUnmapMemory(dev, mem[i]);
        vkDestroyBuffer(dev, buffers[i], nullptr);
        vkFreeMemory(dev, mem[i], nullptr);
    }
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return diffs ? 1 : 0;
}
