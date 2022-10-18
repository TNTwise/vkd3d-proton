/*
 * Copyright 2022 Hans-Kristian Arntzen for Valve Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API

#include "vkd3d_win32.h"
#include "vkd3d_private.h"

static inline struct dxgi_vk_swap_chain_factory *impl_from_IDXGIVkSwapChainFactory(IDXGIVkSwapChainFactory *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_vk_swap_chain_factory, IDXGIVkSwapChainFactory_iface);
}

static ULONG STDMETHODCALLTYPE dxgi_vk_swap_chain_factory_AddRef(IDXGIVkSwapChainFactory *iface)
{
    struct dxgi_vk_swap_chain_factory *chain = impl_from_IDXGIVkSwapChainFactory(iface);
    return ID3D12CommandQueue_AddRef(&chain->queue->ID3D12CommandQueue_iface);
}

static ULONG STDMETHODCALLTYPE dxgi_vk_swap_chain_factory_Release(IDXGIVkSwapChainFactory *iface)
{
    struct dxgi_vk_swap_chain_factory *chain = impl_from_IDXGIVkSwapChainFactory(iface);
    return ID3D12CommandQueue_Release(&chain->queue->ID3D12CommandQueue_iface);
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_factory_QueryInterface(IDXGIVkSwapChainFactory *iface, REFIID riid, void **object)
{
    struct dxgi_vk_swap_chain_factory *chain = impl_from_IDXGIVkSwapChainFactory(iface);
    return ID3D12CommandQueue_QueryInterface(&chain->queue->ID3D12CommandQueue_iface, riid, object);
}

struct dxgi_vk_swap_chain_present_request
{
    uint32_t user_index;
    DXGI_FORMAT dxgi_format;
    DXGI_COLOR_SPACE_TYPE dxgi_color_space_type;
    DXGI_VK_HDR_METADATA dxgi_hdr_metadata;
    uint32_t swap_interval;
    bool modifies_hdr_metadata;
};

struct dxgi_vk_swap_chain
{
    IDXGIVkSwapChain IDXGIVkSwapChain_iface;
    struct d3d12_command_queue *queue;

    LONG refcount;
    DXGI_SWAP_CHAIN_DESC1 desc;

    HANDLE frame_latency_event;
    UINT frame_latency;
    VkSurfaceKHR vk_surface;

    struct
    {
        /* When resizing user buffers, we need to make sure all pending blits have completed on GPU. */
        VkSemaphore vk_blit_semaphore;
        uint64_t blit_count;

        /* PresentID or frame latency fence is used depending on features and if we're really presenting on-screen. */
        ID3D12Fence1 *frame_latency_fence;
        uint64_t frame_latency_count;
        uint64_t present_id;
        bool present_id_valid;

        /* Atomically updated after a PRESENT queue command has processed.
         * We don't care about wrap around.
         * We just care about equality check so we can atomically check if all outstanding present events have completed on CPU timeline.
         * This is used to implement occlusion check. */
        uint32_t present_count;

        /* For blits. Use simple VkFences since we have to use binary semaphores with WSI release anyways.
         * We don't need to wait on these fences on main thread. */
        VkCommandPool vk_blit_command_pool;
        VkCommandBuffer vk_blit_command_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        VkFence vk_blit_fences[DXGI_MAX_SWAP_CHAIN_BUFFERS];

        VkSwapchainKHR vk_swapchain;
        VkImage vk_backbuffer_images[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        VkImageView vk_backbuffer_image_views[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        VkSemaphore vk_release_semaphores[DXGI_MAX_SWAP_CHAIN_BUFFERS];

        /* Since we're presenting in a thread, there's no particular reason to use WSI acquire semaphores.
         * Removes a lot of edge cases. */
        VkFence vk_acquire_fence;
        uint32_t backbuffer_width;
        uint32_t backbuffer_height;
        uint32_t backbuffer_count;
        VkFormat backbuffer_format;

        struct vkd3d_swapchain_info pipeline;

        uint32_t is_occlusion_state; /* Updated atomically. */

        /* State tracking in present tasks on how to deal with swapchain recreation. */
        bool force_swapchain_recreation;
        bool is_surface_lost;
    } present;

    struct dxgi_vk_swap_chain_present_request request, request_ring[DXGI_MAX_SWAP_CHAIN_BUFFERS];

    struct
    {
        struct d3d12_resource *backbuffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        VkImageView vk_image_views[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        uint64_t blit_count;
        uint32_t present_count;
        UINT index;

        DXGI_COLOR_SPACE_TYPE dxgi_color_space_type;
        DXGI_VK_HDR_METADATA dxgi_hdr_metadata;
        bool modifies_hdr_metadata;
    } user;

    struct
    {
        VkSurfaceFormatKHR *formats;
        uint32_t format_count;
    } properties;

    /* If present_wait is supported. */
    struct
    {
        pthread_t thread;
        uint64_t *wait_queue;
        size_t wait_queue_size;
        size_t wait_queue_count;
        pthread_cond_t cond;
        pthread_mutex_t lock;
        bool active;
    } wait_thread;
};

static void dxgi_vk_swap_chain_drain_queue(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkQueue vk_queue;

    /* Full wait-idle. */
    vk_queue = vkd3d_acquire_vk_queue(&chain->queue->ID3D12CommandQueue_iface);
    if (vk_queue)
    {
        VK_CALL(vkQueueWaitIdle(vk_queue));
        vkd3d_release_vk_queue(&chain->queue->ID3D12CommandQueue_iface);
    }
    else
        ERR("Failed to acquire queue.\n");
}

static void dxgi_vk_swap_chain_drain_user_images(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkSemaphoreWaitInfoKHR wait_info;
    VkResult vr;

    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR;
    wait_info.pNext = NULL;
    wait_info.flags = 0;
    wait_info.pSemaphores = &chain->present.vk_blit_semaphore;
    wait_info.pValues = &chain->user.blit_count;
    wait_info.semaphoreCount = 1;
    vr = VK_CALL(vkWaitSemaphoresKHR(chain->queue->device->vk_device, &wait_info, UINT64_MAX));
    if (vr)
        ERR("Failed to wait for present semaphore, vr %d.\n", vr);
}

static void dxgi_vk_swap_chain_push_present_id(struct dxgi_vk_swap_chain *chain, uint64_t present_id)
{
    pthread_mutex_lock(&chain->wait_thread.lock);
    vkd3d_array_reserve((void **)&chain->wait_thread.wait_queue, &chain->wait_thread.wait_queue_size,
            chain->wait_thread.wait_queue_count + 1, sizeof(*chain->wait_thread.wait_queue));
    chain->wait_thread.wait_queue[chain->wait_thread.wait_queue_count++] = present_id;
    pthread_cond_signal(&chain->wait_thread.cond);
    pthread_mutex_unlock(&chain->wait_thread.lock);
}

static void dxgi_vk_swap_chain_cleanup(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    UINT i;

    if (chain->wait_thread.active)
    {
        dxgi_vk_swap_chain_push_present_id(chain, 0);
        pthread_join(chain->wait_thread.thread, NULL);
        pthread_mutex_destroy(&chain->wait_thread.lock);
        pthread_cond_destroy(&chain->wait_thread.cond);
    }
    vkd3d_free(chain->wait_thread.wait_queue);

    if (chain->present.frame_latency_fence)
        ID3D12Fence1_Release(chain->present.frame_latency_fence);
    if (chain->frame_latency_event)
        CloseHandle(chain->frame_latency_event);

    VK_CALL(vkDestroySemaphore(chain->queue->device->vk_device, chain->present.vk_blit_semaphore, NULL));
    VK_CALL(vkDestroyCommandPool(chain->queue->device->vk_device, chain->present.vk_blit_command_pool, NULL));
    for (i = 0; i < ARRAY_SIZE(chain->present.vk_release_semaphores); i++)
        VK_CALL(vkDestroySemaphore(chain->queue->device->vk_device, chain->present.vk_release_semaphores[i], NULL));
    for (i = 0; i < ARRAY_SIZE(chain->present.vk_backbuffer_image_views); i++)
        VK_CALL(vkDestroyImageView(chain->queue->device->vk_device, chain->present.vk_backbuffer_image_views[i], NULL));
    VK_CALL(vkDestroyFence(chain->queue->device->vk_device, chain->present.vk_acquire_fence, NULL));
    for (i = 0; i < ARRAY_SIZE(chain->present.vk_blit_fences); i++)
        VK_CALL(vkDestroyFence(chain->queue->device->vk_device, chain->present.vk_blit_fences[i], NULL));

    VK_CALL(vkDestroySwapchainKHR(chain->queue->device->vk_device, chain->present.vk_swapchain, NULL));

    for (i = 0; i < ARRAY_SIZE(chain->user.backbuffers); i++)
    {
        if (chain->user.backbuffers[i])
            vkd3d_resource_decref((ID3D12Resource *)&chain->user.backbuffers[i]->ID3D12Resource_iface);
        VK_CALL(vkDestroyImageView(chain->queue->device->vk_device, chain->user.vk_image_views[i], NULL));
    }

    vkd3d_free(chain->properties.formats);

    VK_CALL(vkDestroySurfaceKHR(chain->queue->device->vkd3d_instance->vk_instance,
            chain->vk_surface, NULL));
}

static inline struct dxgi_vk_swap_chain *impl_from_IDXGIVkSwapChain(IDXGIVkSwapChain *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_vk_swap_chain, IDXGIVkSwapChain_iface);
}

static ULONG STDMETHODCALLTYPE dxgi_vk_swap_chain_AddRef(IDXGIVkSwapChain *iface)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    return InterlockedIncrement(&chain->refcount);
}

static ULONG STDMETHODCALLTYPE dxgi_vk_swap_chain_Release(IDXGIVkSwapChain *iface)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    struct d3d12_command_queue *queue = chain->queue;
    ULONG refcount;

    refcount = InterlockedDecrement(&chain->refcount);
    if (!refcount)
    {
        dxgi_vk_swap_chain_drain_queue(chain);
        dxgi_vk_swap_chain_cleanup(chain);
        vkd3d_free(chain);
        ID3D12CommandQueue_Release(&queue->ID3D12CommandQueue_iface);
    }
    return refcount;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_QueryInterface(IDXGIVkSwapChain *iface, REFIID riid, void **object)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDXGIVkSwapChain))
    {
        dxgi_vk_swap_chain_AddRef(&chain->IDXGIVkSwapChain_iface);
        *object = iface;
        return S_OK;
    }

    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_GetDesc(IDXGIVkSwapChain *iface, DXGI_SWAP_CHAIN_DESC1 *pDesc)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    *pDesc = chain->desc;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_GetAdapter(IDXGIVkSwapChain *iface, REFIID riid, void **object)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    return IUnknown_QueryInterface(chain->queue->device->parent, riid, object);
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_GetDevice(IDXGIVkSwapChain *iface, REFIID riid, void **object)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    return ID3D12Device9_QueryInterface(&chain->queue->device->ID3D12Device_iface, riid, object);
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_GetImage(IDXGIVkSwapChain *iface, UINT BufferId, REFIID riid, void **object)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    if (BufferId >= chain->desc.BufferCount)
        return E_INVALIDARG;
    return ID3D12Resource2_QueryInterface(&chain->user.backbuffers[BufferId]->ID3D12Resource_iface, riid, object);
}

static UINT STDMETHODCALLTYPE dxgi_vk_swap_chain_GetImageIndex(IDXGIVkSwapChain *iface)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    return chain->user.index;
}

static UINT STDMETHODCALLTYPE dxgi_vk_swap_chain_GetFrameLatency(IDXGIVkSwapChain *iface)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    return chain->frame_latency;
}

static HANDLE STDMETHODCALLTYPE dxgi_vk_swap_chain_GetFrameLatencyEvent(IDXGIVkSwapChain *iface)
{
    struct dxgi_vk_swap_chain *swapchain = impl_from_IDXGIVkSwapChain(iface);
    HANDLE duplicated_handle;

    TRACE("iface %p.\n", iface);

    if (!DuplicateHandle(GetCurrentProcess(), swapchain->frame_latency_event,
                GetCurrentProcess(), &duplicated_handle,
                0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        ERR("Failed to duplicate waitable handle.\n");
        return INVALID_HANDLE_VALUE;
    }

    return duplicated_handle;
}

static HRESULT dxgi_vk_swap_chain_allocate_user_buffer(struct dxgi_vk_swap_chain *chain,
        const DXGI_SWAP_CHAIN_DESC1 *pDesc, struct d3d12_resource **ppResource)
{
    struct d3d12_device *device = chain->queue->device;
    D3D12_RESOURCE_DESC1 resource_desc;
    D3D12_HEAP_PROPERTIES heap_props;

    memset(&resource_desc, 0, sizeof(resource_desc));
    memset(&heap_props, 0, sizeof(heap_props));

    resource_desc.Width = pDesc->Width;
    resource_desc.Height = pDesc->Height;
    resource_desc.Format = pDesc->Format;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.MipLevels = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_props.CreationNodeMask = 1;
    heap_props.VisibleNodeMask = 1;

    return d3d12_resource_create_committed(device, &resource_desc, &heap_props, D3D12_HEAP_FLAG_NONE,
            D3D12_RESOURCE_STATE_PRESENT, NULL, NULL, ppResource);
}

static HRESULT dxgi_vk_swap_chain_reallocate_user_buffers(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    struct d3d12_resource *old_resources[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    VkImageViewCreateInfo view_info;
    unsigned int i;
    VkResult vr;
    HRESULT hr;

    if (chain->desc.BufferCount > DXGI_MAX_SWAP_CHAIN_BUFFERS)
        return E_INVALIDARG;

    for (i = 0; i < DXGI_MAX_SWAP_CHAIN_BUFFERS; i++)
    {
        old_resources[i] = chain->user.backbuffers[i];
        chain->user.backbuffers[i] = NULL;
        VK_CALL(vkDestroyImageView(chain->queue->device->vk_device, chain->user.vk_image_views[i], NULL));
        chain->user.vk_image_views[i] = VK_NULL_HANDLE;
    }

    memset(&view_info, 0, sizeof(view_info));
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

    for (i = 0; i < chain->desc.BufferCount; i++)
    {
        if (FAILED(hr = dxgi_vk_swap_chain_allocate_user_buffer(chain, &chain->desc, &chain->user.backbuffers[i])))
            goto err;

        /* We need to hold a private reference to the resource, not a public one. */
        vkd3d_resource_incref((ID3D12Resource *)&chain->user.backbuffers[i]->ID3D12Resource_iface);
        ID3D12Resource2_Release(&chain->user.backbuffers[i]->ID3D12Resource_iface);

        view_info.format = chain->user.backbuffers[i]->format->vk_format;
        view_info.image = chain->user.backbuffers[i]->res.vk_image;
        vr = VK_CALL(vkCreateImageView(chain->queue->device->vk_device, &view_info, NULL, &chain->user.vk_image_views[i]));
        if (vr < 0)
        {
            hr = E_OUTOFMEMORY;
            goto err;
        }
    }

    for (i = 0; i < DXGI_MAX_SWAP_CHAIN_BUFFERS; i++)
        if (old_resources[i])
            vkd3d_resource_decref((ID3D12Resource *)&old_resources[i]->ID3D12Resource_iface);

    return S_OK;

err:
    for (i = 0; i < DXGI_MAX_SWAP_CHAIN_BUFFERS; i++)
    {
        if (chain->user.backbuffers[i])
            vkd3d_resource_decref((ID3D12Resource *)&chain->user.backbuffers[i]->ID3D12Resource_iface);
        chain->user.backbuffers[i] = old_resources[i];
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_ChangeProperties(IDXGIVkSwapChain *iface, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
        const UINT *pNodeMasks, IUnknown *const *ppPresentQueues)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    DXGI_SWAP_CHAIN_DESC1 old_desc = chain->desc;
    HRESULT hr;
    UINT i;

    /* TODO: Validate pNodeMasks and ppPresentQueues. */

    /* Public ref-counts must be 0 for this to be allowed. */
    for (i = 0; i < chain->desc.BufferCount; i++)
        if (chain->user.backbuffers[i]->refcount != 0)
            return DXGI_ERROR_INVALID_CALL;

    chain->desc = *pDesc;

    /* Don't do anything in this case. */
    if (old_desc.Width == chain->desc.Width &&
            old_desc.Height == chain->desc.Height &&
            old_desc.BufferCount == chain->desc.BufferCount &&
            old_desc.Format == chain->desc.Format &&
            old_desc.Flags == chain->desc.Flags)
    {
        return S_OK;
    }

    /* Waits for any outstanding present event to complete, including the work it takes to blit to screen. */
    dxgi_vk_swap_chain_drain_user_images(chain);

    if (FAILED(hr = dxgi_vk_swap_chain_reallocate_user_buffers(chain)))
    {
        chain->desc = old_desc;
        return hr;
    }

    if (chain->user.index >= chain->desc.BufferCount)
    {
        /* Need to reset the user index in case the buffer count is lowered.
         * It is unclear if we're allowed to always reset, but employ principle of least surprise. */
        chain->user.index = 0;
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_SetPresentRegion(IDXGIVkSwapChain *iface, const RECT *pRegion)
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_SetGammaControl(IDXGIVkSwapChain *iface, UINT NumControlPoints, const DXGI_RGB *pControlPoints)
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_SetFrameLatency(IDXGIVkSwapChain *iface, UINT MaxLatency)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);

    if (!MaxLatency || MaxLatency > DXGI_MAX_SWAP_CHAIN_BUFFERS)
    {
        WARN("Invalid maximum frame latency %u.\n", MaxLatency);
        return DXGI_ERROR_INVALID_CALL;
    }

    /* Max frame latency without WAITABLE_OBJECT is always 3,
     * even if set on the device, according to docs. */
    if (!(chain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
    {
        WARN("DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT not set for swap chain %p.\n", iface);
        return DXGI_ERROR_INVALID_CALL;
    }

    /* Only increasing the latency is handled here; apparently it is
     * the application's responsibility to reduce the semaphore value
     * in case the latency gets reduced. */
    if (MaxLatency > chain->frame_latency)
        ReleaseSemaphore(chain->frame_latency_event, MaxLatency - chain->frame_latency, NULL);
    chain->frame_latency = MaxLatency;
    return S_OK;
}

static VkXYColorEXT convert_xy_color(UINT16 *dxgi_color)
{
    return (VkXYColorEXT) { dxgi_color[0] / 50000.0f, dxgi_color[1] / 50000.0f };
}

static float convert_max_luminance(UINT dxgi_luminance)
{
    /* The documentation says this is in *whole* nits, but this
     * contradicts the HEVC standard it claims to mirror,
     * and the sample's behaviour.
     * We should come back and validate this once
     * https://github.com/microsoft/DirectX-Graphics-Samples/issues/796
     * has an answer. */
    return (float)dxgi_luminance;
}

static float convert_min_luminance(UINT dxgi_luminance)
{
    return dxgi_luminance / 0.0001f;
}

static float convert_level(UINT16 dxgi_level)
{
    return (float)dxgi_level;
}

static VkHdrMetadataEXT convert_hdr_metadata_hdr10(DXGI_HDR_METADATA_HDR10 *dxgi_metadata)
{
    VkHdrMetadataEXT vulkan_metadata = { VK_STRUCTURE_TYPE_HDR_METADATA_EXT };
    vulkan_metadata.displayPrimaryRed = convert_xy_color(dxgi_metadata->RedPrimary);
    vulkan_metadata.displayPrimaryGreen = convert_xy_color(dxgi_metadata->GreenPrimary);
    vulkan_metadata.displayPrimaryBlue = convert_xy_color(dxgi_metadata->BluePrimary);
    vulkan_metadata.whitePoint = convert_xy_color(dxgi_metadata->WhitePoint);
    vulkan_metadata.maxLuminance = convert_max_luminance(dxgi_metadata->MaxMasteringLuminance);
    vulkan_metadata.minLuminance = convert_min_luminance(dxgi_metadata->MinMasteringLuminance);
    vulkan_metadata.maxContentLightLevel = convert_level(dxgi_metadata->MaxContentLightLevel);
    vulkan_metadata.maxFrameAverageLightLevel = convert_level(dxgi_metadata->MaxFrameAverageLightLevel);
    return vulkan_metadata;
}

static void dxgi_vk_swap_chain_set_hdr_metadata(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkHdrMetadataEXT hdr_metadata;

    if (!chain->queue->device->vk_info.EXT_hdr_metadata ||
            !chain->present.vk_swapchain ||
            chain->request.dxgi_hdr_metadata.Type != DXGI_HDR_METADATA_TYPE_HDR10)
    {
        return;
    }

    hdr_metadata = convert_hdr_metadata_hdr10(&chain->request.dxgi_hdr_metadata.HDR10);
    VK_CALL(vkSetHdrMetadataEXT(chain->queue->device->vk_device, 1, &chain->present.vk_swapchain, &hdr_metadata));
}

static bool dxgi_vk_swap_chain_present_task_is_idle(struct dxgi_vk_swap_chain *chain)
{
    uint32_t presented_count = vkd3d_atomic_uint32_load_explicit(&chain->present.present_count, vkd3d_memory_order_acquire);
    return presented_count == chain->user.present_count;
}

static bool dxgi_vk_swap_chain_is_occluded(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkPhysicalDevice vk_physical_device = chain->queue->device->vk_physical_device;
    VkSurfaceCapabilitiesKHR surface_caps;

    VK_CALL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_physical_device, chain->vk_surface, &surface_caps));
    /* Win32 jank, when these are 0 we cannot create a swapchain. */
    return surface_caps.maxImageExtent.width == 0 || surface_caps.maxImageExtent.height == 0;
}

static bool dxgi_vk_swap_chain_present_is_occluded(struct dxgi_vk_swap_chain *chain)
{
    if (dxgi_vk_swap_chain_present_task_is_idle(chain))
    {
        /* Query the surface directly. */
        chain->present.is_occlusion_state = dxgi_vk_swap_chain_is_occluded(chain);
        return chain->present.is_occlusion_state != 0;
    }
    else
    {
        /* If presentation requests are pending it is not safe to access the surface directly
         * without adding tons of locks everywhere,
         * so rely on observed behavior from presentation thread. */
        return vkd3d_atomic_uint32_load_explicit(&chain->present.is_occlusion_state, vkd3d_memory_order_relaxed) != 0;
    }
}

static void dxgi_vk_swap_chain_present_callback(void *chain);

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_Present(IDXGIVkSwapChain *iface, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS *pPresentParameters)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    struct dxgi_vk_swap_chain_present_request *request;
    (void)pPresentParameters;

    if (dxgi_vk_swap_chain_present_is_occluded(chain))
        return DXGI_STATUS_OCCLUDED;
    if (PresentFlags & DXGI_PRESENT_TEST)
        return S_OK;

    assert(chain->user.index < chain->desc.BufferCount);

    /* The present iteration on present thread has a similar counter and it will pick up the request from the ring. */
    chain->user.present_count += 1;
    request = &chain->request_ring[chain->user.present_count % ARRAY_SIZE(chain->request_ring)];

    request->swap_interval = SyncInterval;
    request->dxgi_format = chain->user.backbuffers[chain->user.index]->desc.Format;
    request->user_index = chain->user.index;
    request->dxgi_color_space_type = chain->user.dxgi_color_space_type;
    request->dxgi_hdr_metadata = chain->user.dxgi_hdr_metadata;
    request->modifies_hdr_metadata = chain->user.modifies_hdr_metadata;
    chain->user.modifies_hdr_metadata = false;

    /* Need to process this task in queue thread to deal with wait-before-signal.
     * All interesting works happens in the callback. */
    chain->user.blit_count += 1;
    d3d12_command_queue_enqueue_callback(chain->queue, dxgi_vk_swap_chain_present_callback, chain);

    if (!(chain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
        WaitForSingleObject(chain->frame_latency_event, INFINITE);
    chain->user.index = (chain->user.index + 1) % chain->desc.BufferCount;
    return S_OK;
}

static VkColorSpaceKHR convert_color_space(DXGI_COLOR_SPACE_TYPE dxgi_color_space);

static bool dxgi_vk_swap_chain_supports_color_space(struct dxgi_vk_swap_chain *chain, DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    VkColorSpaceKHR vk_color_space;
    uint32_t i;
    vk_color_space = convert_color_space(ColorSpace);
    for (i = 0; i < chain->properties.format_count; i++)
        if (chain->properties.formats[i].colorSpace == vk_color_space)
            return true;
    return false;
}

static UINT STDMETHODCALLTYPE dxgi_vk_swap_chain_CheckColorSpaceSupport(IDXGIVkSwapChain *iface, DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    UINT support_flags = 0;
    if (dxgi_vk_swap_chain_supports_color_space(chain, ColorSpace))
        support_flags |= DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT;
    return support_flags;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_SetColorSpace(IDXGIVkSwapChain *iface, DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    if (!dxgi_vk_swap_chain_supports_color_space(chain, ColorSpace))
        return E_INVALIDARG;

    chain->user.dxgi_color_space_type = ColorSpace;
    chain->user.modifies_hdr_metadata = true;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_SetHDRMetaData(IDXGIVkSwapChain *iface, const DXGI_VK_HDR_METADATA *pMetaData)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    chain->user.dxgi_hdr_metadata = *pMetaData;
    chain->user.modifies_hdr_metadata = true;
    return S_OK;
}

static CONST_VTBL struct IDXGIVkSwapChainVtbl dxgi_vk_swap_chain_vtbl =
{
    /* IUnknown methods */
    dxgi_vk_swap_chain_QueryInterface,
    dxgi_vk_swap_chain_AddRef,
    dxgi_vk_swap_chain_Release,

    /* IDXGIVkSwapChain methods */
    dxgi_vk_swap_chain_GetDesc,
    dxgi_vk_swap_chain_GetAdapter,
    dxgi_vk_swap_chain_GetDevice,
    dxgi_vk_swap_chain_GetImage,
    dxgi_vk_swap_chain_GetImageIndex,
    dxgi_vk_swap_chain_GetFrameLatency,
    dxgi_vk_swap_chain_GetFrameLatencyEvent,
    dxgi_vk_swap_chain_ChangeProperties,
    dxgi_vk_swap_chain_SetPresentRegion,
    dxgi_vk_swap_chain_SetGammaControl,
    dxgi_vk_swap_chain_SetFrameLatency,
    dxgi_vk_swap_chain_Present,
    dxgi_vk_swap_chain_CheckColorSpaceSupport,
    dxgi_vk_swap_chain_SetColorSpace,
    dxgi_vk_swap_chain_SetHDRMetaData,
};

static HRESULT dxgi_vk_swap_chain_create_surface(struct dxgi_vk_swap_chain *chain, HWND hwnd)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
#ifdef VK_KHR_win32_surface
    VkWin32SurfaceCreateInfoKHR create_info;
#endif
    VkPhysicalDevice vk_physical_device;
    VkInstance vk_instance;
    VkBool32 supported;
    VkResult vr;

    vk_instance = chain->queue->device->vkd3d_instance->vk_instance;
    vk_physical_device = chain->queue->device->vk_physical_device;

#ifdef VK_KHR_win32_surface
    create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    create_info.pNext = NULL;
    create_info.hwnd = hwnd;
    create_info.hinstance = GetModuleHandleA("d3d12.dll");
    create_info.flags = 0;
    vr = VK_CALL(vkCreateWin32SurfaceKHR(vk_instance, &create_info, NULL, &chain->vk_surface));
#else
    /* TODO: With dxvk-native integration, we can modify this as needed. */
    vr = VK_ERROR_SURFACE_LOST_KHR;
#endif

    if (vr < 0)
        return hresult_from_vk_result(vr);

    vr = VK_CALL(vkGetPhysicalDeviceSurfaceSupportKHR(vk_physical_device, chain->queue->vkd3d_queue->vk_family_index, chain->vk_surface, &supported));
    if (vr < 0)
        return hresult_from_vk_result(vr);
    if (!supported)
        return E_INVALIDARG;

    /* Query surface formats up-front. */
    VK_CALL(vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device, chain->vk_surface, &chain->properties.format_count, NULL));
    chain->properties.formats = vkd3d_malloc(chain->properties.format_count * sizeof(*chain->properties.formats));
    VK_CALL(vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device, chain->vk_surface, &chain->properties.format_count, chain->properties.formats));

    return S_OK;
}

static HRESULT dxgi_vk_swap_chain_init_sync_objects(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkSemaphoreTypeCreateInfoKHR type_info;
    VkFenceCreateInfo fence_create_info;
    VkSemaphoreCreateInfo create_info;
    VkResult vr;
    HRESULT hr;

    if (FAILED(hr = ID3D12Device9_CreateFence(&chain->queue->device->ID3D12Device_iface, DXGI_MAX_SWAP_CHAIN_BUFFERS,
                    0, &IID_ID3D12Fence1, (void**)&chain->present.frame_latency_fence)))
    {
        WARN("Failed to create frame latency fence, hr %#x.\n", hr);
        return hr;
    }

#define DEFAULT_FRAME_LATENCY 3
    if (chain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
        chain->frame_latency = 1;
    else
    {
        /* On the first frame, we are supposed to acquire,
         * but we only acquire after a Present, so do the implied one here. */
        chain->frame_latency = DEFAULT_FRAME_LATENCY - 1;
    }

    if (!(chain->frame_latency_event = CreateSemaphore(NULL, chain->frame_latency, DXGI_MAX_SWAP_CHAIN_BUFFERS, NULL)))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        WARN("Failed to create frame latency semaphore, hr %#x.\n", hr);
        return hr;
    }

    create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    create_info.pNext = &type_info;
    create_info.flags = 0;
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR;
    type_info.pNext = NULL;
    type_info.initialValue = 0;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;

    vr = VK_CALL(vkCreateSemaphore(chain->queue->device->vk_device, &create_info, NULL, &chain->present.vk_blit_semaphore));
    if (vr < 0)
        return hresult_from_vkd3d_result(vr);

    fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_create_info.pNext = NULL;
    fence_create_info.flags = 0;
    vr = VK_CALL(vkCreateFence(chain->queue->device->vk_device, &fence_create_info, NULL, &chain->present.vk_acquire_fence));
    if (vr < 0)
        return hresult_from_vkd3d_result(vr);

    return S_OK;
}

static void dxgi_vk_swap_chain_drain_waiter(struct dxgi_vk_swap_chain *chain)
{
    if (chain->wait_thread.active)
    {
        /* Waits until all swapchain waits have been processed. Required before we destroy the swapchain object. */
        pthread_mutex_lock(&chain->wait_thread.lock);
        while (chain->wait_thread.wait_queue_count)
            pthread_cond_wait(&chain->wait_thread.cond, &chain->wait_thread.lock);
        pthread_mutex_unlock(&chain->wait_thread.lock);
    }
}

static void dxgi_vk_swap_chain_destroy_swapchain_in_present_task(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkQueue vk_queue;
    unsigned int i;

    if (!chain->present.vk_swapchain)
        return;

    /* TODO: Can replace this stall with VK_KHR_present_wait,
     * but when destroying vk_release_semaphore we might be in a state where we submitted blit command buffer,
     * but never waited on the semaphore in vkQueuePresent, so we would still need this WaitIdle() most likely. */
    vk_queue = vkd3d_queue_acquire(chain->queue->vkd3d_queue);
    VK_CALL(vkQueueWaitIdle(vk_queue));
    vkd3d_queue_release(chain->queue->vkd3d_queue);

    dxgi_vk_swap_chain_drain_waiter(chain);

    for (i = 0; i < ARRAY_SIZE(chain->present.vk_backbuffer_image_views); i++)
        VK_CALL(vkDestroyImageView(chain->queue->device->vk_device, chain->present.vk_backbuffer_image_views[i], NULL));
    for (i = 0; i < ARRAY_SIZE(chain->present.vk_release_semaphores); i++)
        VK_CALL(vkDestroySemaphore(chain->queue->device->vk_device, chain->present.vk_release_semaphores[i], NULL));
    memset(chain->present.vk_backbuffer_images, 0, sizeof(chain->present.vk_backbuffer_images));
    memset(chain->present.vk_backbuffer_image_views, 0, sizeof(chain->present.vk_backbuffer_image_views));
    memset(chain->present.vk_release_semaphores, 0, sizeof(chain->present.vk_release_semaphores));

    VK_CALL(vkDestroySwapchainKHR(chain->queue->device->vk_device, chain->present.vk_swapchain, NULL));
    chain->present.vk_swapchain = VK_NULL_HANDLE;
    chain->present.backbuffer_width = 0;
    chain->present.backbuffer_height = 0;
    chain->present.backbuffer_format = VK_FORMAT_UNDEFINED;
    chain->present.backbuffer_count = 0;
    chain->present.force_swapchain_recreation = false;
    chain->present.present_id_valid = false;
    chain->present.present_id = 0;
}

static VkColorSpaceKHR convert_color_space(DXGI_COLOR_SPACE_TYPE dxgi_color_space)
{
    switch (dxgi_color_space)
    {
        case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
            return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
            return VK_COLOR_SPACE_HDR10_ST2084_EXT;
        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
            return VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
        default:
            WARN("Unhandled color space %#x. Falling back to sRGB.\n", dxgi_color_space);
            return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }
}

static bool dxgi_vk_swap_chain_accept_format(const VkSurfaceFormatKHR *format, VkFormat vk_format)
{
    if (vk_format == VK_FORMAT_UNDEFINED)
    {
        switch (format->format)
        {
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
                return true;

            default:
                return false;
        }
    }
    else
    {
        return format->format == vk_format;
    }
}

static bool dxgi_vk_swap_chain_find_surface_format(struct dxgi_vk_swap_chain *chain, VkFormat vk_format,
        VkColorSpaceKHR color_space, VkSurfaceFormatKHR *format)
{
    uint32_t i;

    for (i = 0; i < chain->properties.format_count; i++)
    {
        if (dxgi_vk_swap_chain_accept_format(&chain->properties.formats[i], vk_format) &&
                chain->properties.formats[i].colorSpace == color_space)
        {
            *format = chain->properties.formats[i];
            return true;
        }
    }

    return false;
}

static bool dxgi_vk_swap_chain_select_format(struct dxgi_vk_swap_chain *chain, VkSurfaceFormatKHR *format)
{
    VkFormat vk_format = vkd3d_get_format(chain->queue->device, chain->request.dxgi_format, false)->vk_format;
    VkColorSpaceKHR vk_color_space = convert_color_space(chain->request.dxgi_color_space_type);

    if (dxgi_vk_swap_chain_find_surface_format(chain, vk_format, vk_color_space, format))
        return true;

    /* If we're using sRGB swapchains, we can fallback.
     * Usually happens for RGBA8 or 10-bit UNORM and display does not support it as a present format.
     * This can be trivially worked around by selecting e.g. BGRA8. */
    if (vk_color_space == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
    {
        return dxgi_vk_swap_chain_find_surface_format(chain, VK_FORMAT_UNDEFINED, vk_color_space, format);
    }
    else
    {
        /* Refuse to present unsupported HDR since it will look completely bogus. */
        return false;
    }
}

static bool dxgi_vk_swap_chain_check_present_mode_support(struct dxgi_vk_swap_chain *chain, VkPresentModeKHR present_mode)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkPhysicalDevice vk_physical_device = chain->queue->device->vk_physical_device;
    VkPresentModeKHR supported_modes[16];
    uint32_t mode_count;
    uint32_t i;

    mode_count = ARRAY_SIZE(supported_modes);
    VK_CALL(vkGetPhysicalDeviceSurfacePresentModesKHR(vk_physical_device, chain->vk_surface, &mode_count, supported_modes));
    for (i = 0; i < mode_count; i++)
        if (supported_modes[i] == present_mode)
            return true;
    return false;
}

static void dxgi_vk_swap_chain_init_blit_pipeline(struct dxgi_vk_swap_chain *chain)
{
    struct d3d12_device *device = chain->queue->device;
    struct vkd3d_swapchain_pipeline_key key;
    HRESULT hr;

    key.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    key.filter = chain->desc.Scaling == DXGI_SCALING_NONE ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    key.format = chain->present.backbuffer_format;

    if (FAILED(hr = vkd3d_meta_get_swapchain_pipeline(&device->meta_ops, &key, &chain->present.pipeline)))
        ERR("Failed to initialize swapchain pipeline.\n");
}

static void dxgi_vk_swap_chain_recreate_swapchain_in_present_task(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkPhysicalDevice vk_physical_device = chain->queue->device->vk_physical_device;
    VkDevice vk_device = chain->queue->device->vk_device;
    VkCommandPoolCreateInfo command_pool_create_info;
    VkSwapchainCreateInfoKHR swapchain_create_info;
    VkSurfaceCapabilitiesKHR surface_caps;
    VkSurfaceFormatKHR surface_format;
    VkImageViewCreateInfo view_info;
    VkPresentModeKHR present_mode;
    bool new_occlusion_state;
    VkResult vr;
    uint32_t i;

    dxgi_vk_swap_chain_destroy_swapchain_in_present_task(chain);

    /* Don't bother if we've observed ERROR_SURFACE_LOST. */
    if (chain->present.is_surface_lost)
        return;

    VK_CALL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_physical_device, chain->vk_surface, &surface_caps));

    /* Win32 quirk. Minimized windows have maximum extents of zero. */
    new_occlusion_state = surface_caps.maxImageExtent.width == 0 || surface_caps.maxImageExtent.height == 0;
    vkd3d_atomic_uint32_store_explicit(&chain->present.is_occlusion_state, (uint32_t)new_occlusion_state, vkd3d_memory_order_relaxed);

    /* There is nothing to do. We'll do a dummy present. */
    if (new_occlusion_state)
        return;

    /* Sanity check, this cannot happen on Win32 surfaces, but could happen on Wayland. */
    if (surface_caps.currentExtent.width == UINT32_MAX || surface_caps.currentExtent.height == UINT32_MAX)
        return;

    /* No format to present to yet. Can happen in transition states for HDR.
     * Where we have modified color space, but not yet changed user backbuffer format. */
    if (!dxgi_vk_swap_chain_select_format(chain, &surface_format))
        return;

    present_mode = chain->request.swap_interval > 0 ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (!dxgi_vk_swap_chain_check_present_mode_support(chain, present_mode))
    {
        if (present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR &&
                dxgi_vk_swap_chain_check_present_mode_support(chain, VK_PRESENT_MODE_MAILBOX_KHR))
        {
            present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
        }
        else
            return;
    }

    memset(&swapchain_create_info, 0, sizeof(swapchain_create_info));
    swapchain_create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_create_info.surface = chain->vk_surface;
    swapchain_create_info.imageArrayLayers = 1;
    swapchain_create_info.imageColorSpace = surface_format.colorSpace;
    swapchain_create_info.imageFormat = surface_format.format;
    swapchain_create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain_create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_create_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchain_create_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_create_info.clipped = VK_TRUE;

    /* We don't block directly on Present(), so there's no reason to use more than 3 images if even application requests more.
     * We could get away with 2 if we used WSI acquire semaphore and async acquire was supported, but e.g. Mesa does not support that. */
    swapchain_create_info.minImageCount = max(3u, surface_caps.minImageCount);

    swapchain_create_info.imageExtent = surface_caps.currentExtent;
    swapchain_create_info.imageExtent.width = max(swapchain_create_info.imageExtent.width, surface_caps.minImageExtent.width);
    swapchain_create_info.imageExtent.width = min(swapchain_create_info.imageExtent.width, surface_caps.maxImageExtent.width);
    swapchain_create_info.imageExtent.height = max(swapchain_create_info.imageExtent.height, surface_caps.minImageExtent.height);
    swapchain_create_info.imageExtent.height = min(swapchain_create_info.imageExtent.height, surface_caps.maxImageExtent.height);

    vr = VK_CALL(vkCreateSwapchainKHR(vk_device, &swapchain_create_info, NULL, &chain->present.vk_swapchain));
    if (vr < 0)
    {
        ERR("Failed to create swapchain, vr %d.\n");
        chain->present.vk_swapchain = VK_NULL_HANDLE;
        return;
    }

    chain->present.backbuffer_count = ARRAY_SIZE(chain->present.vk_backbuffer_images);
    VK_CALL(vkGetSwapchainImagesKHR(vk_device, chain->present.vk_swapchain, &chain->present.backbuffer_count, chain->present.vk_backbuffer_images));

    memset(&view_info, 0, sizeof(view_info));
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.format = swapchain_create_info.imageFormat;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.layerCount = 1;
    view_info.subresourceRange.levelCount = 1;

    for (i = 0; i < chain->present.backbuffer_count; i++)
    {
        view_info.image = chain->present.vk_backbuffer_images[i];
        VK_CALL(vkCreateImageView(vk_device, &view_info, NULL, &chain->present.vk_backbuffer_image_views[i]));
    }

    chain->present.backbuffer_width = swapchain_create_info.imageExtent.width;
    chain->present.backbuffer_height = swapchain_create_info.imageExtent.height;
    chain->present.backbuffer_format = swapchain_create_info.imageFormat;

    if (!chain->present.vk_blit_command_pool)
    {
        command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        command_pool_create_info.pNext = NULL;
        command_pool_create_info.queueFamilyIndex = chain->queue->vkd3d_queue->vk_family_index;
        command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CALL(vkCreateCommandPool(vk_device, &command_pool_create_info, NULL, &chain->present.vk_blit_command_pool));
    }

    dxgi_vk_swap_chain_init_blit_pipeline(chain);
    dxgi_vk_swap_chain_set_hdr_metadata(chain);
}

static bool request_needs_swapchain_recreation(const struct dxgi_vk_swap_chain_present_request *request,
        const struct dxgi_vk_swap_chain_present_request *last_request)
{
    return request->dxgi_color_space_type != last_request->dxgi_color_space_type ||
        request->dxgi_format != last_request->dxgi_format ||
        (!!request->swap_interval) != (!!last_request->swap_interval);
}

static void dxgi_vk_swap_chain_present_signal_blit_semaphore(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkTimelineSemaphoreSubmitInfoKHR timeline_info;
    VkSubmitInfo submit_info;
    VkQueue vk_queue;
    VkResult vr;

    chain->present.blit_count += 1;

    memset(&submit_info, 0, sizeof(submit_info));
    memset(&timeline_info, 0, sizeof(timeline_info));
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = &timeline_info;
    submit_info.pSignalSemaphores = &chain->present.vk_blit_semaphore;
    submit_info.signalSemaphoreCount = 1;

    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR;
    timeline_info.signalSemaphoreValueCount = 1;
    timeline_info.pSignalSemaphoreValues = &chain->present.blit_count;

    vk_queue = vkd3d_queue_acquire(chain->queue->vkd3d_queue);
    vr = VK_CALL(vkQueueSubmit(vk_queue, 1, &submit_info, VK_NULL_HANDLE));
    vkd3d_queue_release(chain->queue->vkd3d_queue);

    if (vr)
    {
        ERR("Failed to submit present discard, vr = %d.\n", vr);
        VKD3D_DEVICE_REPORT_BREADCRUMB_IF(chain->queue->device, vr == VK_ERROR_DEVICE_LOST);
    }
}

static void dxgi_vk_swap_chain_wait_and_reset_acquire_fence(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkDevice vk_device = chain->queue->device->vk_device;
    VkResult vr;

    /* We're doing this in a thread.
     * There is little reason to add complexity with semaphores since behavior is
     * implementation defined regarding if AcquireNextImage is synchronous or not. */
    vr = VK_CALL(vkWaitForFences(vk_device, 1, &chain->present.vk_acquire_fence, VK_TRUE, UINT64_MAX));
    VKD3D_DEVICE_REPORT_BREADCRUMB_IF(chain->queue->device, vr == VK_ERROR_DEVICE_LOST);
    vr = VK_CALL(vkResetFences(vk_device, 1, &chain->present.vk_acquire_fence));
    VKD3D_DEVICE_REPORT_BREADCRUMB_IF(chain->queue->device, vr == VK_ERROR_DEVICE_LOST);
}

static void dxgi_vk_swap_chain_record_render_pass(struct dxgi_vk_swap_chain *chain, VkCommandBuffer vk_cmd, uint32_t swapchain_index)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkRenderingAttachmentInfoKHR attachment_info;
    VkImageMemoryBarrier image_barrier;
    VkRenderingInfoKHR rendering_info;
    VkDescriptorImageInfo image_info;
    VkWriteDescriptorSet write_info;
    struct d3d12_resource *resource;
    VkViewport viewport;
    bool blank_present;

    /* If application intends to present before we have rendered to it,
     * it is valid, but we need to ignore the blit, just clear backbuffer. */
    resource = chain->user.backbuffers[chain->request.user_index];
    blank_present = vkd3d_atomic_uint32_load_explicit(&resource->initial_layout_transition, vkd3d_memory_order_relaxed) != 0;

    if (blank_present)
        WARN("Application is presenting user index %u, but it has never been rendered to.\n", chain->request.user_index);

    memset(&attachment_info, 0, sizeof(attachment_info));
    attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    attachment_info.imageView = chain->present.vk_backbuffer_image_views[swapchain_index];
    attachment_info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment_info.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment_info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    if (chain->desc.Scaling == DXGI_SCALING_NONE || blank_present)
        attachment_info.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;

    memset(&rendering_info, 0, sizeof(rendering_info));
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
    rendering_info.renderArea.extent.width = chain->present.backbuffer_width;
    rendering_info.renderArea.extent.height = chain->present.backbuffer_height;
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachments = &attachment_info;

    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    if (chain->desc.Scaling == DXGI_SCALING_NONE)
    {
        viewport.width = (float)chain->desc.Width;
        viewport.height = (float)chain->desc.Height;
    }
    else
    {
        viewport.width = chain->present.backbuffer_width;
        viewport.height = chain->present.backbuffer_height;
    }

    image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image_barrier.pNext = NULL;
    image_barrier.srcAccessMask = 0;
    image_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = chain->present.vk_backbuffer_images[swapchain_index];
    image_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_barrier.subresourceRange.baseMipLevel = 0;
    image_barrier.subresourceRange.levelCount = 1;
    image_barrier.subresourceRange.baseArrayLayer = 0;
    image_barrier.subresourceRange.layerCount = 1;

    /* srcStage = TOP_OF_PIPE since we're using fences to acquire WSI. */
    VK_CALL(vkCmdPipelineBarrier(vk_cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, 0, NULL, 0, NULL, 1, &image_barrier));

    VK_CALL(vkCmdBeginRenderingKHR(vk_cmd, &rendering_info));

    if (!blank_present)
    {
        VK_CALL(vkCmdSetViewport(vk_cmd, 0, 1, &viewport));
        VK_CALL(vkCmdSetScissor(vk_cmd, 0, 1, &rendering_info.renderArea));
        VK_CALL(vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    chain->present.pipeline.vk_pipeline));

        write_info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_info.pNext = NULL;
        write_info.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write_info.pBufferInfo = NULL;
        write_info.dstSet = VK_NULL_HANDLE;
        write_info.pTexelBufferView = NULL;
        write_info.pImageInfo = &image_info;
        write_info.dstBinding = 0;
        write_info.dstArrayElement = 0;
        write_info.descriptorCount = 1;
        image_info.imageView = chain->user.vk_image_views[chain->request.user_index];
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_info.sampler = VK_NULL_HANDLE;

        VK_CALL(vkCmdPushDescriptorSetKHR(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    chain->present.pipeline.vk_pipeline_layout, 0, 1, &write_info));

        VK_CALL(vkCmdDraw(vk_cmd, 3, 1, 0, 0));
    }

    VK_CALL(vkCmdEndRenderingKHR(vk_cmd));

    image_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image_barrier.dstAccessMask = 0;
    image_barrier.oldLayout = image_barrier.newLayout;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VK_CALL(vkCmdPipelineBarrier(vk_cmd,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0, 0, NULL, 0, NULL, 1, &image_barrier));
}

static bool dxgi_vk_swap_chain_submit_blit(struct dxgi_vk_swap_chain *chain, uint32_t swapchain_index)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkDevice vk_device = chain->queue->device->vk_device;
    VkSemaphoreCreateInfo semaphore_create_info;
    VkCommandBufferAllocateInfo allocate_info;
    VkCommandBufferBeginInfo cmd_begin_info;
    VkFenceCreateInfo fence_create_info;
    VkSubmitInfo submit_info;
    VkCommandBuffer vk_cmd;
    VkQueue vk_queue;
    VkResult vr;

    /* Create objects on-demand. */
    if (!chain->present.vk_release_semaphores[swapchain_index])
    {
        semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphore_create_info.pNext = NULL;
        semaphore_create_info.flags = 0;
        vr = VK_CALL(vkCreateSemaphore(vk_device, &semaphore_create_info,
                    NULL, &chain->present.vk_release_semaphores[swapchain_index]));
        if (vr < 0)
        {
            ERR("Failed to create semaphore, vr %d.\n", vr);
            return false;
        }
    }

    if (!chain->present.vk_blit_command_buffers[swapchain_index])
    {
        memset(&allocate_info, 0, sizeof(allocate_info));
        allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate_info.commandBufferCount = 1;
        allocate_info.commandPool = chain->present.vk_blit_command_pool;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        VK_CALL(vkAllocateCommandBuffers(vk_device, &allocate_info,
                    &chain->present.vk_blit_command_buffers[swapchain_index]));
    }

    if (chain->present.vk_blit_fences[swapchain_index])
    {
        vr = VK_CALL(vkWaitForFences(vk_device, 1, &chain->present.vk_blit_fences[swapchain_index], VK_TRUE, UINT64_MAX));
        VKD3D_DEVICE_REPORT_BREADCRUMB_IF(chain->queue->device, vr == VK_ERROR_DEVICE_LOST);
        if (vr < 0)
            return false;
        VK_CALL(vkResetFences(vk_device, 1, &chain->present.vk_blit_fences[swapchain_index]));
    }
    else
    {
        fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_create_info.pNext = NULL;
        fence_create_info.flags = 0;
        vr = VK_CALL(vkCreateFence(vk_device, &fence_create_info, NULL, &chain->present.vk_blit_fences[swapchain_index]));
        if (vr < 0)
            return false;
    }

    vk_cmd = chain->present.vk_blit_command_buffers[swapchain_index];

    VK_CALL(vkResetCommandBuffer(vk_cmd, 0));
    memset(&cmd_begin_info, 0, sizeof(cmd_begin_info));
    cmd_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmd_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CALL(vkBeginCommandBuffer(vk_cmd, &cmd_begin_info));
    dxgi_vk_swap_chain_record_render_pass(chain, vk_cmd, swapchain_index);
    VK_CALL(vkEndCommandBuffer(vk_cmd));

    memset(&submit_info, 0, sizeof(submit_info));
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pCommandBuffers = &vk_cmd;
    submit_info.commandBufferCount = 1;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &chain->present.vk_release_semaphores[swapchain_index];

    vk_queue = vkd3d_queue_acquire(chain->queue->vkd3d_queue);
    vr = VK_CALL(vkQueueSubmit(vk_queue, 1, &submit_info, chain->present.vk_blit_fences[swapchain_index]));
    vkd3d_queue_release(chain->queue->vkd3d_queue);
    VKD3D_DEVICE_REPORT_BREADCRUMB_IF(chain->queue->device, vr == VK_ERROR_DEVICE_LOST);

    return vr == VK_SUCCESS;
}

static void dxgi_vk_swap_chain_present_recreate_swapchain_if_required(struct dxgi_vk_swap_chain *chain)
{
    if (!chain->present.vk_swapchain || chain->present.force_swapchain_recreation)
        dxgi_vk_swap_chain_recreate_swapchain_in_present_task(chain);
}

static void dxgi_vk_swap_chain_present_iteration(struct dxgi_vk_swap_chain *chain, unsigned int retry_counter)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkDevice vk_device = chain->queue->device->vk_device;
    VkPresentInfoKHR present_info;
    VkPresentIdKHR present_id;
    uint32_t swapchain_index;
    VkResult vk_result;
    VkQueue vk_queue;
    VkResult vr;

    dxgi_vk_swap_chain_present_recreate_swapchain_if_required(chain);
    if (!chain->present.vk_swapchain)
        return;

    vr = VK_CALL(vkAcquireNextImageKHR(vk_device, chain->present.vk_swapchain, UINT64_MAX,
                VK_NULL_HANDLE, chain->present.vk_acquire_fence, &swapchain_index));
    VKD3D_DEVICE_REPORT_BREADCRUMB_IF(chain->queue->device, vr == VK_ERROR_DEVICE_LOST);
    if (vr >= 0)
        dxgi_vk_swap_chain_wait_and_reset_acquire_fence(chain);

    /* Handle any errors and retry as needed. If we cannot make meaningful forward progress, just give up and retry later. */
    if (vr == VK_SUBOPTIMAL_KHR || vr < 0)
        chain->present.force_swapchain_recreation = true;
    if (vr < 0)
        dxgi_vk_swap_chain_destroy_swapchain_in_present_task(chain);

    if (vr == VK_ERROR_OUT_OF_DATE_KHR)
    {
        if (retry_counter < 3)
            dxgi_vk_swap_chain_present_iteration(chain, retry_counter + 1);
    }
    else if (vr == VK_ERROR_SURFACE_LOST_KHR)
    {
        /* If the surface is lost, we cannot expect to get forward progress. Just keep rendering to nothing. */
        chain->present.is_surface_lost = true;
    }

    if (vr < 0)
        return;

    if (!dxgi_vk_swap_chain_submit_blit(chain, swapchain_index))
        return;

    memset(&present_info, 0, sizeof(present_info));
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.pSwapchains = &chain->present.vk_swapchain;
    present_info.swapchainCount = 1;
    present_info.pImageIndices = &swapchain_index;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &chain->present.vk_release_semaphores[swapchain_index];
    present_info.pResults = &vk_result;

    if (chain->wait_thread.active && !chain->present.present_id_valid)
    {
        chain->present.present_id += 1;
        present_id.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
        present_id.pNext = NULL;
        present_id.swapchainCount = 1;
        present_id.pPresentIds = &chain->present.present_id;
        present_info.pNext = &present_id;
    }

    vk_queue = vkd3d_queue_acquire(chain->queue->vkd3d_queue);
    vr = VK_CALL(vkQueuePresentKHR(vk_queue, &present_info));
    vkd3d_queue_release(chain->queue->vkd3d_queue);
    VKD3D_DEVICE_REPORT_BREADCRUMB_IF(chain->queue->device, vr == VK_ERROR_DEVICE_LOST);

    if (vr == VK_SUCCESS && vk_result != VK_SUCCESS)
        vr = vk_result;

    /* Only use the present wait mechanism for FIFO present mode.
     * For IMMEDIATE or MAILBOX, I have observed iffy behavior on NVIDIA in the past,
     * and accurate frame latency isn't really a concern with these modes anyways.
     * When swap interval >= 1, make sure we signal after the first present iteration goes on screen. */
    if (present_info.pNext && vr >= 0 && chain->request.swap_interval >= 1)
        chain->present.present_id_valid = true;

    /* Handle any errors and retry as needed. If we cannot make meaningful forward progress, just give up and retry later. */
    if (vr == VK_SUBOPTIMAL_KHR || vr < 0)
        chain->present.force_swapchain_recreation = true;
    if (vr < 0)
        dxgi_vk_swap_chain_destroy_swapchain_in_present_task(chain);

    if (vr == VK_ERROR_OUT_OF_DATE_KHR)
    {
        if (retry_counter < 3)
            dxgi_vk_swap_chain_present_iteration(chain, retry_counter + 1);
    }
    else if (vr == VK_ERROR_SURFACE_LOST_KHR)
    {
        /* If the surface is lost, we cannot expect to get forward progress. Just keep rendering to nothing. */
        chain->present.is_surface_lost = true;
    }
}

static void dxgi_vk_swap_chain_signal_waitable_handle(struct dxgi_vk_swap_chain *chain)
{
    HRESULT hr;

    if (chain->present.present_id_valid)
    {
        dxgi_vk_swap_chain_push_present_id(chain, chain->present.present_id);
    }
    else
    {
        chain->present.frame_latency_count += 1;
        d3d12_command_queue_signal_inline(chain->queue, chain->present.frame_latency_fence, chain->present.frame_latency_count);

        if (FAILED(hr = d3d12_fence_set_event_on_completion(impl_from_ID3D12Fence1(chain->present.frame_latency_fence),
                        chain->present.frame_latency_count, chain->frame_latency_event, VKD3D_WAITING_EVENT_TYPE_SEMAPHORE)))
        {
            ERR("Failed to enqueue frame latency event, hr %#x.\n", hr);
            ReleaseSemaphore(chain->frame_latency_event, 1, NULL);
        }
    }
}

static void dxgi_vk_swap_chain_present_callback(void *chain_)
{
    const struct dxgi_vk_swap_chain_present_request *next_request;
    struct dxgi_vk_swap_chain *chain = chain_;
    uint32_t next_present_count;
    uint32_t present_count;
    uint32_t i;

    next_present_count = chain->present.present_count + 1;
    next_request = &chain->request_ring[next_present_count % ARRAY_SIZE(chain->request_ring)];
    if (request_needs_swapchain_recreation(next_request, &chain->request))
        chain->present.force_swapchain_recreation = true;

    chain->request = *next_request;
    if (chain->request.modifies_hdr_metadata)
        dxgi_vk_swap_chain_set_hdr_metadata(chain);

    /* If no QueuePresentKHRs successfully commits a present ID, we'll fallback to a normal queue signal. */
    chain->present.present_id_valid = false;

    /* There is currently no present timing in Vulkan we can rely on, so just duplicate blit them as needed.
     * This happens on a thread, so the blocking should not be a significant problem. */
    present_count = max(1u, chain->request.swap_interval);
    for (i = 0; i < present_count; i++)
    {
        /* A present iteration may or may not render to backbuffer. We'll apply best effort here.
         * Forward progress must be ensured, so if we cannot get anything on-screen in a reasonable amount of retries, ignore it. */
        dxgi_vk_swap_chain_present_iteration(chain, 0);
    }

    /* When this is signalled, lets main thread know that it's safe to free user buffers.
     * Signal this just once on the outside since we might have retries, swap_interval > 1, etc, which complicates command buffer recycling. */
    dxgi_vk_swap_chain_present_signal_blit_semaphore(chain);

    /* Signal latency fence. */
    dxgi_vk_swap_chain_signal_waitable_handle(chain);

    /* Signal main thread that we are done with all CPU work. No need to signal a condition variable, main thread can poll to deduce. */
    vkd3d_atomic_uint32_store_explicit(&chain->present.present_count, next_present_count, vkd3d_memory_order_release);
}

static void *dxgi_vk_swap_chain_wait_worker(void *chain_)
{
    struct dxgi_vk_swap_chain *chain = chain_;

    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    uint64_t next_wait_id;

    vkd3d_set_thread_name("vkd3d-swapchain-sync");

    for (;;)
    {
        pthread_mutex_lock(&chain->wait_thread.lock);
        while (!chain->wait_thread.wait_queue_count)
            pthread_cond_wait(&chain->wait_thread.cond, &chain->wait_thread.lock);
        next_wait_id = chain->wait_thread.wait_queue[0];
        pthread_mutex_unlock(&chain->wait_thread.lock);

        /* Sentinel for swapchain teardown. */
        if (!next_wait_id)
            break;

        /* We don't really care if we observed OUT_OF_DATE or something here. */
        VK_CALL(vkWaitForPresentKHR(chain->queue->device->vk_device, chain->present.vk_swapchain,
                    next_wait_id, UINT64_MAX));
        ReleaseSemaphore(chain->frame_latency_event, 1, NULL);

        /* Need to let present tasks know when it's safe to destroy a swapchain.
         * We must have completed all outstanding waits touching VkSwapchainKHR. */
        pthread_mutex_lock(&chain->wait_thread.lock);
        chain->wait_thread.wait_queue_count -= 1;
        memmove(chain->wait_thread.wait_queue, chain->wait_thread.wait_queue + 1, chain->wait_thread.wait_queue_count * sizeof(uint64_t));
        if (chain->wait_thread.wait_queue_count == 0)
            pthread_cond_signal(&chain->wait_thread.cond);
        pthread_mutex_unlock(&chain->wait_thread.lock);
    }

    return NULL;
}

static HRESULT dxgi_vk_swap_chain_init_waiter_thread(struct dxgi_vk_swap_chain *chain)
{
    if (!chain->queue->device->device_info.present_wait_features.presentWait)
        return S_OK;

    vkd3d_array_reserve((void **)&chain->wait_thread.wait_queue, &chain->wait_thread.wait_queue_size,
            DXGI_MAX_SWAP_CHAIN_BUFFERS, sizeof(*chain->wait_thread.wait_queue));
    pthread_mutex_init(&chain->wait_thread.lock, NULL);
    pthread_cond_init(&chain->wait_thread.cond, NULL);

    /* Have to throw a thread under the bus unfortunately.
     * That thread will only wait on present IDs and release HANDLEs as necessary. */
    if (pthread_create(&chain->wait_thread.thread, NULL, dxgi_vk_swap_chain_wait_worker, chain) < 0)
    {
        pthread_mutex_destroy(&chain->wait_thread.lock);
        pthread_cond_destroy(&chain->wait_thread.cond);
        return E_OUTOFMEMORY;
    }

    INFO("Enabling present wait path for frame latency.\n");
    chain->wait_thread.active = true;
    return S_OK;
}

static HRESULT dxgi_vk_swap_chain_init(struct dxgi_vk_swap_chain *chain, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc, struct d3d12_command_queue *queue)
{
    HRESULT hr;

    chain->IDXGIVkSwapChain_iface.lpVtbl = &dxgi_vk_swap_chain_vtbl;
    chain->refcount = 1;
    chain->queue = queue;
    chain->desc = *pDesc;

    if (FAILED(hr = dxgi_vk_swap_chain_reallocate_user_buffers(chain)))
        goto err;

    if (FAILED(hr = dxgi_vk_swap_chain_init_sync_objects(chain)))
        goto err;

    if (FAILED(hr = dxgi_vk_swap_chain_create_surface(chain, hwnd)))
        goto err;

    if (FAILED(hr = dxgi_vk_swap_chain_init_waiter_thread(chain)))
        goto err;

    ID3D12CommandQueue_AddRef(&queue->ID3D12CommandQueue_iface);
    return S_OK;

err:
    dxgi_vk_swap_chain_cleanup(chain);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_factory_CreateSwapChain(IDXGIVkSwapChainFactory *iface,
        HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIVkSwapChain **ppSwapchain)
{
    struct dxgi_vk_swap_chain_factory *factory = impl_from_IDXGIVkSwapChainFactory(iface);
    struct dxgi_vk_swap_chain *chain;
    HRESULT hr;

    chain = vkd3d_calloc(1, sizeof(*chain));
    if (!chain)
        return E_OUTOFMEMORY;

    if (FAILED(hr = dxgi_vk_swap_chain_init(chain, hWnd, pDesc, factory->queue)))
    {
        vkd3d_free(chain);
        return hr;
    }

    *ppSwapchain = &chain->IDXGIVkSwapChain_iface;
    return S_OK;
}

static CONST_VTBL struct IDXGIVkSwapChainFactoryVtbl dxgi_vk_swap_chain_factory_vtbl =
{
    /* IUnknown methods */
    dxgi_vk_swap_chain_factory_QueryInterface,
    dxgi_vk_swap_chain_factory_AddRef,
    dxgi_vk_swap_chain_factory_Release,

    /* IDXGIVkSwapChain methods */
    dxgi_vk_swap_chain_factory_CreateSwapChain,
};

HRESULT dxgi_vk_swap_chain_factory_init(struct d3d12_command_queue *queue, struct dxgi_vk_swap_chain_factory *chain)
{
    chain->IDXGIVkSwapChainFactory_iface.lpVtbl = &dxgi_vk_swap_chain_factory_vtbl;
    chain->queue = queue;
    return S_OK;
}
