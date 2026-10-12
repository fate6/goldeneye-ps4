// Vulkan backend platform without a window (Linux test path, GE_RENDERER=vulkan):
// the system Vulkan loader, loaded at run time so the program needs no
// libvulkan to start. Frames are only seen through GE_PCDUMP dumps.

#include <dlfcn.h>

#include "system.h"
#include "gfx_vulkan.h"

static GfxVulkanPlatform s_headless;

extern "C" const struct GfxVulkanPlatform *gfx_vulkan_headless_platform(void) {
    if (!s_headless.get_instance_proc_addr) {
        void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            sysFatalError("Vulkan: cannot load libvulkan.so.1 (%s)", dlerror());
        }
        s_headless.name = "headless";
        s_headless.get_instance_proc_addr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
        if (!s_headless.get_instance_proc_addr) {
            sysFatalError("Vulkan: libvulkan.so.1 has no vkGetInstanceProcAddr");
        }
    }
    return &s_headless;
}
