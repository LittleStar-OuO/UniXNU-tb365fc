// First on-device probe for the NativeMali Vulkan/Metal stack.
//
// Loads the PanVK ICD dylib directly (no Vulkan loader needed), creates an
// instance and device through the libdrm-darwin shim, and exercises the BO
// path: allocate a buffer, map it, write a pattern and read it back.
// Results go to stdout/stderr and to /tmp/mali-vk-probe.log.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#define PROBE_DYLIB "/private/var/ngpu/bin/libvulkan_panfrost.dylib"
#define PROBE_LOG "/private/var/ngpu/mali-vk-probe.log"

static FILE *g_log;
static FILE *g_console;

static void probe(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    fputs(line, stderr);
    if (g_log) {
        fputs(line, g_log);
        fflush(g_log);
    }
    if (g_console) {
        fputs(line, g_console);
        fflush(g_console);
    }
}

static void probe_open_logs(void)
{
    g_log = fopen(PROBE_LOG, "w");
    g_console = fopen("/dev/console", "w");
}

typedef PFN_vkVoidFunction (*PFN_icd_gipa)(VkInstance, const char *);

int main(void)
{
    probe_open_logs();
    probe("mali-vk-probe start\n");
    /* launchd starts the job early; give the accelerator service time to appear. */
    sleep(5);

    void *lib = dlopen(PROBE_DYLIB, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        probe("dlopen failed: %s\n", dlerror());
        return 11;
    }
    probe("dlopen ok\n");

    PFN_icd_gipa gipa = (PFN_icd_gipa)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!gipa) {
        probe("vk_icdGetInstanceProcAddr missing\n");
        return 12;
    }

    PFN_vkCreateInstance createInstance =
        (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!createInstance) {
        probe("no vkCreateInstance\n");
        return 13;
    }

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "mali-vk-probe",
        .applicationVersion = 1,
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult res = createInstance(&ici, NULL, &instance);
    probe("vkCreateInstance=%d instance=%p\n", res, (void *)instance);
    if (res != VK_SUCCESS)
        return 14;

    PFN_vkEnumeratePhysicalDevices enumerate =
        (PFN_vkEnumeratePhysicalDevices)gipa(instance,
            "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties getProps =
        (PFN_vkGetPhysicalDeviceProperties)gipa(instance,
            "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties getMemProps =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance,
            "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkCreateDevice createDevice =
        (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr gdpa =
        (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
    if (!enumerate || !getProps || !getMemProps || !createDevice || !gdpa) {
        probe("missing entrypoints\n");
        return 15;
    }

    uint32_t count = 0;
    res = enumerate(instance, &count, NULL);
    probe("vkEnumeratePhysicalDevices count=%u res=%d\n", count, res);
    if (res != VK_SUCCESS || !count)
        return 16;

    VkPhysicalDevice phys[4] = {};
    count = 4;
    res = enumerate(instance, &count, phys);
    probe("enumerate list res=%d count=%u\n", res, count);
    if (res != VK_SUCCESS || !count)
        return 17;

    VkPhysicalDeviceProperties props;
    getProps(phys[0], &props);
    probe("gpu=%s api=%u.%u.%u driver=0x%08x type=%u\n", props.deviceName,
        VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
        VK_VERSION_PATCH(props.apiVersion), props.driverVersion,
        props.deviceType);

    VkPhysicalDeviceMemoryProperties memProps;
    getMemProps(phys[0], &memProps);
    probe("memory types=%u heaps=%u\n", memProps.memoryTypeCount,
        memProps.memoryHeapCount);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    VkDevice device = VK_NULL_HANDLE;
    res = createDevice(phys[0], &dci, NULL, &device);
    probe("vkCreateDevice=%d device=%p\n", res, (void *)device);
    if (res != VK_SUCCESS)
        return 18;

    PFN_vkCreateBuffer createBuffer =
        (PFN_vkCreateBuffer)gdpa(device, "vkCreateBuffer");
    PFN_vkDestroyBuffer destroyBuffer =
        (PFN_vkDestroyBuffer)gdpa(device, "vkDestroyBuffer");
    PFN_vkGetBufferMemoryRequirements getReqs =
        (PFN_vkGetBufferMemoryRequirements)gdpa(device,
            "vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory allocMemory =
        (PFN_vkAllocateMemory)gdpa(device, "vkAllocateMemory");
    PFN_vkFreeMemory freeMemory =
        (PFN_vkFreeMemory)gdpa(device, "vkFreeMemory");
    PFN_vkBindBufferMemory bindBuffer =
        (PFN_vkBindBufferMemory)gdpa(device, "vkBindBufferMemory");
    PFN_vkMapMemory mapMemory = (PFN_vkMapMemory)gdpa(device, "vkMapMemory");
    PFN_vkUnmapMemory unmapMemory =
        (PFN_vkUnmapMemory)gdpa(device, "vkUnmapMemory");
    if (!createBuffer || !getReqs || !allocMemory || !bindBuffer ||
        !mapMemory || !unmapMemory) {
        probe("missing device entrypoints\n");
        return 19;
    }

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 0x4000,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    res = createBuffer(device, &bci, NULL, &buffer);
    probe("vkCreateBuffer=%d\n", res);
    if (res != VK_SUCCESS)
        return 20;

    VkMemoryRequirements reqs;
    getReqs(device, buffer, &reqs);
    probe("buffer reqs size=%llu align=%llu typeBits=0x%x\n",
        (unsigned long long)reqs.size, (unsigned long long)reqs.alignment,
        reqs.memoryTypeBits);

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = (uint32_t)__builtin_ctz(reqs.memoryTypeBits),
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    res = allocMemory(device, &mai, NULL, &memory);
    probe("vkAllocateMemory=%d\n", res);
    if (res != VK_SUCCESS)
        return 21;

    res = bindBuffer(device, buffer, memory, 0);
    probe("vkBindBufferMemory=%d\n", res);
    if (res != VK_SUCCESS)
        return 22;

    void *map = NULL;
    res = mapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map);
    probe("vkMapMemory=%d map=%p\n", res, map);
    if (res != VK_SUCCESS)
        return 23;

    uint32_t *words = (uint32_t *)map;
    for (uint32_t i = 0; i < 16; i++)
        words[i] = 0xa5a50000u | i;
    unmapMemory(device, memory);

    res = mapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map);
    uint32_t mismatches = 0;
    if (res == VK_SUCCESS) {
        words = (uint32_t *)map;
        for (uint32_t i = 0; i < 16; i++) {
            if (words[i] != (0xa5a50000u | i))
                mismatches++;
        }
        unmapMemory(device, memory);
    }
    probe("readback mismatches=%u res=%d\n", mismatches, res);

    destroyBuffer(device, buffer, NULL);
    freeMemory(device, memory, NULL);
    int failed = res != VK_SUCCESS || mismatches != 0;
    probe("BO CPU readback %s (GPU submission not tested)\n", failed ? "FAIL" : "PASS");
    if (g_log)
        fclose(g_log);
    return failed ? 24 : 0;
}
