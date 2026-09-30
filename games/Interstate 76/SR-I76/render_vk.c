/**
 *
 *  Rendering backend: Vulkan (Linux; SR-I76.cfg graphics_api = vulkan).
 *
 *  Same design as render_gl.c / render_d3d11.c: 2D pictures are uploaded to a texture and drawn letterboxed
 *  into the window; Glide draws into front/back color images (+ a 32-bit float depth image; with MSAA
 *  multisampled images that are resolved when the Glide render pass ends) at the render target size; one
 *  pixel shader implements the Glide pixel pipeline from a uniform block (shaders/, vk_shaders.h).
 *
 *  The game runs at ~20 FPS, so the command flow is simple: one command buffer, submitted and waited for on
 *  every present and LFB read. Vulkan functions are loaded through SDL (no link dependency: without Vulkan,
 *  render.c falls back to OpenGL).
 *
 */

#if !defined(_WIN32) && defined(HAVE_VULKAN)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <SDL.h>
#include <SDL_vulkan.h>
#include "render_backend.h"
#include "config.h"
#include "vk_shaders.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int winapi_debug;

#define eprintf(...) fprintf(stderr,__VA_ARGS__)


/* ------------------------------------------------------------------ */
/* function loading                                                    */

#define VK_GLOBAL_FUNCTIONS \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceLayerProperties)

#define VK_INSTANCE_FUNCTIONS \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkDestroySurfaceKHR) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr)

#define VK_DEVICE_FUNCTIONS \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) \
    X(vkQueueSubmit) \
    X(vkDeviceWaitIdle) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkWaitForFences) \
    X(vkResetFences) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkGetImageMemoryRequirements) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkBindImageMemory) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkCreateRenderPass) \
    X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) \
    X(vkDestroyPipeline) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkFreeDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCreateSampler) \
    X(vkDestroySampler) \
    X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) \
    X(vkCmdSetViewport) \
    X(vkCmdSetScissor) \
    X(vkCmdDraw) \
    X(vkCmdClearAttachments) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdBlitImage)

static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
#define X(name) static PFN_##name p_##name;
VK_GLOBAL_FUNCTIONS
VK_INSTANCE_FUNCTIONS
VK_DEVICE_FUNCTIONS
#undef X


/* ------------------------------------------------------------------ */
/* state                                                               */

#define FRAME_RING_SIZE (8 * 1024 * 1024)   // vertices, uniform blocks and staging data between two submits
#define MAX_PIPELINES 256
#define MAX_SAMPLERS 64

// Glide uniform block (std140, shaders/vk_glide_ubo.glsl)
typedef struct {
    float screen[2], texscale[2];
    int32_t cc[4], ac[4], tc[4], inv[4];
    float constcolor[4];
    float chroma_color[4];
    float fog_color[4];
    int32_t textured, chroma, fog, depth_mode;
    float fog_table[64];
} glide_ubo;

// push constants of the quad shaders
typedef struct {
    int32_t keyed, fxaa;
    float sharpen, gamma;
    float texel[2];
} quad_push;

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    int w, h, levels;
    VkImageLayout layout;           // current layout (for textures: SHADER_READ_ONLY after the upload)
} vk_image;

// a texture and its descriptor sets (one per sampler)
typedef struct vk_texture {
    vk_image img;
    int used;
    struct { VkSampler sampler; VkDescriptorSet set; } sets[8];
    int num_sets;
} vk_texture;

static SDL_Window *window;
static VkInstance instance;
static VkSurfaceKHR surface;
static VkPhysicalDevice phys;
static VkPhysicalDeviceProperties phys_props;
static VkPhysicalDeviceMemoryProperties mem_props;
static VkDevice device;
static uint32_t queue_family;
static VkQueue queue;
static VkCommandPool cmd_pool;
static VkCommandBuffer cmd;
static int recording;
static VkFence fence;
static VkSemaphore sem_acquired, sem_rendered;    // sem_rendered: the one of the current swapchain image
static int anisotropy_ok;
static float max_anisotropy = 1.0f, anisotropy = 1.0f;
static int vsync;

// swapchain
static VkSwapchainKHR swapchain;
static VkFormat swap_format;
static VkExtent2D swap_extent;
static uint32_t swap_count;
static VkImage *swap_images;
static VkImageView *swap_views;
static VkFramebuffer *swap_fbs;
static VkSemaphore *swap_rendered;      // per swapchain image: a present may still wait for the previous one
static VkRenderPass present_pass;

// per-submit ring: host-visible buffer for vertices, uniform blocks and upload staging
static VkBuffer ring_buffer;
static VkDeviceMemory ring_memory;
static uint8_t *ring_ptr;
static VkDeviceSize ring_pos;

// readback buffer (LFB reads, frame dumps)
static VkBuffer read_buffer;
static VkDeviceMemory read_memory;
static uint8_t *read_ptr;
static VkDeviceSize read_size;

// pipelines
static VkDescriptorSetLayout set_layout_ubo, set_layout_tex;
static VkPipelineLayout pipeline_layout;
static VkDescriptorPool descriptor_pool;
static VkDescriptorSet ubo_set;
static VkShaderModule sm_quad_vert, sm_quad_frag, sm_post_frag, sm_glide_vert, sm_glide_frag;
static VkPipeline pipe_present_quad, pipe_present_post, pipe_lfb;
static struct { uint32_t key; VkPipeline pipe; } glide_pipes[MAX_PIPELINES];
static int num_glide_pipes;
static struct { uint32_t key; VkSampler sampler; } samplers[MAX_SAMPLERS];
static int num_samplers;
static VkSampler sampler_linear, sampler_nearest;
static VkBuffer quad_buffer;
static VkDeviceMemory quad_memory;

// textures (handle = index + 1)
static vk_texture *textures;
static int num_textures, max_textures;
static int *deferred_free;          // textures destroyed while the command buffer may still use them
static int num_deferred, max_deferred;
static vk_texture dummy_texture, tex_2d, tex_lfb;

// Glide
static int glide_open, glide_w, glide_h, target_w, target_h;
static VkSampleCountFlagBits msaa = VK_SAMPLE_COUNT_1_BIT;
static vk_image color[2], color_ms[2], depth;
static VkRenderPass glide_pass;
static VkFramebuffer glide_fb[2];
static int back_index;
static int pass_target = -1;        // Glide target of the active render pass (-1 = none)
static uint32_t last_glide_present;
static int post_fxaa;
static float post_sharpen, post_gamma = 1.0f;

static uint32_t last_ubo_offset = 0xFFFFFFFF;
static glide_ubo last_ubo;

// last presented picture for frame dumps
static uint32_t *last_2d;
static int last_2d_w, last_2d_h;
static uint32_t *last_window;
static int last_window_w, last_window_h;


/* ------------------------------------------------------------------ */
/* helpers                                                             */

static int check(VkResult r, const char *what)
{
    if (r == VK_SUCCESS) return 1;
    eprintf("render_vk: %s failed: %d\n", what, (int)r);
    return 0;
}

static int find_memory(uint32_t type_bits, VkMemoryPropertyFlags flags)
{
    uint32_t i;
    for (i = 0; i < mem_props.memoryTypeCount; i++)
    {
        if ((type_bits & (1u << i)) && ((mem_props.memoryTypes[i].propertyFlags & flags) == flags)) return (int)i;
    }
    return -1;
}

static int create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags, VkBuffer *buffer, VkDeviceMemory *memory)
{
    VkBufferCreateInfo bi;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    int type;

    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(p_vkCreateBuffer(device, &bi, NULL, buffer), "vkCreateBuffer")) return 0;
    p_vkGetBufferMemoryRequirements(device, *buffer, &req);
    type = find_memory(req.memoryTypeBits, flags);
    if (type < 0) return 0;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (!check(p_vkAllocateMemory(device, &ai, NULL, memory), "vkAllocateMemory")) return 0;
    return check(p_vkBindBufferMemory(device, *buffer, *memory, 0), "vkBindBufferMemory");
}

static void destroy_image(vk_image *img)
{
    if (img->view) p_vkDestroyImageView(device, img->view, NULL);
    if (img->image) p_vkDestroyImage(device, img->image, NULL);
    if (img->memory) p_vkFreeMemory(device, img->memory, NULL);
    memset(img, 0, sizeof(*img));
}

static int create_image(vk_image *img, int w, int h, int levels, VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples, VkImageAspectFlags aspect)
{
    VkImageCreateInfo ii;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkImageViewCreateInfo vi;
    int type;

    memset(img, 0, sizeof(*img));
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = (uint32_t)levels;
    ii.arrayLayers = 1;
    ii.samples = samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!check(p_vkCreateImage(device, &ii, NULL, &img->image), "vkCreateImage")) return 0;
    p_vkGetImageMemoryRequirements(device, img->image, &req);
    type = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) return 0;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (!check(p_vkAllocateMemory(device, &ai, NULL, &img->memory), "vkAllocateMemory")) return 0;
    if (!check(p_vkBindImageMemory(device, img->image, img->memory, 0), "vkBindImageMemory")) return 0;
    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = (uint32_t)levels;
    vi.subresourceRange.layerCount = 1;
    if (!check(p_vkCreateImageView(device, &vi, NULL, &img->view), "vkCreateImageView")) return 0;
    img->w = w;
    img->h = h;
    img->levels = levels;
    img->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return 1;
}

static void barrier(VkImage image, VkImageAspectFlags aspect, int base_level, int levels, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.baseMipLevel = (uint32_t)base_level;
    b.subresourceRange.levelCount = (uint32_t)levels;
    b.subresourceRange.layerCount = 1;
    p_vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

// color image layout changes (whole image)
static void color_layout(vk_image *img, VkImageLayout to)
{
    VkAccessFlags src_access = 0, dst_access = 0;
    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    if (img->layout == to) return;
    switch (img->layout)
    {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: src_access = VK_ACCESS_SHADER_READ_BIT; src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: src_access = VK_ACCESS_TRANSFER_READ_BIT; src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: src_access = VK_ACCESS_TRANSFER_WRITE_BIT; src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
        default: break;
    }
    switch (to)
    {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: dst_access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: dst_access = VK_ACCESS_SHADER_READ_BIT; dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: dst_access = VK_ACCESS_TRANSFER_READ_BIT; dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: dst_access = VK_ACCESS_TRANSFER_WRITE_BIT; dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
        default: break;
    }
    barrier(img->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, img->levels, img->layout, to, src_access, dst_access, src_stage, dst_stage);
    img->layout = to;
}

static void begin_commands(void)
{
    VkCommandBufferBeginInfo bi;
    if (recording) return;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_vkResetCommandBuffer(cmd, 0);
    p_vkBeginCommandBuffer(cmd, &bi);
    recording = 1;
}

static void end_pass(void)
{
    if (pass_target < 0) return;
    p_vkCmdEndRenderPass(cmd);
    // the render pass leaves the (resolved) color image in COLOR_ATTACHMENT_OPTIMAL
    pass_target = -1;
}

static void free_texture_now(int handle);

// submits the recorded commands (optionally with the swapchain semaphores) and waits for them
static void submit(int with_semaphores)
{
    VkSubmitInfo si;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    int i;

    if (!recording) return;
    end_pass();
    p_vkEndCommandBuffer(cmd);
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (with_semaphores)
    {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &sem_acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sem_rendered;
    }
    p_vkResetFences(device, 1, &fence);
    check(p_vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");
    p_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    recording = 0;
    ring_pos = 0;
    last_ubo_offset = 0xFFFFFFFF;
    for (i = 0; i < num_deferred; i++) free_texture_now(deferred_free[i]);
    num_deferred = 0;
}

// space in the ring buffer (submits when it's full)
static VkDeviceSize ring_alloc(VkDeviceSize size, VkDeviceSize align)
{
    VkDeviceSize pos = (ring_pos + align - 1) & ~(align - 1);
    if (pos + size > FRAME_RING_SIZE)
    {
        int target = pass_target;
        submit(0);
        begin_commands();
        (void) target;
        pos = 0;
        if (size > FRAME_RING_SIZE) return (VkDeviceSize)-1;
    }
    ring_pos = pos + size;
    return pos;
}


/* ------------------------------------------------------------------ */
/* textures                                                            */

// uploads pixels (w x h x 4 bytes) into level 0 and generates the mipmaps; commands are recorded outside of a pass
static int upload_image(vk_image *img, const void *pixels, int w, int h)
{
    VkDeviceSize size = (VkDeviceSize)w * h * 4, pos;
    VkBufferImageCopy copy;
    int l;

    begin_commands();
    end_pass();
    pos = ring_alloc(size, 16);
    if (pos == (VkDeviceSize)-1) return 0;
    memcpy(ring_ptr + pos, pixels, (size_t)size);

    barrier(img->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, img->levels, img->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    memset(&copy, 0, sizeof(copy));
    copy.bufferOffset = pos;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = (uint32_t)w;
    copy.imageExtent.height = (uint32_t)h;
    copy.imageExtent.depth = 1;
    p_vkCmdCopyBufferToImage(cmd, ring_buffer, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    for (l = 1; l < img->levels; l++)
    {
        VkImageBlit blit;
        barrier(img->image, VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        memset(&blit, 0, sizeof(blit));
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = (uint32_t)(l - 1);
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1].x = (w >> (l - 1)) > 1 ? (w >> (l - 1)) : 1;
        blit.srcOffsets[1].y = (h >> (l - 1)) > 1 ? (h >> (l - 1)) : 1;
        blit.srcOffsets[1].z = 1;
        blit.dstSubresource = blit.srcSubresource;
        blit.dstSubresource.mipLevel = (uint32_t)l;
        blit.dstOffsets[1].x = (w >> l) > 1 ? (w >> l) : 1;
        blit.dstOffsets[1].y = (h >> l) > 1 ? (h >> l) : 1;
        blit.dstOffsets[1].z = 1;
        p_vkCmdBlitImage(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        barrier(img->image, VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    barrier(img->image, VK_IMAGE_ASPECT_COLOR_BIT, img->levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    img->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return 1;
}

static int mip_levels(int w, int h)
{
    int l = 1, m = (w > h) ? w : h;
    while (m > 1) { m >>= 1; l++; }
    return l;
}

static VkDescriptorSet texture_set(vk_texture *t, VkSampler sampler)
{
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w;
    VkDescriptorSet set;
    int i;

    for (i = 0; i < t->num_sets; i++)
    {
        if (t->sets[i].sampler == sampler) return t->sets[i].set;
    }
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = descriptor_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout_tex;
    if (!check(p_vkAllocateDescriptorSets(device, &ai, &set), "vkAllocateDescriptorSets")) return VK_NULL_HANDLE;
    memset(&ii, 0, sizeof(ii));
    ii.sampler = sampler;
    ii.imageView = t->img.view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&w, 0, sizeof(w));
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    p_vkUpdateDescriptorSets(device, 1, &w, 0, NULL);
    if (t->num_sets < 8)
    {
        t->sets[t->num_sets].sampler = sampler;
        t->sets[t->num_sets].set = set;
        t->num_sets++;
    }
    return set;
}

static void release_texture(vk_texture *t)
{
    int i;
    for (i = 0; i < t->num_sets; i++) p_vkFreeDescriptorSets(device, descriptor_pool, 1, &t->sets[i].set);
    t->num_sets = 0;
    destroy_image(&t->img);
    t->used = 0;
}

static void free_texture_now(int handle)
{
    if ((handle <= 0) || (handle > num_textures)) return;
    release_texture(&textures[handle - 1]);
}

// dynamic textures (2D pictures, LFB writes): BGRA (XRGB/ARGB8888 little endian), recreated when the size changes
static int upload_dyn(vk_texture *t, int w, int h, const uint32_t *pixels)
{
    if ((t->img.image == VK_NULL_HANDLE) || (t->img.w != w) || (t->img.h != h))
    {
        if (t->img.image != VK_NULL_HANDLE)
        {
            submit(0);  // the old image may be in use
            release_texture(t);
        }
        if (!create_image(&t->img, w, h, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return 0;
        t->used = 1;
    }
    return upload_image(&t->img, pixels, w, h);
}


/* ------------------------------------------------------------------ */
/* pipelines                                                           */

static VkShaderModule shader_module(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci;
    VkShaderModule m = VK_NULL_HANDLE;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size;
    ci.pCode = code;
    check(p_vkCreateShaderModule(device, &ci, NULL, &m), "vkCreateShaderModule");
    return m;
}

static VkBlendFactor blend_factor(int f, int is_src, int is_alpha)
{
    switch (f)
    {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 2: return is_src ? (is_alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR) : (is_alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR);
        case 3: return VK_BLEND_FACTOR_DST_ALPHA;
        case 4: return VK_BLEND_FACTOR_ONE;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 6: return is_src ? (is_alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR) : (is_alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR);
        case 7: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 15: return is_src ? VK_BLEND_FACTOR_SRC_ALPHA_SATURATE : VK_BLEND_FACTOR_ONE;     // dst: PREFOG_COLOR (approximation)
        default: return is_src ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
    }
}

// key: bits 0-3 rgb src, 4-7 rgb dst, 8-11 alpha src, 12-15 alpha dst, 16 blend, 17 depth test, 18-20 depth func,
// 21 depth write, 22-23 topology (0 triangles, 1 lines, 2 points)
static VkPipeline create_pipeline(VkShaderModule vs, VkShaderModule fs, VkRenderPass pass, VkSampleCountFlagBits samples, uint32_t key, int glide_vertices)
{
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attrs[4];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vps;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy;
    VkGraphicsPipelineCreateInfo pi;
    VkPipeline pipe = VK_NULL_HANDLE;
    int i, topology = (key >> 22) & 3;

    memset(stages, 0, sizeof(stages));
    for (i = 0; i < 2; i++)
    {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;

    memset(&binding, 0, sizeof(binding));
    memset(attrs, 0, sizeof(attrs));
    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.pVertexAttributeDescriptions = attrs;
    if (glide_vertices)
    {
        // render_glide_vertex: x y z r g b ooz a oow sow tow tmu_oow
        binding.stride = sizeof(render_glide_vertex);
        for (i = 0; i < 4; i++)
        {
            attrs[i].location = (uint32_t)i;
            attrs[i].format = VK_FORMAT_R32G32B32_SFLOAT;
            attrs[i].offset = (uint32_t)(12 * i);
        }
        vi.vertexAttributeDescriptionCount = 4;
    }
    else
    {
        binding.stride = 8;
        attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
        vi.vertexAttributeDescriptionCount = 1;
    }

    memset(&ia, 0, sizeof(ia));
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = (topology == 0) ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : ((topology == 1) ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_POINT_LIST);

    memset(&vps, 0, sizeof(vps));
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount = 1;

    memset(&rs, 0, sizeof(rs));
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    memset(&ms, 0, sizeof(ms));
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = samples;

    memset(&ds, 0, sizeof(ds));
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = (key >> 17) & 1;
    ds.depthWriteEnable = (key >> 21) & 1;
    ds.depthCompareOp = (VkCompareOp)((key >> 18) & 7);     // GR_CMP_* order = VkCompareOp order

    memset(&cba, 0, sizeof(cba));
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if ((key >> 16) & 1)
    {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = blend_factor(key & 15, 1, 0);
        cba.dstColorBlendFactor = blend_factor((key >> 4) & 15, 0, 0);
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = blend_factor((key >> 8) & 15, 1, 1);
        cba.dstAlphaBlendFactor = blend_factor((key >> 12) & 15, 0, 1);
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    memset(&cb, 0, sizeof(cb));
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    memset(&dy, 0, sizeof(dy));
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vps;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = pipeline_layout;
    pi.renderPass = pass;
    check(p_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &pipe), "vkCreateGraphicsPipelines");
    return pipe;
}

static VkSampler create_sampler(VkFilter min, VkFilter mag, VkSamplerMipmapMode mip, int mipmaps, VkSamplerAddressMode u, VkSamplerAddressMode v, float aniso)
{
    VkSamplerCreateInfo si;
    VkSampler s = VK_NULL_HANDLE;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = mag;
    si.minFilter = min;
    si.mipmapMode = mip;
    si.addressModeU = u;
    si.addressModeV = v;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.anisotropyEnable = (aniso > 1.0f) ? VK_TRUE : VK_FALSE;
    si.maxAnisotropy = aniso;
    si.maxLod = mipmaps ? VK_LOD_CLAMP_NONE : 0.0f;
    check(p_vkCreateSampler(device, &si, NULL, &s), "vkCreateSampler");
    return s;
}

static VkSampler glide_sampler(const render_glide_state *st)
{
    int min_linear = (st->filter_min == 1), mag_linear = (st->filter_mag == 1);     // GR_TEXTUREFILTER_BILINEAR = 1
    int mipmaps = (st->mipmap != 0), aniso = anisotropy_ok && mipmaps && min_linear && (anisotropy > 1.0f);
    uint32_t key = min_linear | (mag_linear << 1) | (mipmaps << 2) | ((st->clamp_s == 1) << 3) | ((st->clamp_t == 1) << 4) | (aniso << 5);
    int i;

    for (i = 0; i < num_samplers; i++)
    {
        if (samplers[i].key == key) return samplers[i].sampler;
    }
    if (num_samplers == MAX_SAMPLERS) return sampler_linear;
    samplers[num_samplers].key = key;
    // mipmaps: like render_gl.c (GL_LINEAR_MIPMAP_LINEAR for bilinear, nearest level otherwise)
    samplers[num_samplers].sampler = create_sampler(min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                                    min_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST, mipmaps,
                                                    (st->clamp_s == 1) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                                    (st->clamp_t == 1) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                                    aniso ? anisotropy : 1.0f);
    return samplers[num_samplers++].sampler;
}

static VkPipeline glide_pipeline(const render_glide_state *st, int primitive)
{
    uint32_t key;
    int blend, depth_test, i;

    blend = !((st->rgb_src == 4) && (st->rgb_dst == 0) && (st->alpha_src == 4) && (st->alpha_dst == 0));
    depth_test = (st->depth_mode != 0);
    key = blend ? ((st->rgb_src & 15) | ((st->rgb_dst & 15) << 4) | ((st->alpha_src & 15) << 8) | ((st->alpha_dst & 15) << 12) | (1u << 16)) : 0;
    if (depth_test) key |= (1u << 17) | ((uint32_t)(st->depth_func & 7) << 18) | ((st->depth_mask ? 1u : 0u) << 21);
    key |= (uint32_t)(primitive & 3) << 22;
    for (i = 0; i < num_glide_pipes; i++)
    {
        if (glide_pipes[i].key == key) return glide_pipes[i].pipe;
    }
    if (num_glide_pipes == MAX_PIPELINES)
    {
        submit(0);
        for (i = 0; i < num_glide_pipes; i++) p_vkDestroyPipeline(device, glide_pipes[i].pipe, NULL);
        num_glide_pipes = 0;
        begin_commands();
    }
    glide_pipes[num_glide_pipes].key = key;
    glide_pipes[num_glide_pipes].pipe = create_pipeline(sm_glide_vert, sm_glide_frag, glide_pass, msaa, key, 1);
    return glide_pipes[num_glide_pipes++].pipe;
}


/* ------------------------------------------------------------------ */
/* swapchain                                                           */

static void destroy_swapchain(void)
{
    uint32_t i;
    for (i = 0; i < swap_count; i++)
    {
        if (swap_fbs) p_vkDestroyFramebuffer(device, swap_fbs[i], NULL);
        if (swap_views) p_vkDestroyImageView(device, swap_views[i], NULL);
        if (swap_rendered) p_vkDestroySemaphore(device, swap_rendered[i], NULL);
    }
    free(swap_rendered);
    swap_rendered = NULL;
    free(swap_fbs);
    free(swap_views);
    free(swap_images);
    swap_fbs = NULL;
    swap_views = NULL;
    swap_images = NULL;
    swap_count = 0;
    if (swapchain) p_vkDestroySwapchainKHR(device, swapchain, NULL);
    swapchain = VK_NULL_HANDLE;
}

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSwapchainCreateInfoKHR ci;
    VkPresentModeKHR modes[16], mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t num_modes = 16, i;
    int w, h;

    p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    SDL_Vulkan_GetDrawableSize(window, &w, &h);
    if (caps.currentExtent.width != 0xFFFFFFFF)
    {
        w = (int)caps.currentExtent.width;
        h = (int)caps.currentExtent.height;
    }
    if ((w <= 0) || (h <= 0)) return 0;     // minimized
    if (!vsync)
    {
        p_vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &num_modes, modes);
        for (i = 0; i < num_modes; i++)
        {
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
            else if ((modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) && (mode == VK_PRESENT_MODE_FIFO_KHR)) mode = modes[i];
        }
    }

    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface;
    ci.minImageCount = (caps.minImageCount < 2) ? 2 : caps.minImageCount;
    if ((caps.maxImageCount != 0) && (ci.minImageCount > caps.maxImageCount)) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = swap_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent.width = (uint32_t)w;
    ci.imageExtent.height = (uint32_t)h;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    if (!check(p_vkCreateSwapchainKHR(device, &ci, NULL, &swapchain), "vkCreateSwapchainKHR")) return 0;
    swap_extent = ci.imageExtent;

    p_vkGetSwapchainImagesKHR(device, swapchain, &swap_count, NULL);
    swap_images = (VkImage *)calloc(swap_count, sizeof(VkImage));
    swap_views = (VkImageView *)calloc(swap_count, sizeof(VkImageView));
    swap_fbs = (VkFramebuffer *)calloc(swap_count, sizeof(VkFramebuffer));
    swap_rendered = (VkSemaphore *)calloc(swap_count, sizeof(VkSemaphore));
    p_vkGetSwapchainImagesKHR(device, swapchain, &swap_count, swap_images);
    for (i = 0; i < swap_count; i++)
    {
        VkImageViewCreateInfo vi;
        VkFramebufferCreateInfo fi;
        VkSemaphoreCreateInfo sci;
        memset(&sci, 0, sizeof(sci));
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        p_vkCreateSemaphore(device, &sci, NULL, &swap_rendered[i]);
        memset(&vi, 0, sizeof(vi));
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = swap_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swap_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (!check(p_vkCreateImageView(device, &vi, NULL, &swap_views[i]), "vkCreateImageView")) return 0;
        memset(&fi, 0, sizeof(fi));
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = present_pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &swap_views[i];
        fi.width = swap_extent.width;
        fi.height = swap_extent.height;
        fi.layers = 1;
        if (!check(p_vkCreateFramebuffer(device, &fi, NULL, &swap_fbs[i]), "vkCreateFramebuffer")) return 0;
    }
    return 1;
}

static void vk_drawable_size(int *w, int *h)
{
    SDL_Vulkan_GetDrawableSize(window, w, h);
}


/* ------------------------------------------------------------------ */
/* init                                                                */

static uint32_t vk_window_flags(void)
{
    return SDL_WINDOW_VULKAN;
}

static int create_render_passes(void)
{
    VkAttachmentDescription a;
    VkAttachmentReference ref;
    VkSubpassDescription sub;
    VkSubpassDependency dep;
    VkRenderPassCreateInfo ci;

    // present pass: swapchain image, cleared, ends in PRESENT_SRC
    memset(&a, 0, sizeof(a));
    a.format = swap_format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    memset(&ref, 0, sizeof(ref));
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    memset(&dep, 0, sizeof(dep));
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments = &a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    return check(p_vkCreateRenderPass(device, &ci, NULL, &present_pass), "vkCreateRenderPass");
}

// Glide pass: color (+ multisampled color + resolve) and depth are loaded and stored (the pass is ended and
// restarted for uploads, LFB accesses and presents); all color images stay in COLOR_ATTACHMENT_OPTIMAL
static int create_glide_pass(void)
{
    VkAttachmentDescription a[3];
    VkAttachmentReference color_ref, depth_ref, resolve_ref;
    VkSubpassDescription sub;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo ci;
    int n = 0;

    memset(a, 0, sizeof(a));
    // 0: color (multisampled with MSAA)
    a[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    a[0].samples = msaa;
    a[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // 1: depth
    a[1] = a[0];
    a[1].format = VK_FORMAT_D32_SFLOAT;
    a[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    n = 2;
    if (msaa != VK_SAMPLE_COUNT_1_BIT)
    {
        // 2: resolve target (the single-sampled color image)
        a[2] = a[0];
        a[2].samples = VK_SAMPLE_COUNT_1_BIT;
        a[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        n = 3;
    }
    memset(&color_ref, 0, sizeof(color_ref));
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    depth_ref.attachment = 1;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    resolve_ref.attachment = 2;
    resolve_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;
    sub.pDepthStencilAttachment = &depth_ref;
    if (n == 3) sub.pResolveAttachments = &resolve_ref;
    memset(dep, 0, sizeof(dep));
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = (uint32_t)n;
    ci.pAttachments = a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 2;
    ci.pDependencies = dep;
    return check(p_vkCreateRenderPass(device, &ci, NULL, &glide_pass), "vkCreateRenderPass");
}

static int vk_init(SDL_Window *w)
{
    unsigned int num_ext = 0;
    const char **exts;
    VkApplicationInfo app;
    VkInstanceCreateInfo ici;
    VkPhysicalDevice devs[8];
    uint32_t num_devs = 8, i, num_fam = 0;
    VkQueueFamilyProperties fams[16];
    VkDeviceQueueCreateInfo qci;
    VkDeviceCreateInfo dci;
    VkPhysicalDeviceFeatures features, enable;
    const char *dev_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    const char *layers[1] = { "VK_LAYER_KHRONOS_validation" };
    float prio = 1.0f;
    VkSurfaceFormatKHR formats[32];
    uint32_t num_formats = 32;
    int found = 0, validation;
    const char *s;

    window = w;
    p_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr) SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (p_vkGetInstanceProcAddr == NULL)
    {
        eprintf("render_vk: no Vulkan loader: %s\n", SDL_GetError());
        return 0;
    }
#define X(name) p_##name = (PFN_##name) p_vkGetInstanceProcAddr(VK_NULL_HANDLE, #name); if (p_##name == NULL) return 0;
    VK_GLOBAL_FUNCTIONS
#undef X

    if (!SDL_Vulkan_GetInstanceExtensions(window, &num_ext, NULL)) return 0;
    exts = (const char **)calloc(num_ext, sizeof(char *));
    SDL_Vulkan_GetInstanceExtensions(window, &num_ext, exts);

    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "SR-I76";
    app.apiVersion = VK_API_VERSION_1_0;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = num_ext;
    ici.ppEnabledExtensionNames = exts;
    validation = (getenv("I76_VK_VALIDATION") != NULL);
    if (validation)
    {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = layers;
    }
    if (!check(p_vkCreateInstance(&ici, NULL, &instance), "vkCreateInstance"))
    {
        free(exts);
        return 0;
    }
    free(exts);
#define X(name) p_##name = (PFN_##name) p_vkGetInstanceProcAddr(instance, #name); if (p_##name == NULL) { eprintf("render_vk: missing %s\n", #name); return 0; }
    VK_INSTANCE_FUNCTIONS
#undef X

    if (!SDL_Vulkan_CreateSurface(window, instance, &surface))
    {
        eprintf("render_vk: SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return 0;
    }

    // a device with a queue that does graphics and presents (discrete GPUs first)
    p_vkEnumeratePhysicalDevices(instance, &num_devs, devs);
    for (int pass = 0; (pass < 2) && !found; pass++)
    {
        for (i = 0; (i < num_devs) && !found; i++)
        {
            uint32_t f;
            p_vkGetPhysicalDeviceProperties(devs[i], &phys_props);
            if ((pass == 0) && (phys_props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) continue;
            num_fam = 16;
            p_vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &num_fam, fams);
            for (f = 0; f < num_fam; f++)
            {
                VkBool32 present = VK_FALSE;
                p_vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], f, surface, &present);
                if ((fams[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
                {
                    phys = devs[i];
                    queue_family = f;
                    found = 1;
                    break;
                }
            }
        }
    }
    if (!found)
    {
        eprintf("render_vk: no suitable device\n");
        return 0;
    }
    p_vkGetPhysicalDeviceProperties(phys, &phys_props);
    p_vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
    p_vkGetPhysicalDeviceFeatures(phys, &features);

    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    memset(&enable, 0, sizeof(enable));
    anisotropy_ok = features.samplerAnisotropy;
    enable.samplerAnisotropy = features.samplerAnisotropy;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = &dev_ext;
    dci.pEnabledFeatures = &enable;
    if (!check(p_vkCreateDevice(phys, &dci, NULL, &device), "vkCreateDevice")) return 0;
#define X(name) p_##name = (PFN_##name) p_vkGetDeviceProcAddr(device, #name); if (p_##name == NULL) { eprintf("render_vk: missing %s\n", #name); return 0; }
    VK_DEVICE_FUNCTIONS
#undef X
    p_vkGetDeviceQueue(device, queue_family, 0, &queue);

    // swapchain format: BGRA8 (frame dumps read it as XRGB8888)
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &num_formats, formats);
    swap_format = formats[0].format;
    for (i = 0; i < num_formats; i++)
    {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) swap_format = formats[i].format;
    }
    if (swap_format == VK_FORMAT_UNDEFINED) swap_format = VK_FORMAT_B8G8R8A8_UNORM;

    s = config_get("vsync");
    vsync = (s != NULL) ? atoi(s) : 1;
    max_anisotropy = phys_props.limits.maxSamplerAnisotropy;
    anisotropy = (float)config_get_int("anisotropy", 8);
    if (anisotropy > max_anisotropy) anisotropy = max_anisotropy;
    if (anisotropy < 1.0f) anisotropy = 1.0f;
    render_post_settings(&post_fxaa, &post_sharpen, &post_gamma);

    {
        VkCommandPoolCreateInfo pci;
        VkCommandBufferAllocateInfo ai;
        VkFenceCreateInfo fci;
        VkSemaphoreCreateInfo sci;
        memset(&pci, 0, sizeof(pci));
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queue_family;
        if (!check(p_vkCreateCommandPool(device, &pci, NULL, &cmd_pool), "vkCreateCommandPool")) return 0;
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = cmd_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!check(p_vkAllocateCommandBuffers(device, &ai, &cmd), "vkAllocateCommandBuffers")) return 0;
        memset(&fci, 0, sizeof(fci));
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        p_vkCreateFence(device, &fci, NULL, &fence);
        memset(&sci, 0, sizeof(sci));
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        p_vkCreateSemaphore(device, &sci, NULL, &sem_acquired);
    }

    if (!create_render_passes() || !create_swapchain()) return 0;
    if (!create_buffer(FRAME_RING_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ring_buffer, &ring_memory)) return 0;
    p_vkMapMemory(device, ring_memory, 0, FRAME_RING_SIZE, 0, (void **)&ring_ptr);
    {
        static const float quad[12] = { 0,0, 1,0, 0,1, 1,0, 1,1, 0,1 };
        void *p;
        if (!create_buffer(sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &quad_buffer, &quad_memory)) return 0;
        p_vkMapMemory(device, quad_memory, 0, sizeof(quad), 0, &p);
        memcpy(p, quad, sizeof(quad));
        p_vkUnmapMemory(device, quad_memory);
    }

    // descriptors: set 0 = Glide uniform block (dynamic offset), set 1 = texture
    {
        VkDescriptorSetLayoutBinding b;
        VkDescriptorSetLayoutCreateInfo lci;
        VkDescriptorSetLayout layouts[2];
        VkPushConstantRange pcr;
        VkPipelineLayoutCreateInfo plci;
        VkDescriptorPoolSize sizes[2];
        VkDescriptorPoolCreateInfo dpci;
        VkDescriptorSetAllocateInfo ai;
        VkDescriptorBufferInfo bi;
        VkWriteDescriptorSet wr;

        memset(&b, 0, sizeof(b));
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        memset(&lci, 0, sizeof(lci));
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 1;
        lci.pBindings = &b;
        if (!check(p_vkCreateDescriptorSetLayout(device, &lci, NULL, &set_layout_ubo), "vkCreateDescriptorSetLayout")) return 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        if (!check(p_vkCreateDescriptorSetLayout(device, &lci, NULL, &set_layout_tex), "vkCreateDescriptorSetLayout")) return 0;
        layouts[0] = set_layout_ubo;
        layouts[1] = set_layout_tex;
        memset(&pcr, 0, sizeof(pcr));
        pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.size = sizeof(quad_push);
        memset(&plci, 0, sizeof(plci));
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 2;
        plci.pSetLayouts = layouts;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        if (!check(p_vkCreatePipelineLayout(device, &plci, NULL, &pipeline_layout), "vkCreatePipelineLayout")) return 0;

        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        sizes[0].descriptorCount = 4;
        sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = 16384;
        memset(&dpci, 0, sizeof(dpci));
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dpci.maxSets = 16384 + 4;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = sizes;
        if (!check(p_vkCreateDescriptorPool(device, &dpci, NULL, &descriptor_pool), "vkCreateDescriptorPool")) return 0;

        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descriptor_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_ubo;
        if (!check(p_vkAllocateDescriptorSets(device, &ai, &ubo_set), "vkAllocateDescriptorSets")) return 0;
        memset(&bi, 0, sizeof(bi));
        bi.buffer = ring_buffer;
        bi.range = sizeof(glide_ubo);
        memset(&wr, 0, sizeof(wr));
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = ubo_set;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        wr.pBufferInfo = &bi;
        p_vkUpdateDescriptorSets(device, 1, &wr, 0, NULL);
    }

    sm_quad_vert = shader_module(vk_quad_vert, sizeof(vk_quad_vert));
    sm_quad_frag = shader_module(vk_quad_frag, sizeof(vk_quad_frag));
    sm_post_frag = shader_module(vk_post_frag, sizeof(vk_post_frag));
    sm_glide_vert = shader_module(vk_glide_vert, sizeof(vk_glide_vert));
    sm_glide_frag = shader_module(vk_glide_frag, sizeof(vk_glide_frag));
    pipe_present_quad = create_pipeline(sm_quad_vert, sm_quad_frag, present_pass, VK_SAMPLE_COUNT_1_BIT, 0, 0);
    pipe_present_post = create_pipeline(sm_quad_vert, sm_post_frag, present_pass, VK_SAMPLE_COUNT_1_BIT, 0, 0);
    if (!pipe_present_quad || !pipe_present_post) return 0;

    sampler_linear = create_sampler(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, 0, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);
    sampler_nearest = create_sampler(VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, 0, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);

    // a white 1x1 texture for untextured draws (set 1 must be bound)
    {
        static const uint32_t white = 0xFFFFFFFF;
        if (!create_image(&dummy_texture.img, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return 0;
        dummy_texture.used = 1;
        upload_image(&dummy_texture.img, &white, 1, 1);
        submit(0);
    }

    if (winapi_debug) eprintf("render_vk: %s, Vulkan %u.%u%s\n", phys_props.deviceName, VK_VERSION_MAJOR(phys_props.apiVersion), VK_VERSION_MINOR(phys_props.apiVersion), validation ? " (validation)" : "");
    return 1;
}

static void vk_glide_close(void);

static void vk_shutdown(void)
{
    int i;
    if (device == VK_NULL_HANDLE) return;
    p_vkDeviceWaitIdle(device);
    vk_glide_close();
    for (i = 0; i < num_textures; i++) if (textures[i].used) release_texture(&textures[i]);
    release_texture(&tex_2d);
    release_texture(&tex_lfb);
    release_texture(&dummy_texture);
    for (i = 0; i < num_glide_pipes; i++) p_vkDestroyPipeline(device, glide_pipes[i].pipe, NULL);
    num_glide_pipes = 0;
    if (pipe_lfb) p_vkDestroyPipeline(device, pipe_lfb, NULL);
    pipe_lfb = VK_NULL_HANDLE;
    if (glide_pass) p_vkDestroyRenderPass(device, glide_pass, NULL);
    glide_pass = VK_NULL_HANDLE;
    for (i = 0; i < num_samplers; i++) p_vkDestroySampler(device, samplers[i].sampler, NULL);
    num_samplers = 0;
    p_vkDestroySampler(device, sampler_linear, NULL);
    p_vkDestroySampler(device, sampler_nearest, NULL);
    p_vkDestroyPipeline(device, pipe_present_quad, NULL);
    p_vkDestroyPipeline(device, pipe_present_post, NULL);
    p_vkDestroyShaderModule(device, sm_quad_vert, NULL);
    p_vkDestroyShaderModule(device, sm_quad_frag, NULL);
    p_vkDestroyShaderModule(device, sm_post_frag, NULL);
    p_vkDestroyShaderModule(device, sm_glide_vert, NULL);
    p_vkDestroyShaderModule(device, sm_glide_frag, NULL);
    p_vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    p_vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    p_vkDestroyDescriptorSetLayout(device, set_layout_ubo, NULL);
    p_vkDestroyDescriptorSetLayout(device, set_layout_tex, NULL);
    p_vkDestroyBuffer(device, ring_buffer, NULL);
    p_vkFreeMemory(device, ring_memory, NULL);
    p_vkDestroyBuffer(device, quad_buffer, NULL);
    p_vkFreeMemory(device, quad_memory, NULL);
    if (read_buffer) { p_vkDestroyBuffer(device, read_buffer, NULL); p_vkFreeMemory(device, read_memory, NULL); }
    destroy_swapchain();
    p_vkDestroyRenderPass(device, present_pass, NULL);
    p_vkDestroySemaphore(device, sem_acquired, NULL);
    p_vkDestroyFence(device, fence, NULL);
    p_vkDestroyCommandPool(device, cmd_pool, NULL);
    p_vkDestroyDevice(device, NULL);
    p_vkDestroySurfaceKHR(instance, surface, NULL);
    p_vkDestroyInstance(instance, NULL);
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
    window = NULL;
}


/* ------------------------------------------------------------------ */
/* present                                                             */

static int ensure_read_buffer(VkDeviceSize size)
{
    if (read_size >= size) return 1;
    if (read_buffer)
    {
        p_vkDestroyBuffer(device, read_buffer, NULL);
        p_vkFreeMemory(device, read_memory, NULL);
    }
    read_size = 0;
    if (!create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &read_buffer, &read_memory) &&
        !create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &read_buffer, &read_memory)) return 0;
    p_vkMapMemory(device, read_memory, 0, size, 0, (void **)&read_ptr);
    read_size = size;
    return 1;
}

static void set_viewport(float x, float y, float w, float h)
{
    VkViewport vp;
    VkRect2D sc;
    vp.x = x;
    vp.y = y;
    vp.width = w;
    vp.height = h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    p_vkCmdSetViewport(cmd, 0, 1, &vp);
    sc.offset.x = (int32_t)x;
    sc.offset.y = (int32_t)y;
    sc.extent.width = (uint32_t)w;
    sc.extent.height = (uint32_t)h;
    p_vkCmdSetScissor(cmd, 0, 1, &sc);
}

static void draw_quad(VkPipeline pipe, VkDescriptorSet tex_set, const quad_push *push)
{
    VkDeviceSize offset = 0;
    p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &tex_set, 0, NULL);
    p_vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(quad_push), push);
    p_vkCmdBindVertexBuffers(cmd, 0, 1, &quad_buffer, &offset);
    p_vkCmdDraw(cmd, 6, 1, 0, 0);
}

// draws a picture (an image in SHADER_READ_ONLY layout) letterboxed (w x h) into the window and presents it
static void present_picture(vk_image *img, VkDescriptorSet set, int w, int h, int post)
{
    uint32_t index;
    VkResult r;
    VkRenderPassBeginInfo rbi;
    VkClearValue clear;
    VkPresentInfoKHR pi;
    quad_push push;
    int vx, vy, vw, vh, dw, dh;
    int dump = (getenv("I76_DUMP_FRAMES") != NULL);

    SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
    if ((swapchain == VK_NULL_HANDLE) || ((uint32_t)dw != swap_extent.width) || ((uint32_t)dh != swap_extent.height))
    {
        submit(0);
        p_vkDeviceWaitIdle(device);
        destroy_swapchain();
        if (!create_swapchain()) return;
    }

    r = p_vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, sem_acquired, VK_NULL_HANDLE, &index);
    if ((r == VK_ERROR_OUT_OF_DATE_KHR) || (r == VK_ERROR_SURFACE_LOST_KHR))
    {
        submit(0);
        p_vkDeviceWaitIdle(device);
        destroy_swapchain();
        create_swapchain();
        return;
    }

    begin_commands();
    end_pass();
    sem_rendered = swap_rendered[index];

    memset(&clear, 0, sizeof(clear));
    clear.color.float32[3] = 1.0f;
    memset(&rbi, 0, sizeof(rbi));
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = present_pass;
    rbi.framebuffer = swap_fbs[index];
    rbi.renderArea.extent = swap_extent;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    p_vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    render_viewport(w, h, &vx, &vy, &vw, &vh);
    if ((vw > 0) && (vh > 0))
    {
        set_viewport((float)vx, (float)vy, (float)vw, (float)vh);
        memset(&push, 0, sizeof(push));
        push.fxaa = post_fxaa;
        push.sharpen = post_sharpen;
        push.gamma = post_gamma;
        push.texel[0] = 1.0f / (float)((img != NULL) ? img->w : w);
        push.texel[1] = 1.0f / (float)((img != NULL) ? img->h : h);
        draw_quad((post && (post_fxaa || (post_sharpen > 0.0f) || (post_gamma != 1.0f))) ? pipe_present_post : pipe_present_quad, set, &push);
    }
    p_vkCmdEndRenderPass(cmd);

    if (dump && ensure_read_buffer((VkDeviceSize)swap_extent.width * swap_extent.height * 4))
    {
        VkBufferImageCopy copy;
        barrier(swap_images[index], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        memset(&copy, 0, sizeof(copy));
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = swap_extent.width;
        copy.imageExtent.height = swap_extent.height;
        copy.imageExtent.depth = 1;
        p_vkCmdCopyImageToBuffer(cmd, swap_images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_buffer, 1, &copy);
        barrier(swap_images[index], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }

    submit(1);

    if (dump && (read_size >= (VkDeviceSize)swap_extent.width * swap_extent.height * 4))
    {
        int ww = (int)swap_extent.width, wh = (int)swap_extent.height;
        if ((last_window_w != ww) || (last_window_h != wh))
        {
            free(last_window);
            last_window = (uint32_t *)malloc((size_t)ww * wh * 4);
            last_window_w = ww;
            last_window_h = wh;
        }
        memcpy(last_window, read_ptr, (size_t)ww * wh * 4);     // BGRA8 = XRGB8888
    }

    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &sem_rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &index;
    r = p_vkQueuePresentKHR(queue, &pi);
    if ((r == VK_ERROR_OUT_OF_DATE_KHR) || (r == VK_SUBOPTIMAL_KHR))
    {
        p_vkDeviceWaitIdle(device);
        destroy_swapchain();
        create_swapchain();
    }
}

static void vk_present_2d(const uint32_t *pixels, int w, int h)
{
    if (device == VK_NULL_HANDLE) return;
    if (!upload_dyn(&tex_2d, w, h, pixels)) return;
    present_picture(&tex_2d.img, texture_set(&tex_2d, sampler_linear), w, h, 0);

    if ((last_2d_w != w) || (last_2d_h != h))
    {
        free(last_2d);
        last_2d = (uint32_t *)malloc((size_t)w * h * 4);
        last_2d_w = w;
        last_2d_h = h;
    }
    memcpy(last_2d, pixels, (size_t)w * h * 4);
}

static uint32_t *vk_read_last(int *w, int *h)
{
    uint32_t *p;
    if (glide_open && (last_window != NULL))
    {
        p = (uint32_t *)malloc((size_t)last_window_w * last_window_h * 4);
        memcpy(p, last_window, (size_t)last_window_w * last_window_h * 4);
        *w = last_window_w;
        *h = last_window_h;
        return p;
    }
    if (last_2d == NULL) return NULL;
    p = (uint32_t *)malloc((size_t)last_2d_w * last_2d_h * 4);
    memcpy(p, last_2d, (size_t)last_2d_w * last_2d_h * 4);
    *w = last_2d_w;
    *h = last_2d_h;
    return p;
}


/* ------------------------------------------------------------------ */
/* Glide                                                               */

static void destroy_targets(void)
{
    int i;
    for (i = 0; i < 2; i++)
    {
        if (glide_fb[i]) p_vkDestroyFramebuffer(device, glide_fb[i], NULL);
        glide_fb[i] = VK_NULL_HANDLE;
        destroy_image(&color[i]);
        destroy_image(&color_ms[i]);
    }
    destroy_image(&depth);
}

static void begin_pass(int target);

// the targets' descriptor sets (for presenting) are allocated per target and freed with it
static vk_texture target_tex[2];

static int create_targets(int fw, int fh)
{
    int i;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    for (i = 0; i < 2; i++)
    {
        VkImageView views[3];
        VkFramebufferCreateInfo fi;
        if (!create_image(&color[i], fw, fh, 1, VK_FORMAT_R8G8B8A8_UNORM, usage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return 0;
        if ((msaa != VK_SAMPLE_COUNT_1_BIT) &&
            !create_image(&color_ms[i], fw, fh, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, msaa, VK_IMAGE_ASPECT_COLOR_BIT)) return 0;
        if (i == 0)
        {
            if (!create_image(&depth, fw, fh, 1, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, msaa, VK_IMAGE_ASPECT_DEPTH_BIT)) return 0;
        }
        if (msaa != VK_SAMPLE_COUNT_1_BIT)
        {
            views[0] = color_ms[i].view;
            views[1] = depth.view;
            views[2] = color[i].view;
        }
        else
        {
            views[0] = color[i].view;
            views[1] = depth.view;
        }
        memset(&fi, 0, sizeof(fi));
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = glide_pass;
        fi.attachmentCount = (msaa != VK_SAMPLE_COUNT_1_BIT) ? 3 : 2;
        fi.pAttachments = views;
        fi.width = (uint32_t)fw;
        fi.height = (uint32_t)fh;
        fi.layers = 1;
        if (!check(p_vkCreateFramebuffer(device, &fi, NULL, &glide_fb[i]), "vkCreateFramebuffer")) return 0;
    }

    // everything to the layouts the Glide pass expects
    begin_commands();
    for (i = 0; i < 2; i++)
    {
        color_layout(&color[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (msaa != VK_SAMPLE_COUNT_1_BIT) color_layout(&color_ms[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    barrier(depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    target_w = fw;
    target_h = fh;
    for (i = 0; i < 2; i++)
    {
        memset(&target_tex[i], 0, sizeof(target_tex[i]));
        target_tex[i].img = color[i];
    }
    // initial contents: black, depth 1.0 (the pass ends with a resolve of the cleared multisampled image)
    for (i = 0; i < 2; i++)
    {
        VkClearAttachment att[2];
        VkClearRect rect;
        memset(att, 0, sizeof(att));
        att[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        att[1].clearValue.depthStencil.depth = 1.0f;
        memset(&rect, 0, sizeof(rect));
        rect.rect.extent.width = (uint32_t)fw;
        rect.rect.extent.height = (uint32_t)fh;
        rect.layerCount = 1;
        begin_pass(i);
        p_vkCmdClearAttachments(cmd, 2, att, 1, &rect);
    }
    end_pass();
    return 1;
}

static void free_target_sets(void)
{
    int i, k;
    for (i = 0; i < 2; i++)
    {
        for (k = 0; k < target_tex[i].num_sets; k++) p_vkFreeDescriptorSets(device, descriptor_pool, 1, &target_tex[i].sets[k].set);
        target_tex[i].num_sets = 0;
    }
}

// starts the Glide render pass on a target (0/1 = index into color[])
static void begin_pass(int target)
{
    VkRenderPassBeginInfo rbi;
    if (pass_target == target) return;
    begin_commands();
    end_pass();
    color_layout(&color[target], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    memset(&rbi, 0, sizeof(rbi));
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = glide_pass;
    rbi.framebuffer = glide_fb[target];
    rbi.renderArea.extent.width = (uint32_t)target_w;
    rbi.renderArea.extent.height = (uint32_t)target_h;
    p_vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    set_viewport(0.0f, 0.0f, (float)target_w, (float)target_h);
    pass_target = target;
}

static int vk_glide_open(int width, int height)
{
    int fw, fh, samples;
    VkSampleCountFlags supported;

    if (device == VK_NULL_HANDLE) return 0;
    if (glide_open) vk_glide_close();

    glide_w = width;
    glide_h = height;
    render_glide_target_size(width, height, &fw, &fh);

    samples = config_get_int("antialiasing", 4);
    supported = phys_props.limits.framebufferColorSampleCounts & phys_props.limits.framebufferDepthSampleCounts;
    msaa = VK_SAMPLE_COUNT_1_BIT;
    while (samples > 1)
    {
        if (supported & (VkSampleCountFlags)samples)
        {
            msaa = (VkSampleCountFlagBits)samples;
            break;
        }
        samples /= 2;
    }

    // pipelines depend on the sample count and the pass
    if (glide_pass != VK_NULL_HANDLE)
    {
        int i;
        for (i = 0; i < num_glide_pipes; i++) p_vkDestroyPipeline(device, glide_pipes[i].pipe, NULL);
        num_glide_pipes = 0;
        if (pipe_lfb) p_vkDestroyPipeline(device, pipe_lfb, NULL);
        pipe_lfb = VK_NULL_HANDLE;
        p_vkDestroyRenderPass(device, glide_pass, NULL);
        glide_pass = VK_NULL_HANDLE;
    }
    if (!create_glide_pass()) return 0;
    pipe_lfb = create_pipeline(sm_quad_vert, sm_quad_frag, glide_pass, msaa, 0, 0);
    if (!create_targets(fw, fh)) return 0;
    back_index = 0;
    glide_open = 1;
    last_ubo_offset = 0xFFFFFFFF;
    if (winapi_debug) eprintf("render_vk: Glide screen %dx%d, render target %dx%d, MSAA %d, anisotropy %.0f\n", width, height, fw, fh, (int)msaa, anisotropy_ok ? anisotropy : 1.0f);
    return 1;
}

static void vk_glide_close(void)
{
    if (!glide_open) return;
    submit(0);
    p_vkDeviceWaitIdle(device);
    free_target_sets();
    destroy_targets();
    glide_open = 0;
}

static int vk_glide_is_open(void)
{
    return glide_open;
}

static int vk_glide_texture_create(int w, int h, const uint32_t *rgba)
{
    vk_texture *t;
    int i;

    for (i = 0; i < num_textures; i++)
    {
        if (!textures[i].used)
        {
            int k, pending = 0;
            for (k = 0; k < num_deferred; k++) if (deferred_free[k] == i + 1) pending = 1;
            if (!pending) break;
        }
    }
    if (i == num_textures)
    {
        if (num_textures == max_textures)
        {
            max_textures = max_textures ? max_textures * 2 : 256;
            textures = (vk_texture *)realloc(textures, (size_t)max_textures * sizeof(vk_texture));
        }
        num_textures++;
    }
    t = &textures[i];
    memset(t, 0, sizeof(*t));
    if (!create_image(&t->img, w, h, mip_levels(w, h), VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return 0;
    t->used = 1;
    upload_image(&t->img, rgba, w, h);
    return i + 1;
}

static void vk_glide_texture_destroy(int texture)
{
    if ((texture <= 0) || (texture > num_textures) || !textures[texture - 1].used) return;
    if (!recording)
    {
        free_texture_now(texture);
        return;
    }
    // recorded commands may still use it: free it after the next submit
    if (num_deferred == max_deferred)
    {
        max_deferred = max_deferred ? max_deferred * 2 : 64;
        deferred_free = (int *)realloc(deferred_free, (size_t)max_deferred * sizeof(int));
    }
    deferred_free[num_deferred++] = texture;
    textures[texture - 1].used = 0;     // not reused before it's freed (see vk_glide_texture_create)
}

static void vk_glide_draw(const render_glide_state *st, const render_glide_vertex *vertices, int count, int primitive)
{
    glide_ubo u;
    VkDeviceSize vpos, bytes;
    uint32_t ubo_offset;
    VkPipeline pipe;
    VkDescriptorSet tex_set;
    vk_texture *t = &dummy_texture;
    VkSampler sampler;

    if (!glide_open || (count <= 0)) return;

    memset(&u, 0, sizeof(u));
    u.screen[0] = (float)glide_w;
    u.screen[1] = (float)glide_h;
    u.texscale[0] = st->s_scale;
    u.texscale[1] = st->t_scale;
    u.cc[0] = st->cc_function; u.cc[1] = st->cc_factor; u.cc[2] = st->cc_local; u.cc[3] = st->cc_other;
    u.ac[0] = st->ac_function; u.ac[1] = st->ac_factor; u.ac[2] = st->ac_local; u.ac[3] = st->ac_other;
    u.tc[0] = st->tc_rgb_function; u.tc[1] = st->tc_rgb_factor; u.tc[2] = st->tc_alpha_function; u.tc[3] = st->tc_alpha_factor;
    u.inv[0] = st->cc_invert; u.inv[1] = st->ac_invert; u.inv[2] = st->tc_rgb_invert; u.inv[3] = st->tc_alpha_invert;
    u.constcolor[0] = ((st->constant_color >> 16) & 0xFF) / 255.0f;
    u.constcolor[1] = ((st->constant_color >> 8) & 0xFF) / 255.0f;
    u.constcolor[2] = (st->constant_color & 0xFF) / 255.0f;
    u.constcolor[3] = (st->constant_color >> 24) / 255.0f;
    u.chroma_color[0] = (float)((st->chromakey_value >> 16) & 0xFF);
    u.chroma_color[1] = (float)((st->chromakey_value >> 8) & 0xFF);
    u.chroma_color[2] = (float)(st->chromakey_value & 0xFF);
    u.fog_color[0] = ((st->fog_color >> 16) & 0xFF) / 255.0f;
    u.fog_color[1] = ((st->fog_color >> 8) & 0xFF) / 255.0f;
    u.fog_color[2] = (st->fog_color & 0xFF) / 255.0f;
    u.textured = (st->texture != 0);
    u.chroma = st->chromakey_enable;
    u.fog = st->fog_mode;
    u.depth_mode = st->depth_mode;
    memcpy(u.fog_table, st->fog_table, sizeof(u.fog_table));

    // pipeline and sampler creation may submit: before anything is recorded for this draw
    pipe = glide_pipeline(st, primitive);
    if ((st->texture > 0) && (st->texture <= num_textures) && textures[st->texture - 1].used) t = &textures[st->texture - 1];
    sampler = (t == &dummy_texture) ? sampler_nearest : glide_sampler(st);

    begin_pass(back_index);

    if ((last_ubo_offset == 0xFFFFFFFF) || (memcmp(&u, &last_ubo, sizeof(u)) != 0))
    {
        VkDeviceSize pos = ring_alloc(sizeof(glide_ubo), phys_props.limits.minUniformBufferOffsetAlignment);
        if (pos == (VkDeviceSize)-1) return;
        if (pass_target != back_index) begin_pass(back_index);     // ring_alloc may have submitted
        memcpy(ring_ptr + pos, &u, sizeof(u));
        last_ubo = u;
        last_ubo_offset = (uint32_t)pos;
    }
    ubo_offset = last_ubo_offset;

    bytes = (VkDeviceSize)count * sizeof(render_glide_vertex);
    vpos = ring_alloc(bytes, 16);
    if (vpos == (VkDeviceSize)-1) return;
    if (pass_target != back_index)
    {
        // the ring was full: submitted, the uniform block has to be written again
        VkDeviceSize pos;
        begin_pass(back_index);
        pos = ring_alloc(sizeof(glide_ubo), phys_props.limits.minUniformBufferOffsetAlignment);
        memcpy(ring_ptr + pos, &u, sizeof(u));
        last_ubo = u;
        last_ubo_offset = ubo_offset = (uint32_t)pos;
        vpos = ring_alloc(bytes, 16);
    }
    memcpy(ring_ptr + vpos, vertices, (size_t)bytes);

    tex_set = texture_set(t, sampler);
    p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &ubo_set, 1, &ubo_offset);
    p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &tex_set, 0, NULL);
    p_vkCmdBindVertexBuffers(cmd, 0, 1, &ring_buffer, &vpos);
    p_vkCmdDraw(cmd, (uint32_t)count, 1, 0, 0);
}

static void vk_glide_clear(uint32_t color_value, uint8_t alpha, uint16_t depth_value, int color_mask, int depth_mask)
{
    VkClearAttachment att[2];
    VkClearRect rect;
    uint32_t n = 0;

    if (!glide_open || (!color_mask && !depth_mask)) return;
    begin_pass(back_index);
    memset(att, 0, sizeof(att));
    if (color_mask)
    {
        att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att[n].colorAttachment = 0;
        att[n].clearValue.color.float32[0] = ((color_value >> 16) & 0xFF) / 255.0f;
        att[n].clearValue.color.float32[1] = ((color_value >> 8) & 0xFF) / 255.0f;
        att[n].clearValue.color.float32[2] = (color_value & 0xFF) / 255.0f;
        att[n].clearValue.color.float32[3] = alpha / 255.0f;
        n++;
    }
    if (depth_mask)
    {
        att[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        att[n].clearValue.depthStencil.depth = depth_value / 65535.0f;
        n++;
    }
    memset(&rect, 0, sizeof(rect));
    rect.rect.extent.width = (uint32_t)target_w;
    rect.rect.extent.height = (uint32_t)target_h;
    rect.layerCount = 1;
    p_vkCmdClearAttachments(cmd, n, att, 1, &rect);
}

// presents the Glide front buffer (resolved: the pass ended) in the window
static void present_front(void)
{
    int front = back_index ^ 1;
    VkDescriptorSet set;

    last_glide_present = SDL_GetTicks();
    begin_commands();
    end_pass();
    color_layout(&color[front], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    target_tex[front].img = color[front];
    set = texture_set(&target_tex[front], sampler_linear);
    present_picture(&color[front], set, glide_w, glide_h, 1);
    begin_commands();
    color_layout(&color[front], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

// glide_scale = auto: new render targets when the window size changed (after a swap, keeping the picture)
static void check_target_size(void)
{
    int fw, fh, front = back_index ^ 1, ow = target_w, oh = target_h;
    vk_image old;
    VkImageBlit blit;

    if (!render_glide_target_auto()) return;
    render_glide_target_size(glide_w, glide_h, &fw, &fh);
    if ((fw == target_w) && (fh == target_h)) return;

    submit(0);
    p_vkDeviceWaitIdle(device);
    old = color[front];
    memset(&color[front], 0, sizeof(vk_image));
    free_target_sets();
    destroy_targets();
    if (!create_targets(fw, fh))
    {
        destroy_image(&old);
        return;
    }
    // copy the old front picture
    begin_commands();
    color_layout(&old, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    color_layout(&color[front], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&blit, 0, sizeof(blit));
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = ow;
    blit.srcOffsets[1].y = oh;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1].x = fw;
    blit.dstOffsets[1].y = fh;
    blit.dstOffsets[1].z = 1;
    p_vkCmdBlitImage(cmd, old.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color[front].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    color_layout(&color[front], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    submit(0);
    destroy_image(&old);
    if (winapi_debug) eprintf("render_vk: render target %dx%d\n", fw, fh);
}

static void vk_glide_swap(void)
{
    if (!glide_open) return;
    end_pass();         // resolves the back buffer
    back_index ^= 1;
    present_front();
    check_target_size();
}

static void vk_glide_refresh(int force)
{
    if (!glide_open) return;
    if (force || (SDL_GetTicks() - last_glide_present >= 33)) present_front();
}

static void vk_glide_read_565(int buffer, uint16_t *dst, int stride_pixels)
{
    int target, x, y;
    VkBufferImageCopy copy;
    const uint8_t *tmp;

    if (!glide_open) return;
    target = (buffer == 1) ? back_index : (back_index ^ 1);
    if (!ensure_read_buffer((VkDeviceSize)target_w * target_h * 4)) return;
    begin_commands();
    end_pass();         // resolves
    color_layout(&color[target], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = (uint32_t)target_w;
    copy.imageExtent.height = (uint32_t)target_h;
    copy.imageExtent.depth = 1;
    p_vkCmdCopyImageToBuffer(cmd, color[target].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_buffer, 1, &copy);
    color_layout(&color[target], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    submit(0);

    tmp = read_ptr;
    for (y = 0; y < glide_h; y++)
    {
        const uint8_t *row = tmp + (size_t)((int64_t)y * target_h / glide_h) * target_w * 4;
        for (x = 0; x < glide_w; x++)
        {
            const uint8_t *p = row + (size_t)((int64_t)x * target_w / glide_w) * 4;     // R, G, B, A
            dst[(size_t)y * stride_pixels + x] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
}

static void vk_glide_write_argb(int buffer, const uint32_t *src)
{
    int target;
    quad_push push;

    if (!glide_open) return;
    target = (buffer == 1) ? back_index : (back_index ^ 1);
    if (!upload_dyn(&tex_lfb, glide_w, glide_h, src)) return;
    begin_pass(target);
    memset(&push, 0, sizeof(push));
    push.keyed = 1;
    draw_quad(pipe_lfb, texture_set(&tex_lfb, sampler_nearest), &push);
    if (target != back_index) end_pass();     // the front buffer is presented from the resolved image
}

const render_backend render_backend_vk = {
    "Vulkan",
    vk_window_flags, vk_init, vk_shutdown, vk_drawable_size, vk_present_2d, vk_read_last,
    vk_glide_open, vk_glide_close, vk_glide_is_open, vk_glide_texture_create, vk_glide_texture_destroy,
    vk_glide_draw, vk_glide_clear, vk_glide_swap, vk_glide_refresh, vk_glide_read_565, vk_glide_write_argb
};

#ifdef __cplusplus
}
#endif

#endif
