// User/kernel wire ABI for the NativeMali accelerator.
// Fixed-size records copied through IOExternalMethodDispatch selectors.
// Kernel side: NativeMali.cpp (NativeMaliUserClient). Userspace side:
// gpu/libdrm/native_drm.c (PanVK libdrm shim).
#ifndef NATIVE_MALI_ABI_H
#define NATIVE_MALI_ABI_H

#include <stdint.h>

enum {
    kMaliAbiVersion = 2,
    kMaliAbiMaxBuffers = 16,
    kMaliAbiMaxBo = 64,
    kMaliAbiVaWindowBytes = 0x800000,

    kMaliAbiMethodQuery = 1,
    kMaliAbiMethodBoAlloc = 2,
    kMaliAbiMethodBoMap = 3,
    kMaliAbiMethodBoFree = 4,
    kMaliAbiMethodSubmit = 5,
    kMaliAbiMethodWaitFence = 6,
    kMaliAbiMethodQueryParam = 7,
};

// Panfrost GET_PARAM ids, matching drm_panfrost_param in the UAPI.
enum {
    kMaliParamGpuProdId = 0,
    kMaliParamGpuRevision = 1,
    kMaliParamShaderPresent = 2,
    kMaliParamTilerPresent = 3,
    kMaliParamL2Present = 4,
    kMaliParamStackPresent = 5,
    kMaliParamAsPresent = 6,
    kMaliParamJsPresent = 7,
    kMaliParamL2Features = 8,
    kMaliParamCoreFeatures = 9,
    kMaliParamTilerFeatures = 10,
    kMaliParamMemFeatures = 11,
    kMaliParamMmuFeatures = 12,
    kMaliParamThreadFeatures = 13,
    kMaliParamMaxThreads = 14,
    kMaliParamThreadMaxWorkgroupSz = 15,
    kMaliParamThreadMaxBarrierSz = 16,
    kMaliParamCoherencyFeatures = 17,
    kMaliParamTextureFeatures0 = 18,
    kMaliParamTextureFeatures1 = 19,
    kMaliParamTextureFeatures2 = 20,
    kMaliParamTextureFeatures3 = 21,
    kMaliParamJsFeatures0 = 22,
    kMaliParamJsFeatures1 = 23,
    kMaliParamJsFeatures2 = 24,
    kMaliParamJsFeatures3 = 25,
    kMaliParamJsFeatures4 = 26,
    kMaliParamJsFeatures5 = 27,
    kMaliParamJsFeatures6 = 28,
    kMaliParamJsFeatures7 = 29,
    kMaliParamJsFeatures8 = 30,
    kMaliParamJsFeatures9 = 31,
    kMaliParamJsFeatures10 = 32,
    kMaliParamJsFeatures11 = 33,
    kMaliParamJsFeatures12 = 34,
    kMaliParamJsFeatures13 = 35,
    kMaliParamJsFeatures14 = 36,
    kMaliParamJsFeatures15 = 37,
    kMaliParamNrCoreGroups = 38,
    kMaliParamThreadTlsAlloc = 39,
    kMaliParamAfbcFeatures = 40,
    kMaliParamSystemTimestamp = 41,
    kMaliParamSystemTimestampFrequency = 42,
    kMaliParamAllowedJmCtxPriorities = 43,
    kMaliParamSelectedCoherency = 44,
};

struct MaliAbiParams {
    uint32_t abi_version;
    uint32_t input_bits;
    uint32_t output_bits;
    uint32_t flags;
    uint64_t max_bo_bytes;
    uint64_t va_base;
    uint64_t va_size;
};

struct MaliAbiBoAllocRequest {
    uint64_t bytes;
    uint32_t align_log2;
    uint32_t flags;
};

// BoAlloc returns the handle, the GPU VA the mapping was installed at and
// the offset userspace must pass to mmap() on the device fd.
struct MaliAbiHandle {
    uint64_t handle;
    uint64_t va;
    uint64_t mmap_offset;
    uint64_t size;
};

struct MaliAbiBoMapRequest {
    uint64_t handle;
    uint64_t iova;
    uint32_t permissions;
    uint32_t writeback;
};

struct MaliAbiSubmitRequest {
    uint64_t head_va;
    uint32_t buffer_count;
    uint32_t flags;
    uint64_t buffers[kMaliAbiMaxBuffers];
};

struct MaliAbiSubmitReply {
    uint64_t fence;
    uint32_t raw;
    uint32_t jsst;
    uint32_t af;
    uint32_t reserved;
};

struct MaliAbiWaitRequest {
    uint64_t fence;
    uint32_t timeout_ms;
    uint32_t reserved;
};

struct MaliAbiQueryParamRequest {
    uint32_t param;
    uint32_t reserved;
};

struct MaliAbiQueryParamReply {
    uint64_t value;
};

typedef struct MaliAbiParams MaliAbiParams;
typedef struct MaliAbiBoAllocRequest MaliAbiBoAllocRequest;
typedef struct MaliAbiHandle MaliAbiHandle;
typedef struct MaliAbiBoMapRequest MaliAbiBoMapRequest;
typedef struct MaliAbiSubmitRequest MaliAbiSubmitRequest;
typedef struct MaliAbiSubmitReply MaliAbiSubmitReply;
typedef struct MaliAbiWaitRequest MaliAbiWaitRequest;
typedef struct MaliAbiQueryParamRequest MaliAbiQueryParamRequest;
typedef struct MaliAbiQueryParamReply MaliAbiQueryParamReply;

// mmap of the device fd: offset selects the BO by its mmap_offset (== GPU VA
// in the current window layout); IOConnectMapMemory64 type is the BO handle.
static inline uint32_t mali_abi_mmap_type(uint64_t handle)
{
    return (uint32_t)handle;
}

#endif
