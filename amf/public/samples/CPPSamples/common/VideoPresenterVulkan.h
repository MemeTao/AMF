// 
// Notice Regarding Standards.  AMD does not provide a license or sublicense to
// any Intellectual Property Rights relating to any standards, including but not
// limited to any audio and/or video codec technologies such as MPEG-2, MPEG-4;
// AVC/H.264; HEVC/H.265; AAC decode/FFMPEG; AAC encode/FFMPEG; VC-1; and MP3
// (collectively, the "Media Technologies"). For clarity, you will pay any
// royalties due for such third party technologies, which may include the Media
// Technologies that are owed as a result of AMD providing the Software to you.
// 
// MIT license 
// 
// Copyright (c) 2018 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
#pragma once

#include "BackBufferPresenter.h"
#include "SwapChainVulkan.h"
#include <unordered_map>

struct DescriptorVulkan
{
    VkDescriptorSetLayoutBinding    layoutBinding;  // Binding layout
    amf_uint32                      setIndex;
};

class DescriptorHeapVulkan : public VulkanContext
{
public:

    DescriptorHeapVulkan();
    virtual                                         ~DescriptorHeapVulkan();
    AMF_RESULT                                      Terminate();
    AMF_RESULT                                      RegisterDescriptorSet(DescriptorVulkan** ppDescriptors, amf_uint32 count);
    AMF_RESULT                                      CreateDescriptors();

    amf_uint32                                      GetDescriptorCount() const;

    VkDescriptorSet                                 GetDescriptorSet(amf_uint32 setIndex) const;
    VkDescriptorSetLayout                           GetDescriptorSetLayout(amf_uint32 setIndex) const;

    const amf::amf_vector<VkDescriptorSet>& GetDescriptorSets() const;
    const amf::amf_vector<VkDescriptorSetLayout>& GetDescriptorSetLayouts() const;

    AMF_RESULT UpdateDescriptorSet(const DescriptorVulkan* pDescriptor, amf_uint32 arrayIndex, amf_uint32 count, VkWriteDescriptorSet& writeInfo, bool immediate);
    AMF_RESULT UpdateDescriptorSet(const DescriptorVulkan* pDescriptor, amf_uint32 arrayIndex, amf_uint32 count, const VkDescriptorBufferInfo* pBufferInfos, bool immediate);
    AMF_RESULT UpdateDescriptorSet(const DescriptorVulkan* pDescriptor, amf_uint32 arrayIndex, amf_uint32 count, const VkDescriptorImageInfo* pImageInfos, bool immediate);
    AMF_RESULT UpdatePendingDescriptorSets();

private:
    VkDescriptorPool                        m_hDescriptorPool;
    amf::amf_vector<VkDescriptorSet>        m_hDescriptorSets;
    amf::amf_vector<VkDescriptorSetLayout>  m_hDescriptorSetLayouts;
    amf::amf_vector<VkDescriptorPoolSize>   m_descriptorPoolSizes;

    // Cache the descriptor set updates for combined call
    amf::amf_vector<VkWriteDescriptorSet>   m_pendingDescriptorUpdates;

    // If we are caching the descriptor update for later call,
    // we need to also cache the image/buffer info array passed
    // Allocate the arrays on heap with smart pointers and keep
    // list of all the smart pointers
    template <typename _Ty>
    using UpdateInfoList = std::unique_ptr<_Ty[]>;

    template<typename _Ty>
    using UpdateInfoHeap = amf::amf_vector<UpdateInfoList<_Ty>>;

    UpdateInfoHeap<VkDescriptorBufferInfo>  m_descriptorBufferInfoHeap;
    UpdateInfoHeap<VkDescriptorImageInfo>   m_descriptorImageInfoHeap;

    template<typename UpdateInfo_T>
    AMF_RESULT ProcessUpdateInfo(amf_uint32 count, const UpdateInfo_T* pUpdateInfos, bool immediate, UpdateInfoHeap<UpdateInfo_T>& infoHeap, const UpdateInfo_T** ppOutUpdateInfos)
    {
        if (immediate == true)
        {
            *ppOutUpdateInfos = pUpdateInfos;
        }

        UpdateInfoList<UpdateInfo_T> spBufferInfo(new UpdateInfo_T[count]);
        memcpy(spBufferInfo.get(), pUpdateInfos, sizeof(UpdateInfo_T) * count);
        *ppOutUpdateInfos = spBufferInfo.get();
        infoHeap.push_back(std::move(spBufferInfo));

        return AMF_OK;
    }
};

class RenderingPipelineVulkan : public VulkanContext
{
public:
    RenderingPipelineVulkan();
    virtual                         ~RenderingPipelineVulkan();

    virtual AMF_RESULT              Init(amf::AMFVulkanDevice* pDevice, const VulkanImportTable* pImportTable, DescriptorHeapVulkan* pDescriptorHeap);
    AMF_RESULT                      Terminate();

    AMF_RESULT                      RegisterDescriptorSet(amf_uint32 setIndex, amf_uint32 setNum, const amf_uint32* pGroups, amf_uint32 groupCount);

    struct PipelineCreateInfo
    {
        amf::amf_vector<VkPipelineShaderStageCreateInfo>        shaderStages;
        amf::amf_vector<VkVertexInputBindingDescription>        vertexBindingDescs;
        amf::amf_vector<VkVertexInputAttributeDescription>      vertexAttributeDescs;
        VkPipelineInputAssemblyStateCreateInfo                  inputAssemblyInfo;
        amf::amf_vector<VkViewport>                             viewports;
        amf::amf_vector<VkRect2D>                               scissors;
        VkPipelineRasterizationStateCreateInfo                  rasterizationStateInfo;
        VkPipelineMultisampleStateCreateInfo                    multiSamplingInfo;
        amf::amf_vector<VkPipelineColorBlendAttachmentState>    colorBlendAttachments;
        VkPipelineColorBlendStateCreateInfo                     colorBlendInfo;
        VkPipelineDepthStencilStateCreateInfo                   depthStencilState;
        amf::amf_vector<VkDynamicState>                         dynamicStates;
    };

    static AMF_RESULT               SetDefaultInfo(PipelineCreateInfo& createInfo);
    AMF_RESULT                      CreatePipeline(PipelineCreateInfo& createInfo, VkRenderPass hRenderPass, amf_uint32 subpass);
    AMF_RESULT                      SetStates(VkCommandBuffer hCommandBuffer, amf_uint32 groupNum);

private:
    struct DescriptorSetGroup
    {
        amf::amf_vector<amf_uint32>         descriptorSetIndices;
        amf::amf_vector<VkDescriptorSet>    descriptorSetHandles;
    };

    DescriptorHeapVulkan* m_pDescriptorHeap;
    VkRenderPass                            m_hRenderPass;

    VkPipeline                              m_hPipeline;
    VkPipelineLayout                        m_hPipelineLayout;

    typedef std::unordered_map<amf_uint32, DescriptorSetGroup> DescriptorSetGroupMap;
    DescriptorSetGroupMap                   m_descriptorSetGroupMap;

    amf::amf_vector<VkDescriptorSetLayout>  m_hDescriptorSetLayouts;
};


class VideoPresenterVulkan : public BackBufferPresenter, public VulkanContext

{
public:
#if defined(METRO_APP)
    VideoPresenterVulkan(ISwapChainBackgroundPanelNative* pSwapChainPanel, AMFSize swapChainPanelSize, amf::AMFContext* pContext);
#else
    VideoPresenterVulkan(amf_handle hwnd, amf::AMFContext* pContext, amf_handle display);
#endif
    virtual                             ~VideoPresenterVulkan();

    virtual AMF_RESULT                  Present(amf::AMFSurface* pSurface);

    virtual bool                        SupportAllocator() const { return false; }

    virtual amf::AMF_MEMORY_TYPE        GetMemoryType() const { return amf::AMF_MEMORY_VULKAN; }
    virtual amf::AMF_SURFACE_FORMAT     GetInputFormat() const { return m_eInputFormat; }
    virtual AMF_RESULT                  SetInputFormat(amf::AMF_SURFACE_FORMAT format);
    virtual AMF_RESULT                  Flush();

    virtual AMF_RESULT                  Init(amf_int32 width, amf_int32 height, amf::AMFSurface* pSurface);
    virtual AMF_RESULT                  Terminate();

    // amf::AMFDataAllocatorCB interface
    virtual AMF_RESULT AMF_STD_CALL     AllocSurface(amf::AMF_MEMORY_TYPE type, amf::AMF_SURFACE_FORMAT format, amf_int32 width,
                                                     amf_int32 height, amf_int32 hPitch, amf_int32 vPitch, amf::AMFSurface** ppSurface);
    // amf::AMFSurfaceObserver interface
    virtual void AMF_STD_CALL           OnSurfaceDataRelease(amf::AMFSurface* pSurface);

    virtual AMFSize                     GetSwapchainSize();
    virtual void AMF_STD_CALL           ResizeIfNeeded(); // call from UI thread (for VulkanPresenter on Linux)

protected:
    typedef SwapChainVulkan::BackBuffer RenderTarget;

    struct Vertex
    {
        amf_float pos[3];
        amf_float tex[2];
    };

    struct ViewProjection
    {
        amf_float vertexTransformMatrix[4][4];
        amf_float texTransformMatrix[4][4];
    };

    struct PipelineGroupBindingInfo
    {
        RenderingPipelineVulkan*      pPipeline;
        amf_uint32              setNum;
        const amf_uint32*       pGroups;
        amf_uint32              groupCount;
    };

    AMF_RESULT                          RegisterDescriptorSet(DescriptorVulkan** ppDescriptors, amf_uint32 count, const PipelineGroupBindingInfo* pPipelineBindings, amf_uint32 piplineBindingCount);
    AMF_RESULT                          RegisterDescriptorSet(DescriptorVulkan** ppDescriptors, amf_uint32 count, RenderingPipelineVulkan* pPipeline, amf_uint32 setNum, const amf_uint32* pGroups, amf_uint32 groupCount);
    virtual AMF_RESULT                  RegisterDescriptorSets(DescriptorVulkan& viewProjectionDescriptor, DescriptorVulkan& samplerDescriptor, RenderingPipelineVulkan& pipeline);
    
    AMF_RESULT                          CreateShaderFromFile(const wchar_t* pFileName, VkShaderModule* pShaderModule);
    AMF_RESULT                          GetShaderStageInfoFromFile(const wchar_t* pFileName, VkShaderStageFlagBits stage, const char* pEntryPoint, VkPipelineShaderStageCreateInfo& createInfo);
    virtual AMF_RESULT                  GetShaderStages(amf::amf_vector<VkPipelineShaderStageCreateInfo>& shaderStages);
    virtual AMF_RESULT                  DestroyShaderStages(amf::amf_vector<VkPipelineShaderStageCreateInfo>& shaderStages);

    AMF_RESULT                          CreateVertexBuffer(const void* pData, amf_size size, amf::AMFVulkanBuffer& buffer);
    AMF_RESULT                          CreateBufferForDescriptor(const DescriptorVulkan* pDescriptor, const void* pData, amf_size bufferSize, amf_size bindingOffset, amf_size bindingSize, amf_uint arrayIndex, amf::AMFVulkanBuffer& buffer);
    AMF_RESULT                          CreateBufferForDescriptor(const DescriptorVulkan* pDescriptor, const void* pData, amf_size bufferSize, amf_uint arrayIndex, amf::AMFVulkanBuffer& buffer);

    AMF_RESULT                          CreateRenderTarget(RenderTarget& renderTarget);
    AMF_RESULT                          DestroyRenderTarget(RenderTarget& renderTarget);

    virtual AMF_RESULT                  UpdateTextureDescriptorSet(DescriptorVulkan* pDescriptor, amf::AMFSurface * pSurface, VkSampler hSampler);
    virtual AMF_RESULT                  UpdateTextureDescriptorSet(amf::AMFSurface* pSurface);

    virtual AMF_RESULT                  OnRenderViewResize(const RenderViewSizeInfo& newRenderView) override;

    AMF_RESULT                          RenderSurface(amf::AMFSurface* pSurface, const RenderTarget* pRenderTarget);
    virtual AMF_RESULT                  DrawBackground();
    virtual AMF_RESULT                  SetStates(amf_uint32 descriptorGroupNum);
    AMF_RESULT                          DrawFrame(amf::AMFVulkanSurface* pSurface);
    virtual AMF_RESULT                  DrawOverlay(amf::AMFSurface* pSurface);

    VkFormat                            GetVkFormat();

    virtual AMFRect                     GetVertexViewRect() const override  { return AMFConstructRect(-1, -1, 1, 1); }
    virtual AMFRect                     GetTextureViewRect() const override { return AMFConstructRect(0, 1, 1, 0); }

    static const amf_uint32 DSG_PRESENT_SURFACE = 0;

    amf::AMFContext1Ptr                 m_pContext1;
    SwapChainVulkan                     m_swapChain;
    CommandBufferVulkan                 m_cmdBuffer;
    DescriptorHeapVulkan                m_descriptorHeap;
    amf::AMFVulkanBuffer                m_viewProjectionBuffer;

private:
    AMF_RESULT                          InitDescriptors();
    AMF_RESULT                          CreatePipeline();
    AMF_RESULT                          PrepareStates();

    AMF_RESULT                          CheckForResize(amf_bool force);
    AMF_RESULT                          ResizeSwapChain();

    AMF_RESULT                          BitBltRender(amf::AMFSurface* pSrcSurface, const RenderTarget* pDstSurface, RenderViewSizeInfo& renderView);
    AMF_RESULT                          BitBltCopy(amf::AMFSurface* pSrcSurface, const RenderTarget* pDstSurface, RenderViewSizeInfo& renderView);
    AMF_RESULT                          StartRendering(const RenderTarget* pRenderTarget);
    AMF_RESULT                          StopRendering();

    amf::AMF_SURFACE_FORMAT             m_eInputFormat;

    RenderingPipelineVulkan             m_pipeline;

    amf::AMFVulkanBuffer                m_VertexBuffer;
    DescriptorVulkan                    m_viewProjectionDescriptor;

    DescriptorVulkan                    m_samplerDescriptor;
    VkSampler                           m_hSampler;

    amf::AMFCriticalSection                 m_sect;
    amf::amf_vector<amf::AMFSurface*>       m_TrackSurfaces; // raw pointer  doent want keep references to ensure object is destroying

    bool                                    m_bResizeSwapChain;
};