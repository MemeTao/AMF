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
#include "VideoPresenterVulkan.h"
#include "../common/CmdLogger.h"
#include "public/common/ByteArray.h"
#include "public/common/TraceAdapter.h"
#include "public/include/core/Compute.h"
#include <iostream>
#include <fstream>

using namespace amf;

//#define VK_NO_PROTOTYPES
#if defined(_WIN32)
#else
   #include <unistd.h>
#endif

#define AMF_FACILITY L"VideoPresenterVulkan"

VideoPresenterVulkan::VideoPresenterVulkan(amf_handle hwnd, AMFContext* pContext, amf_handle display) :
    BackBufferPresenter(hwnd, pContext, display),
    m_pContext1(pContext),
    m_swapChain(pContext),
    m_eInputFormat(AMF_SURFACE_BGRA),
    m_viewProjectionBuffer{},
    m_VertexBuffer{},
    m_hSampler(NULL),
    m_bResizeSwapChain(false)
{
}

VideoPresenterVulkan::~VideoPresenterVulkan()
{
    Terminate();
}

AMF_RESULT VideoPresenterVulkan::Init(amf_int32 width, amf_int32 height, AMFSurface* /*pSurface*/)
{
    AMF_RETURN_IF_FALSE(width > 0 && height > 0, AMF_INVALID_ARG, L"Init() - Invalid width/height: width=%d height=%d", width, height);

    AMF_RESULT res = VideoPresenter::Init(width, height);
    AMF_RETURN_IF_FAILED(res, L"Init() - VideoPresenter::Init() failed");

    res = m_swapChain.Init(m_hwnd, m_hDisplay, nullptr, width, height, GetInputFormat());
    AMF_RETURN_IF_FAILED(res, L"Init() - m_swapChain Init() failed");

    res = VulkanContext::Init((VulkanContext*)&m_swapChain);
    AMF_RETURN_IF_FAILED(res, L"Init() - VulkanContext Init() failed");

    res = m_descriptorHeap.Init(m_pVulkanDevice,  GetVulkan());
    AMF_RETURN_IF_FAILED(res, L"Init() - m_descriptorHeap.Init() failed");

    res = m_pipeline.Init(m_pVulkanDevice, GetVulkan(), &m_descriptorHeap);

    res = InitDescriptors();
    AMF_RETURN_IF_FAILED(res, L"Init() - InitDescriptors() failed");

    res = RegisterDescriptorSets(m_viewProjectionDescriptor, m_samplerDescriptor, m_pipeline);
    AMF_RETURN_IF_FAILED(res, L"Init() - RegisterDescriptorSet() failed");

    res = m_descriptorHeap.CreateDescriptors();
    AMF_RETURN_IF_FAILED(res, L"Init() - CreateDescriptors() failed");

    res = CreatePipeline();
    AMF_RETURN_IF_FAILED(res, L"Init() - CreatePipeline() failed");

    res = m_cmdBuffer.Init(m_pVulkanDevice, GetVulkan(), m_swapChain.GetCmdPool());
    AMF_RETURN_IF_FAILED(res, L"Init() - Command Buffer Init() failed");

    res = PrepareStates();
    AMF_RETURN_IF_FAILED(res, L"Init() - PrepareStates() failed");

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::Terminate()
{
    if (m_pVulkanDevice == nullptr)
    {
        return AMF_OK;
    }

    GetVulkan()->vkDeviceWaitIdle(m_pVulkanDevice->hDevice);

    m_descriptorHeap.Terminate();
    m_pipeline.Terminate();

    m_samplerDescriptor = {};
    if (m_hSampler != NULL)
    {
		GetVulkan()->vkDestroySampler(m_pVulkanDevice->hDevice, m_hSampler, nullptr);
        m_hSampler = NULL;
    }

    DestroyBuffer(m_viewProjectionBuffer);
    m_viewProjectionDescriptor = {};

    DestroyBuffer(m_VertexBuffer);

    m_bResizeSwapChain = false;

    m_cmdBuffer.Terminate();
    m_swapChain.Terminate();
    VulkanContext::Terminate();
    return VideoPresenter::Terminate();
}

AMF_RESULT VideoPresenterVulkan::RegisterDescriptorSet(DescriptorVulkan** ppDescriptors, amf_uint32 count, const PipelineGroupBindingInfo* pPipelineBindings, amf_uint32 piplineBindingCount)
{
    // All descriptor sets that need to be used with the pipeline
    // can be registered here. When a descriptor set is register
    // a space will be allocated for it on the descriptor pool
    // and the descriptor set will be allocated on the pool
    // The layout is also allocated. The caller SHOULD NOT
    // worry about deleting the layout and descriptor set

    AMF_RETURN_IF_FALSE(ppDescriptors != nullptr, AMF_INVALID_ARG, L"RegisterDescriptorSet() - ppDescriptors is NULL");
    AMF_RETURN_IF_FALSE(count > 0, AMF_INVALID_ARG, L"RegisterDescriptorSet() - descriptor count cannot be 0");

    AMF_RESULT res = m_descriptorHeap.RegisterDescriptorSet(ppDescriptors, count);
    AMF_RETURN_IF_FAILED(res, L"RegisterDescriptorSet() - m_descriptorHeap.RegisterDescriptorSet() failed");

    if (pPipelineBindings != nullptr)
    {
        for (amf_uint32 i = 0; i < piplineBindingCount; ++i)
        {
            const PipelineGroupBindingInfo& bindingInfo = pPipelineBindings[i];
            AMF_RETURN_IF_FALSE(bindingInfo.pPipeline != nullptr, AMF_INVALID_ARG, L"RegisterDescriptorSet() - pPipelineBindings[%u].pPipeline is NULL", i);

            res = bindingInfo.pPipeline->RegisterDescriptorSet(ppDescriptors[0]->setIndex, bindingInfo.setNum, bindingInfo.pGroups, bindingInfo.groupCount);
            AMF_RETURN_IF_FAILED(res, L"RegisterDescriptorSet() - m_pipeline.RegisterDescriptorSet() failed");
        }
    }

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::RegisterDescriptorSet(DescriptorVulkan** ppDescriptors, amf_uint32 count, RenderingPipelineVulkan* pPipeline, amf_uint32 setNum, const amf_uint32* pGroups, amf_uint32 groupCount)
{
    PipelineGroupBindingInfo info = {};
    info.pPipeline = pPipeline;
    info.setNum = setNum;
    info.groupCount = groupCount;
    info.pGroups = pGroups;

    return RegisterDescriptorSet(ppDescriptors, count, &info, 1);
}

AMF_RESULT VideoPresenterVulkan::InitDescriptors()
{
    m_viewProjectionDescriptor = {};
    VkDescriptorSetLayoutBinding& viewProjectionBinding = m_viewProjectionDescriptor.layoutBinding;
        viewProjectionBinding.binding = 0;
        viewProjectionBinding.descriptorCount = 1;
        viewProjectionBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        viewProjectionBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    m_samplerDescriptor = {};
    VkDescriptorSetLayoutBinding& samplerLayoutBinding = m_samplerDescriptor.layoutBinding;
        samplerLayoutBinding.binding = 1;
        samplerLayoutBinding.descriptorCount = 1;
        samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; //VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
        samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::RegisterDescriptorSets(DescriptorVulkan& viewProjectionDescriptor, DescriptorVulkan& samplerDescriptor, RenderingPipelineVulkan& pipeline)
{
    DescriptorVulkan* pDescriptors[] = { &viewProjectionDescriptor, &samplerDescriptor };
    constexpr amf_uint32 groups[] = { DSG_PRESENT_SURFACE };

    AMF_RESULT res = RegisterDescriptorSet(pDescriptors, amf_countof(pDescriptors), &pipeline, 0, groups, amf_countof(groups));
    AMF_RETURN_IF_FAILED(res, L"RegisterDescriptorSet() - RegisterDescriptorSet() failed");

    return AMF_OK;
}

static AMF_RESULT LoadShaderFile(const wchar_t* pFileName, AMFByteArray &data)
{
    AMF_RETURN_IF_FALSE(pFileName != nullptr, AMF_INVALID_ARG, L"LoadShaderFile() - pFileName is NULL");
    AMF_RETURN_IF_FALSE(pFileName[0] != L'\0', AMF_INVALID_ARG, L"LoadShaderFile() - pFileName is empty");

#if defined(_WIN32)

    wchar_t path[5 * 1024];
    ::GetModuleFileNameW(NULL, path, amf_countof(path));
    std::wstring filepath(path);
    std::wstring::size_type slash = filepath.find_last_of(L"\\/");
    filepath = filepath.substr(0, slash + 1);
    std::wstring fileName = filepath + pFileName;

#elif defined(__linux)

    char path[5 * 1024] = {0};
    ssize_t len = readlink("/proc/self/exe", path, amf_countof(path));
    std::string filepath(path);
    std::string::size_type slash = filepath.find_last_of("\\/");
    filepath = filepath.substr(0, slash + 1);
    std::string fileName = filepath + amf_from_unicode_to_utf8(pFileName).c_str();

#else

    std::wstring fileName = pFileName;

#endif

    //Load data from file
    std::ifstream fs(fileName, std::ios::ate | std::ios::binary);
    AMF_RETURN_IF_FALSE(fs.is_open(), AMF_FILE_NOT_OPEN, L"LoadShaderFile() - Failed to open file %S", fileName.c_str());

    size_t fileSize = (size_t)fs.tellg();
    data.SetSize(fileSize);
    fs.seekg(0);
    fs.read((char*)data.GetData(), fileSize);
    fs.close();
    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::CreateShaderFromFile(const wchar_t* pFileName, VkShaderModule* pShaderModule)
{
    AMF_RETURN_IF_FALSE(pShaderModule != nullptr, AMF_INVALID_ARG, L"LoadShaderFile() - pShaderModule is NULL");
    AMF_RETURN_IF_FALSE(*pShaderModule == NULL, AMF_ALREADY_INITIALIZED, L"LoadShaderFile() - pShaderModule is already initialized");
    AMF_RETURN_IF_FALSE(m_pVulkanDevice != nullptr, AMF_NOT_INITIALIZED, L"LoadShaderFile() - m_pVulkanDevice is not initialized");
    AMF_RETURN_IF_FALSE(m_pVulkanDevice->hDevice != nullptr, AMF_NOT_INITIALIZED, L"LoadShaderFile() - m_pVulkanDevice->hDevice is not initialized");

    AMFByteArray shaderByteArray;
    AMF_RESULT res = LoadShaderFile(pFileName, shaderByteArray);
    AMF_RETURN_IF_FAILED(res, L"CreateShaderFromFile() - LoadShaderFile(%S) failed", pFileName);

    VkShaderModuleCreateInfo vertModuleCreateInfo = {};
    vertModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertModuleCreateInfo.codeSize = shaderByteArray.GetSize();
    vertModuleCreateInfo.pCode = (amf_uint32*)shaderByteArray.GetData();
    vertModuleCreateInfo.flags = 0; // Reserved for future use

    VkResult vkres = GetVulkan()->vkCreateShaderModule(m_pVulkanDevice->hDevice, &vertModuleCreateInfo, nullptr, pShaderModule);
    ASSERT_RETURN_IF_VK_FAILED(vkres, AMF_VULKAN_FAILED, L"CreateShaderFromFile() - vkCreateShaderModule(%s)", pFileName);
    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::GetShaderStageInfoFromFile(const wchar_t* pFileName, VkShaderStageFlagBits stage, const char* pEntryPoint, VkPipelineShaderStageCreateInfo& stageCreateInfo)
{
    VkShaderModule shaderModule = NULL;
    AMF_RESULT res = CreateShaderFromFile(pFileName, &shaderModule);
    AMF_RETURN_IF_FAILED(res, L"GetShaderInfoFromFile() - CreateShaderFromFile() failed to create shader module");

    stageCreateInfo = {};
    stageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageCreateInfo.stage = stage;
    stageCreateInfo.module = shaderModule;
    stageCreateInfo.pName = pEntryPoint;
    stageCreateInfo.flags = 0;
    stageCreateInfo.pSpecializationInfo = nullptr;

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::GetShaderStages(amf_vector<VkPipelineShaderStageCreateInfo>& shaderStages)
{
    const wchar_t* pCubeShaderFileNameVert = L"quad.vert.spv";
    const wchar_t* pCubeShaderFileNameFraq = L"quad.frag.spv";

    shaderStages.resize(2);
    AMF_RESULT res = GetShaderStageInfoFromFile(pCubeShaderFileNameVert, VK_SHADER_STAGE_VERTEX_BIT, "main", shaderStages[0]);
    AMF_RETURN_IF_FAILED(res, L"GetShaderStages() - GetShaderStageInfoFromFile() failed to get vertex shader stage create info");

    res = GetShaderStageInfoFromFile(pCubeShaderFileNameFraq, VK_SHADER_STAGE_FRAGMENT_BIT, "main", shaderStages[1]);
    AMF_RETURN_IF_FAILED(res, L"GetShaderStages() - GetShaderStageInfoFromFile() failed to get fragment shader stage create info");

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::DestroyShaderStages(amf_vector<VkPipelineShaderStageCreateInfo>& shaderStages)
{
    AMF_RETURN_IF_FALSE(m_pVulkanDevice != nullptr, AMF_NOT_INITIALIZED, L"DestroyShaderStages() - m_pVulkanDevice is not initialized");
    AMF_RETURN_IF_FALSE(m_pVulkanDevice->hDevice != nullptr, AMF_NOT_INITIALIZED, L"DestroyShaderStages() - m_pVulkanDevice->hDevice is not initialized");

    for (VkPipelineShaderStageCreateInfo& stage : shaderStages)
    {
        if (stage.module != NULL)
        {
            GetVulkan()->vkDestroyShaderModule(m_pVulkanDevice->hDevice, stage.module, nullptr);
        }
    }

    shaderStages.clear();

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::CreateVertexBuffer(const void* pData, amf_size size, AMFVulkanBuffer& buffer)
{
    AMF_RESULT res = MakeBuffer(pData, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer);
    AMF_RETURN_IF_FAILED(res, L"CreateVertexBuffers() - MakeBuffer() failed to create vertex buffer");
    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::CreateBufferForDescriptor(const DescriptorVulkan* pDescriptor, const void* pData, amf_size bufferSize, amf_size bindingOffset, amf_size bindingSize, amf_uint arrayIndex, AMFVulkanBuffer& buffer)
{
    AMF_RETURN_IF_FALSE(pDescriptor != nullptr, AMF_INVALID_ARG, L"CreateBufferForDescriptor() - pDescriptor is NULL");
    AMF_RETURN_IF_FALSE(pDescriptor->layoutBinding.descriptorCount > 0, AMF_INVALID_ARG, L"CreateBufferForDescriptor() - descriptor set descriptor count cannot be 0");
    AMF_RETURN_IF_FALSE(pDescriptor->layoutBinding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || 
                        pDescriptor->layoutBinding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 
                        AMF_INVALID_ARG, L"CreateBufferForDescriptor() - descriptor is not a uniform buffer descriptor");

    AMF_RETURN_IF_FALSE(bindingSize > 0, AMF_INVALID_ARG, L"CreateBufferForDescriptor() - bindingSize must be greater than 0");
    AMF_RETURN_IF_FALSE(bindingSize + bindingOffset <= bufferSize, AMF_OUT_OF_RANGE, 
        L"CreateBufferForDescriptor() - bindingSize (%zu) + bindingOffset (%zu) must be <= bufferSize (%zu)", bindingSize, bindingOffset, bufferSize);

    AMF_RESULT res = MakeBuffer(pData, bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer);
    AMF_RETURN_IF_FAILED(res, L"CreateBufferForDescriptor() - MakeBuffer() failed");

    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = buffer.hBuffer;
    bufferInfo.offset = bindingOffset;
    bufferInfo.range = bindingSize;

    res = m_descriptorHeap.UpdateDescriptorSet(pDescriptor, arrayIndex, 1, &bufferInfo, false);
    if (res != AMF_OK)
    {
        DestroyBuffer(buffer);
    }
    AMF_RETURN_IF_FAILED(res, L"CreateBufferForDescriptor() - UpdateDescriptorSet() failed");

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::CreateBufferForDescriptor(const DescriptorVulkan* pDescriptor, const void* pData, amf_size bufferSize, amf_uint arrayIndex, AMFVulkanBuffer& buffer)
{
    return CreateBufferForDescriptor(pDescriptor, pData, bufferSize, 0, bufferSize, arrayIndex, buffer);
}

AMF_RESULT VideoPresenterVulkan::CreatePipeline()
{
    AMF_RETURN_IF_FALSE(m_pVulkanDevice != nullptr, AMF_NOT_INITIALIZED, L"CreatePipeline() - m_pVulkanDevice is not initialized");
    AMF_RETURN_IF_FALSE(m_pVulkanDevice->hDevice != nullptr, AMF_NOT_INITIALIZED, L"CreatePipeline() - m_pVulkanDevice->hDevice is not initialized");
    AMF_RETURN_IF_FALSE(m_swapChain.GetRenderPass() != NULL, AMF_NOT_INITIALIZED, L"CreatePipeline() - Render Pass is not initialized");

    RenderingPipelineVulkan::PipelineCreateInfo createInfo;
    RenderingPipelineVulkan::SetDefaultInfo(createInfo);

    // IMPORTANT: The shader info created below contain the shader
    // module which needs to be destroyed after creating the pipeline
    // Cannot let vector go out of scope before calling DestroyShaderStages()
    // Todo: it would be safer to write custom vector allocator or keep stage info
    // as class member to delete in terminate
    AMF_RESULT res = GetShaderStages(createInfo.shaderStages);
    AMF_RETURN_IF_FAILED(res, L"CreatePipeline() - GetShaderStages() failed");

    // Vertex bindings
    createInfo.vertexBindingDescs.resize(1);
    createInfo.vertexBindingDescs[0].binding = 0;
    createInfo.vertexBindingDescs[0].stride = sizeof(Vertex);
    createInfo.vertexBindingDescs[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // Vertex attributes
    createInfo.vertexAttributeDescs.resize(2);

        //Position
        createInfo.vertexAttributeDescs[0].binding = 0;
        createInfo.vertexAttributeDescs[0].location = 0;
        createInfo.vertexAttributeDescs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        createInfo.vertexAttributeDescs[0].offset = offsetof(Vertex, pos);

        //Texture
        createInfo.vertexAttributeDescs[1].binding = 0;
        createInfo.vertexAttributeDescs[1].location = 1;
        createInfo.vertexAttributeDescs[1].format = VK_FORMAT_R32G32_SFLOAT;
        createInfo.vertexAttributeDescs[1].offset = offsetof(Vertex, tex);

    AMF_RESULT createRes = m_pipeline.CreatePipeline(createInfo, m_swapChain.GetRenderPass(), 0);

    // Destroy shader stages regardless of pipeline creation result
    res = DestroyShaderStages(createInfo.shaderStages);
    AMF_RETURN_IF_FAILED(res, L"CreatePipeline() - DestroyShaderStages() failed");

    AMF_RETURN_IF_FAILED(createRes, L"CreatePipeline() - m_pipeline.CreatePipeline() failed");
    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::PrepareStates()
{
    AMF_RETURN_IF_FALSE(m_pVulkanDevice != nullptr, AMF_NOT_INITIALIZED, L"PrepareStates() - m_pVulkanDevice is not initialized");
    AMF_RETURN_IF_FALSE(m_pVulkanDevice->hDevice != nullptr, AMF_NOT_INITIALIZED, L"PrepareStates() - m_pVulkanDevice->hDevice is not initialized");
    AMF_RETURN_IF_FALSE(m_hSampler == NULL, AMF_ALREADY_INITIALIZED, L"PrepareStates() - m_hSampler is already initialized");

    // Vertices
    constexpr Vertex vertices[4] =
    {
        { {  0.0f,  1.0f, 0.0f },   { 0.0f, 0.0f } }, // Top left
        { {  1.0f,  1.0f, 0.0f },   { 1.0f, 0.0f } }, // top right
        { {  0.0f,  0.0f, 0.0f },   { 0.0f, 1.0f } }, // bottom left
        { {  1.0f,  0.0f, 0.0f },   { 1.0f, 1.0f } }, // bottom right
    };

    AMF_RESULT res = CreateVertexBuffer(vertices, sizeof(vertices), m_VertexBuffer);
    AMF_RETURN_IF_FAILED(res, L"PrepareStates() - CreateVertexBuffer() failed to create vertex buffer");

    // View projection buffer
    constexpr ViewProjection mvp = {};
    res = CreateBufferForDescriptor(&m_viewProjectionDescriptor, &mvp, sizeof(ViewProjection), 0, m_viewProjectionBuffer);
    AMF_RETURN_IF_FAILED(res, L"PrepareStates() - CreateBufferForDescriptor() failed to create view projection buffer");

    // Sampler
    VkSamplerCreateInfo sampler = {};
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeV = sampler.addressModeU;
        sampler.addressModeW = sampler.addressModeU;
        sampler.mipLodBias = 0.0f;
        sampler.compareOp = VK_COMPARE_OP_NEVER;
        sampler.minLod = 0.0f;
        sampler.maxLod = 0.0f;
        sampler.maxAnisotropy = 1.0f;
        sampler.borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE; //VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sampler.unnormalizedCoordinates = VK_FALSE;
        sampler.compareEnable = VK_FALSE;

    VkResult vkres = GetVulkan()->vkCreateSampler(m_pVulkanDevice->hDevice, &sampler, nullptr, &m_hSampler);
    ASSERT_RETURN_IF_VK_FAILED(vkres, AMF_VULKAN_FAILED, L"PrepareStates() - vkCreateSampler() failed");

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::CreateRenderTarget(RenderTarget& renderTarget)
{
    const AMFSize size = GetSwapchainSize();
    const VkFormat format = GetVkFormat();

    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    constexpr VkMemoryPropertyFlags memoryProperties = 0;// VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    AMF_RESULT res = CreateSurface(m_swapChain.GetQueueIndex(), size.width, size.height, format, usage, memoryProperties, renderTarget.m_surface);
    AMF_RETURN_IF_FAILED(res, L"CreateRenderTarget() - CreateSurface() failed");

    res = m_swapChain.CreateImageView(renderTarget.m_surface.hImage, format, renderTarget.m_hImageView);
    AMF_RETURN_IF_FAILED(res, L"CreateRenderTarget() - CreateImageView() failed");

    VkFramebufferCreateInfo frameBufferInfo = {};
    frameBufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    frameBufferInfo.renderPass = m_swapChain.GetRenderPass();
    frameBufferInfo.attachmentCount = 1;
    frameBufferInfo.pAttachments = &renderTarget.m_hImageView;
    frameBufferInfo.width = size.width;
    frameBufferInfo.height = size.height;
    frameBufferInfo.layers = 1;

    VkResult vkres = GetVulkan()->vkCreateFramebuffer(m_pVulkanDevice->hDevice, &frameBufferInfo, nullptr, &renderTarget.m_hFrameBuffer);
    ASSERT_RETURN_IF_VK_FAILED(vkres, AMF_VULKAN_FAILED, L"CreateRenderTarget() - vkCreateFramebuffer() failed");

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::DestroyRenderTarget(RenderTarget& renderTarget)
{
    AMF_RESULT res = DestroySurface(renderTarget.m_surface);
    AMF_RETURN_IF_FAILED(res, L"DestroyRenderTarget() - DestroySurface() failed");

    if (renderTarget.m_hFrameBuffer != NULL)
    {
        GetVulkan()->vkDestroyFramebuffer(m_pVulkanDevice->hDevice, renderTarget.m_hFrameBuffer, nullptr);
    }
    if (renderTarget.m_hImageView != NULL)
    {
        GetVulkan()->vkDestroyImageView(m_pVulkanDevice->hDevice, renderTarget.m_hImageView, nullptr);
    }

    renderTarget = {};

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::UpdateTextureDescriptorSet(DescriptorVulkan* pDescriptor, AMFSurface* pSurface, VkSampler hSampler)
{
    AMF_RETURN_IF_FALSE(pDescriptor != nullptr, AMF_INVALID_ARG, L"UpdateTextureDescriptorSet() - pDescriptor is NULL");

    AMF_RETURN_IF_FALSE(hSampler != NULL, AMF_INVALID_ARG, L"UpdateTextureDescriptorSet() - hSampler is NULL");
    AMF_RETURN_IF_FALSE(pSurface != nullptr, AMF_INVALID_ARG, L"UpdateTextureDescriptorSet() - pSurface is NULL");

    AMFPlane* pPlane = pSurface->GetPlane(AMF_PLANE_PACKED);
    AMF_RETURN_IF_FALSE(pSurface != nullptr, AMF_INVALID_ARG, L"UpdateTextureDescriptorSet() - Packed plane doesn't exist");

    const AMFVulkanView* pView = (AMFVulkanView*)pPlane->GetNative();
    AMF_RETURN_IF_FALSE(pView != nullptr, AMF_INVALID_ARG, L"UpdateTextureDescriptorSet() - Plane GetNative() returned NULL");
    AMF_RETURN_IF_FALSE(pView->hView != NULL, AMF_INVALID_ARG, L"UpdateTextureDescriptorSet() - image view is NULL");

    VkDescriptorImageInfo imageInfo = {};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;//(VkImageLayout)pView->pSurface->eCurrentLayout;
        imageInfo.imageView = pView->hView;
        imageInfo.sampler = hSampler;

    AMF_RESULT res = m_descriptorHeap.UpdateDescriptorSet(pDescriptor, 0, 1, &imageInfo, false);
    AMF_RETURN_IF_FAILED(res, L"UpdateTextureDescriptorSet() - UpdateDescriptorSet() failed");

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::UpdateTextureDescriptorSet(AMFSurface* pSurface)
{
    // This function will be used later in PresentEngine for demo frame and interpolation mode
    return UpdateTextureDescriptorSet(&m_samplerDescriptor, pSurface, m_hSampler);
}

AMF_RESULT VideoPresenterVulkan::Present(AMFSurface* pSurface)
{
    AMF_RETURN_IF_FALSE(m_pVulkanDevice != nullptr, AMF_NOT_INITIALIZED, L"Present() - m_pVulkanDevice is not initialized");
    AMF_RETURN_IF_FALSE(pSurface != nullptr, AMF_INVALID_ARG, L"Present() - pSurface is NULL");
    AMF_RETURN_IF_FALSE(pSurface->GetFormat() == GetInputFormat(), AMF_INVALID_FORMAT, L"Present() - Surface format (%s)"
        "does not match input format (%s)", AMFSurfaceGetFormatName(pSurface->GetFormat()),
        AMFSurfaceGetFormatName(GetInputFormat()));

    AMF_RESULT res = pSurface->Convert(GetMemoryType());
    AMF_RETURN_IF_FAILED(res, L"Present() - Surface Convert() failed");

    AMFLock lock(&m_sect);
    amf_uint32 imageIndex = 0;

#if defined(_WIN32)
    res = CheckForResize(false);
    AMF_RETURN_IF_FAILED(res, L"Present() - CheckForResize() failed");
#endif


    // Get index corresponding to the back buffer of incoming frame
    if (m_bRenderToBackBuffer)
    {
        res = m_swapChain.GetBackBufferIndex(pSurface, imageIndex);
        AMF_RETURN_IF_FALSE(res == AMF_OK || res == AMF_NOT_FOUND, res, L"Present() - CheckBackBufferIndex() failed");
    }

    if(m_bRenderToBackBuffer == false || res == AMF_NOT_FOUND)
    {
#if defined(_WIN32)
        if(m_bResizeSwapChain)
        {
            res = ResizeSwapChain();
            AMF_RETURN_IF_FAILED(res, L"Present() - ResizeSwapChain() failed");
        }
#endif

        const BackBufferBase* pBuffer = nullptr;
        res = m_swapChain.AcquireNextBackBuffer(&pBuffer);
        AMF_RETURN_IF_FAILED(res, L"Present() - AcquireBackBuffer() failed");

        const RenderTarget* pRenderTarget = (RenderTarget*)pBuffer;
        res = RenderSurface(pSurface, pRenderTarget);
        if (res != AMF_OK)
        {
            AMF_RETURN_IF_FAILED(m_swapChain.DropBackBuffer(pBuffer), L"Present() - DropBackBuffer() failed");
        }

        AMF_RETURN_IF_FAILED(res, L"Present() - RenderSurface() failed");
    }

    WaitForPTS(pSurface->GetPts());

    res = m_swapChain.Present(m_bWaitForVSync);
    AMF_RETURN_IF_FAILED(res, L"Present() - SwapChainVulkan::Present() failed");
    
#if defined(__linux)
    XFlush((Display*)m_hDisplay);
#endif

    if (m_bResizeSwapChain)
    {
        return AMF_RESOLUTION_UPDATED;
    }

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::RenderSurface(AMFSurface* pSurface, const RenderTarget* pRenderTarget)
{
    AMF_RETURN_IF_FALSE(pSurface != nullptr, AMF_INVALID_ARG, L"RenderSurface() - pSurface is NULL");
    AMF_RETURN_IF_FALSE(pSurface->GetFormat() == GetInputFormat(), AMF_INVALID_FORMAT, L"Present() - Surface format (%s)"
        "does not match input format (%s)", AMFSurfaceGetFormatName(pSurface->GetFormat()),
        AMFSurfaceGetFormatName(GetInputFormat()));

    AMFContext1::AMFVulkanLocker vklock(m_pContext1);
    AMFLock lock(&m_sect);

    const AMFRect srcSurfaceRect = GetPlaneRect(pSurface->GetPlane(AMF_PLANE_PACKED));
    const AMFRect dstSurfaceRect = GetClientRect();
    const AMFSize dstSurfaceSize = AMFConstructSize(pRenderTarget->m_surface.iWidth, pRenderTarget->m_surface.iHeight);

    RenderViewSizeInfo renderView;
    AMF_RESULT res = GetRenderViewSizeInfo(srcSurfaceRect, dstSurfaceSize, dstSurfaceRect, renderView);
    AMF_RETURN_IF_FAILED(res, L"RenderSurface() - GetRenderViewSizeInfo() failed")

    res = BitBltRender(pSurface, pRenderTarget, renderView);
    AMF_RETURN_IF_FAILED(res, L"RenderSurface() - BitBlt() failed");

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::BitBltCopy(AMFSurface* pSrcSurface, const RenderTarget* pRenderTarget, RenderViewSizeInfo& renderView)
{
    AMFSurfacePtr pSwapChainSurface;
    AMF_RESULT res = m_pContext1->CreateSurfaceFromVulkanNative((void*)&(pRenderTarget->m_surface), &pSwapChainSurface, NULL);
    AMF_RETURN_IF_FAILED(res, L"BitBltCopy() - CreateSurfaceFromVulkanNative() failed");

    AMFComputePtr pCompute;
    m_pContext1->GetCompute(AMF_MEMORY_VULKAN, &pCompute);
    
    const amf_size originSrc[3] = { (amf_size)renderView.srcRect.left,      (amf_size)renderView.srcRect.top, 0};
    const amf_size originDst[3] = { (amf_size)renderView.dstRect.left,      (amf_size)renderView.dstRect.top, 0};
    const amf_size region[3] =    { (amf_size)renderView.dstRect.Width(),   (amf_size)renderView.dstRect.Height(), 1};

    res = pCompute->CopyPlane(pSrcSurface->GetPlaneAt(0), originSrc, region, pSwapChainSurface->GetPlaneAt(0), originDst);
    AMF_RETURN_IF_FAILED(res, L"BitBltCopy() - CopyPlane() failed");
    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::BitBltRender(AMFSurface* pSrcSurface, const RenderTarget* pRenderTarget, RenderViewSizeInfo& renderView)
{   
    AMF_RETURN_IF_FALSE(pSrcSurface != nullptr, AMF_INVALID_ARG, L"BitBltRender() - pSrcSurface is NULL");
    AMF_RETURN_IF_FALSE(pRenderTarget != nullptr, AMF_INVALID_ARG, L"BitBltRender() - pRenderTarget is NULL");

    AMFPlane * pSrcPlane = pSrcSurface->GetPlane(AMF_PLANE_PACKED);
    AMF_RETURN_IF_FALSE(pSrcSurface != nullptr, AMF_INVALID_ARG, L"BitBltRender() - Packed plane doesn't exist in SrcSurface");

    const AMFVulkanView* pSrcView = (AMFVulkanView*)pSrcPlane->GetNative();
    AMF_RETURN_IF_FALSE(pSrcView != nullptr, AMF_INVALID_ARG, L"BitBltRender() - SrcPlane GetNative() returned NULL");

    AMF_RESULT res = ResizeRenderView(renderView);
    AMF_RETURN_IF_FAILED(res, L"BitBltRender() - ResizeRenderView() failed");

    res = UpdateTextureDescriptorSet(pSrcSurface);
    AMF_RETURN_IF_FAILED(res, L"BitBltRender() - UpdateTextureDescriptorSet() failed");

    res = m_descriptorHeap.UpdatePendingDescriptorSets();
    AMF_RETURN_IF_FAILED(res, L"BitBltRender() - UpdatePendingDescriptorSets() failed");

    res = StartRendering(pRenderTarget);
    AMF_RETURN_IF_FAILED(res, L"BitBltRender() - StartRendering() failed");

    // If the dstSurface was acquired from the swapchain, then this will do two things
    // 1. Wait for the semaphore to signal that the frame is ready for use before the pipeline renders onto it
    // 2. Signals the semaphore when queue is executed which the present command can wait for, to ensure rendering
    //    is finished before presenting
    res = m_cmdBuffer.SyncResource((AMFVulkanSync*)&pRenderTarget->m_surface.Sync, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    AMF_RETURN_IF_FAILED(res, L"RenderSurface() - SyncResource() failed to sync render target");

    res = DrawBackground();
    AMF_RETURN_IF_FAILED(res, L"BitBltRender() - DrawBackground() failed");

    res = SetStates(DSG_PRESENT_SURFACE);
    AMF_RETURN_IF_FAILED(res, L"BitBltRender() - SetStates() failed");

    res = DrawFrame(pSrcView->pSurface);
    AMF_RETURN_IF_FAILED(res, L"BitBltRender() - DrawFrame() failed");

    res = DrawOverlay(pSrcSurface);
    AMF_RETURN_IF_FAILED(res, L"BitBltRender() - DrawOverlay() failed");

    res = StopRendering();
    AMF_RETURN_IF_FAILED(res, L"BitBltRender() - StopRendering() failed");

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::StartRendering(const RenderTarget* pRenderTarget)
{
    AMF_RETURN_IF_FALSE(pRenderTarget != nullptr, AMF_INVALID_ARG, L"StartRendering() - pRenderTarget is NULL");
    AMF_RETURN_IF_FALSE(pRenderTarget->m_hFrameBuffer != NULL, AMF_INVALID_ARG, L"StartRendering() - pRenderTarget->m_hFrameBuffer NULL");
    AMF_RETURN_IF_FALSE(m_cmdBuffer.GetBuffer() != nullptr, AMF_NOT_INITIALIZED, L"StartRendering() - Command buffer is not initialized");
    AMF_RETURN_IF_FALSE(m_swapChain.GetRenderPass() != NULL, AMF_NOT_INITIALIZED, L"StartRendering() - Render pass is not initialized");


    const amf_int32 width = pRenderTarget->m_surface.iWidth;
    const amf_int32 height = pRenderTarget->m_surface.iHeight;

    // Start recording
    AMF_RESULT res = m_cmdBuffer.StartRecording();
    AMF_RETURN_IF_FAILED(res, L"StartRendering() - Command Buffer StartRecording() failed");

    // Set viewport
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)width;
    viewport.height = (float)height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    GetVulkan()->vkCmdSetViewport(m_cmdBuffer.GetBuffer(), 0, 1, &viewport);

    VkRect2D scissorRect = {};
    scissorRect.extent.width = width;
    scissorRect.extent.height = height;
    GetVulkan()->vkCmdSetScissor(m_cmdBuffer.GetBuffer(), 0, 1, &scissorRect);

    // Begin render pass
    VkClearValue clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_swapChain.GetRenderPass();
    renderPassInfo.framebuffer = pRenderTarget->m_hFrameBuffer;
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent.width = width;
    renderPassInfo.renderArea.extent.height = height;
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;
    GetVulkan()->vkCmdBeginRenderPass(m_cmdBuffer.GetBuffer(), &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::DrawBackground()
{
    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::SetStates(amf_uint32 descriptorGroupNum)
{
    AMF_RETURN_IF_FALSE(m_cmdBuffer.GetBuffer() != nullptr, AMF_NOT_INITIALIZED, L"SetStates() - Command buffer is not initialized");
    AMF_RETURN_IF_FALSE(m_VertexBuffer.hBuffer != NULL, AMF_NOT_INITIALIZED, L"SetStates() - m_VertexBuffer.hBuffer is not initialized");

    AMF_RESULT res = m_pipeline.SetStates(m_cmdBuffer.GetBuffer(), descriptorGroupNum);
    AMF_RETURN_IF_FAILED(res, L"SetStates() - m_pipeline.SetStates() failed");

    // Vertex/Index buffers
    const VkBuffer vertexBuffers[] = { m_VertexBuffer.hBuffer };
    constexpr VkDeviceSize offsets[] = { 0 };
    GetVulkan()->vkCmdBindVertexBuffers(m_cmdBuffer.GetBuffer(), 0, 1, vertexBuffers, offsets);

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::DrawFrame(AMFVulkanSurface* pSurface)
{
    AMF_RETURN_IF_FALSE(pSurface != nullptr, AMF_INVALID_ARG, L"DrawFrame() - pSurface is NULL");

    AMF_RESULT res = TransitionSurface(&m_cmdBuffer, pSurface, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    AMF_RETURN_IF_FAILED(res, L"DrawFrame() - TransitionSurface() failed");

    // Sync surface
    res = m_cmdBuffer.SyncResource(&pSurface->Sync, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    AMF_RETURN_IF_FAILED(res, L"DrawFrame() - SyncResource() failed");

    // Draw
    GetVulkan()->vkCmdDraw(m_cmdBuffer.GetBuffer(), amf_uint32(m_VertexBuffer.iSize / sizeof(Vertex)), 1, 0, 0);
    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::DrawOverlay(AMFSurface* /* pSurface */)
{
    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::StopRendering()
{
    AMF_RETURN_IF_FALSE(m_cmdBuffer.GetBuffer() != nullptr, AMF_NOT_INITIALIZED, L"StopRendering() - Command Buffer is not initialized");
    AMF_RETURN_IF_FALSE(m_swapChain.GetQueue() != nullptr, AMF_NOT_INITIALIZED, L"StopRendering() - Graphcis Queue is not initialized");

    GetVulkan()->vkCmdEndRenderPass(m_cmdBuffer.GetBuffer());
    AMF_RESULT res = m_cmdBuffer.Execute(m_swapChain.GetQueue());
    AMF_RETURN_IF_FAILED(res, L"StopRendering() - Command Buffer Execute() failed");

    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::OnRenderViewResize(const RenderViewSizeInfo& newRenderView)
{
    AMF_RESULT res = VideoPresenter::OnRenderViewResize(newRenderView);
    AMF_RETURN_IF_FAILED(res, L"OnRenderViewResize() - VideoPresenter::OnRenderViewResize() failed");

    const void* pBuffers[2] = { m_fNormalToViewMatrix, m_fTextureMatrix };
    const amf_size sizes[2] = { sizeof(m_fNormalToViewMatrix), sizeof(m_fTextureMatrix) };
    const amf_size offsets[2] = { offsetof(ViewProjection, vertexTransformMatrix), offsetof(ViewProjection, texTransformMatrix) };

    res = UpdateBuffer(m_viewProjectionBuffer, pBuffers, amf_countof(pBuffers), sizes, offsets);
    AMF_RETURN_IF_FAILED(res, L"OnRenderViewResize() - UpdateBuffer() failed to update view projection buffer");
    
    return AMF_OK;
}

AMF_RESULT AMF_STD_CALL VideoPresenterVulkan::AllocSurface(AMF_MEMORY_TYPE type, AMF_SURFACE_FORMAT format, amf_int32 /*width*/, amf_int32 /*height*/,
                                                           amf_int32 /* hPitch */, amf_int32 /* vPitch */, AMFSurface** ppSurface)
{
    if(m_bRenderToBackBuffer == false)
    {
        return AMF_NOT_IMPLEMENTED;
    }

    AMF_RETURN_IF_FALSE(format == m_eInputFormat, AMF_INVALID_ARG, L"AllocSurface() - Format (%s)"
        "does not match swapchain format (%s)", AMFSurfaceGetFormatName(format),
        AMFSurfaceGetFormatName(GetInputFormat()));

    //const AMFSize swapChainSize = GetSwapchainSize();
    //AMF_RETURN_IF_FALSE(width == 0 || width == swapChainSize.width, AMF_INVALID_ARG, L"AllocSurface() - width(%d) must be either 0 or equal swapchain width(%d)", width, swapChainSize.width);
    //AMF_RETURN_IF_FALSE(height == 0 || height == swapChainSize.height, AMF_INVALID_ARG, L"AllocSurface() - height(%d) must be either 0 or equal swapchain height(%d)", height, swapChainSize.height);

    // wait till buffers are released
    while(m_swapChain.GetBackBuffersAvailable() == 0)
    {
        if(m_bFrozen)
        {
            return AMF_INPUT_FULL;
        }
        amf_sleep(1);
    }

    if (m_bResizeSwapChain)
    {
        // wait till all buffers are released
        while (m_swapChain.GetBackBuffersAcquired() > 0 || m_TrackSurfaces.empty() == false)
        {
            amf_sleep(1);
        }

        AMF_RESULT res = ResizeSwapChain();
        AMF_RETURN_IF_FAILED(res, L"AllocSurface() - ResizeSwapChain() failed");
    }

    AMFLock lock(&m_sect);

    AMF_RESULT res = m_swapChain.AcquireNextBackBuffer(ppSurface);
    AMF_RETURN_IF_FAILED(res, L"AllocSurface() - AcquireNextBackBuffer() failed");
    
    (*ppSurface)->AddObserver(this);
    m_TrackSurfaces.push_back(*ppSurface);

    res = (*ppSurface)->Convert(type);
    AMF_RETURN_IF_FAILED(res, L"AllocSurface() - Convert(%s) failed", AMFGetMemoryTypeName(type));

    return res;
}

void AMF_STD_CALL VideoPresenterVulkan::OnSurfaceDataRelease(AMFSurface* pSurface)
{
    if (pSurface == nullptr)
    {
        return;
    }

    AMFLock lock(&m_sect);
    for(amf_vector<AMFSurface*>::iterator it = m_TrackSurfaces.begin(); it != m_TrackSurfaces.end(); it++)
    {
        if( *it == pSurface)
        {
            pSurface->RemoveObserver(this);
            m_TrackSurfaces.erase(it);

            // Drop backbuffer if acquired
            m_swapChain.DropBackBuffer(pSurface);
            break;
        }
    }
}

AMF_RESULT VideoPresenterVulkan::SetInputFormat(AMF_SURFACE_FORMAT format)
{
    if(format != AMF_SURFACE_BGRA && format != AMF_SURFACE_RGBA)
    {
        return AMF_FAIL;
    }
    m_eInputFormat = format;
    return AMF_OK;
}

VkFormat VideoPresenterVulkan::GetVkFormat()
{
    VkFormat format = VK_FORMAT_UNDEFINED;
    switch (GetInputFormat())
    {
        case AMF_SURFACE_BGRA: format = VK_FORMAT_B8G8R8A8_UNORM; break;
        case AMF_SURFACE_RGBA: format = VK_FORMAT_R8G8B8A8_UNORM; break;
    }

    return format;
}

AMF_RESULT VideoPresenterVulkan::Flush()
{
    return BackBufferPresenter::Flush();
}

AMF_RESULT VideoPresenterVulkan::CheckForResize(amf_bool force)
{
    if (force == true || m_bFullScreen != m_swapChain.FullscreenEnabled())
    {
        m_bResizeSwapChain = true;
        return AMF_OK;
    }

    AMFRect client = GetClientRect();
    amf_int width = client.Width();
    amf_int height = client.Height();

    AMFSize size = GetSwapchainSize();
    if ((width == size.width && height == size.height) || width == 0 || height == 0)
    {
        return AMF_OK;
    }

    m_bResizeSwapChain = true;
    return AMF_OK;
}

AMF_RESULT VideoPresenterVulkan::ResizeSwapChain()
{
    AMFLock lock(&m_sect);

    m_rectClient = GetClientRect();

    AMF_RESULT res = m_swapChain.Resize(m_rectClient.Width(), m_rectClient.Height(), m_bFullScreen);
    AMF_RETURN_IF_FAILED(res, L"ResizeSwapChain() - SwapChainVulkan::ResizeSwapChain() failed");

    UpdateProcessor();
    m_bResizeSwapChain = false;

    return AMF_OK;
}

void AMF_STD_CALL VideoPresenterVulkan::ResizeIfNeeded() // call from UI thread (for VulkanPresenter on Linux)
{
    AMFLock lock(&m_sect);

    CheckForResize(false);

    if (m_bRenderToBackBuffer == false)
    {
        if (m_bResizeSwapChain)
        {
            ResizeSwapChain();
        }
    }
}

AMFSize VideoPresenterVulkan::GetSwapchainSize()
{
    return m_swapChain.GetSize();
}


//-------------------------------------------------------------------------------------------------
//---------------------------------------- Descriptor Heap ----------------------------------------
//-------------------------------------------------------------------------------------------------
DescriptorHeapVulkan::DescriptorHeapVulkan() :
    VulkanContext(),
    m_hDescriptorPool(NULL)
{
}

DescriptorHeapVulkan::~DescriptorHeapVulkan()
{
    Terminate();
}

AMF_RESULT DescriptorHeapVulkan::Terminate()
{
    if (m_pVulkanDevice == nullptr)
    {
        return AMF_OK;
    }

    if (m_hDescriptorSets.empty() == false)
    {
        GetVulkan()->vkFreeDescriptorSets(m_pVulkanDevice->hDevice, m_hDescriptorPool, 1, m_hDescriptorSets.data());
    }
    m_hDescriptorSets.clear();

    if (m_hDescriptorPool != NULL)
    {
        GetVulkan()->vkDestroyDescriptorPool(m_pVulkanDevice->hDevice, m_hDescriptorPool, nullptr);
        m_hDescriptorPool = NULL;
    }

    for (VkDescriptorSetLayout hLayout : m_hDescriptorSetLayouts)
    {
        GetVulkan()->vkDestroyDescriptorSetLayout(m_pVulkanDevice->hDevice, hLayout, nullptr);
    }
    m_hDescriptorSetLayouts.clear();

    m_descriptorPoolSizes.clear();

    m_pendingDescriptorUpdates.clear();
    m_descriptorBufferInfoHeap.clear();
    m_descriptorImageInfoHeap.clear();

    return VulkanContext::Terminate();
}

AMF_RESULT DescriptorHeapVulkan::RegisterDescriptorSet(DescriptorVulkan** ppDescriptors, amf_uint32 count)
{
    AMF_RETURN_IF_FALSE(m_hDescriptorPool == NULL, AMF_FAIL, L"RegisterDescriptorSet() - descriptor pool is already initialized, call terminate first");
    AMF_RETURN_IF_FALSE(ppDescriptors != nullptr, AMF_INVALID_ARG, L"RegisterDescriptorSet() - ppDescriptors is NULL");
    AMF_RETURN_IF_FALSE(count > 0, AMF_INVALID_ARG, L"RegisterDescriptorSet() - descriptor count cannot be 0");

    for (amf_uint32 i = 0; i < count; ++i)
    {
        AMF_RETURN_IF_INVALID_POINTER(ppDescriptors[i], L"RegisterDescriptorSet() - ppDescriptors[%u] is NULL", i);
    }

    const amf_uint32 setIndex = (amf_uint32)m_hDescriptorSetLayouts.size();

    amf_vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(count);

    for (amf_uint i = 0; i < count; ++i)
    {
        const VkDescriptorSetLayoutBinding& binding = ppDescriptors[i]->layoutBinding;

        ppDescriptors[i]->setIndex = setIndex;

        bindings.push_back(binding);

        VkDescriptorPoolSize poolSize = {};
        poolSize.type = binding.descriptorType;
        poolSize.descriptorCount = binding.descriptorCount;

        m_descriptorPoolSizes.push_back(poolSize);
    }

    // Create descriptor set layout
    VkDescriptorSetLayoutCreateInfo layoutCreateInfo = {};
    layoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCreateInfo.bindingCount = (amf_uint32)bindings.size();
    layoutCreateInfo.pBindings = bindings.data();

    m_hDescriptorSetLayouts.push_back(NULL);
    VkResult vkres = GetVulkan()->vkCreateDescriptorSetLayout(m_pVulkanDevice->hDevice, &layoutCreateInfo, nullptr, &m_hDescriptorSetLayouts.back());
    ASSERT_RETURN_IF_VK_FAILED(vkres, AMF_VULKAN_FAILED, L"ReigsterDescriptorSet() - vkCreateDescriptorSetLayout()");

    return AMF_OK;
}

AMF_RESULT DescriptorHeapVulkan::CreateDescriptors()
{
    AMF_RETURN_IF_FALSE(m_pVulkanDevice != nullptr, AMF_NOT_INITIALIZED, L"CreateDescriptors() - m_pVulkanDevice is not initialized");
    AMF_RETURN_IF_FALSE(m_pVulkanDevice->hDevice != nullptr, AMF_NOT_INITIALIZED, L"CreateDescriptors() - m_pVulkanDevice->hDevice is not initialized");
    AMF_RETURN_IF_FALSE(m_hDescriptorPool == NULL, AMF_ALREADY_INITIALIZED, L"CreateDescriptors() - m_hDescriptorPool is already initialized");
    AMF_RETURN_IF_FALSE(m_hDescriptorSets.size() == 0, AMF_ALREADY_INITIALIZED, L"CreateDescriptors() - m_hDescriptorSets should be empty");

    // Nothing to create
    if (m_hDescriptorSetLayouts.size() == 0)
    {
        AMFTraceWarning(AMF_FACILITY, L"CreateDescriptorSetPool() - No descriptor sets were registered");
        AMF_RETURN_IF_FALSE(m_descriptorPoolSizes.size() == 0, AMF_UNEXPECTED, L"CreateDescriptors() - Descriptor pools registed without any descriptor sets");
        return AMF_OK;
    }

    // Create descriptor pool
    VkDescriptorPoolCreateInfo poolCreateInfo = {};
    poolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCreateInfo.poolSizeCount = (amf_uint32)m_descriptorPoolSizes.size();
    poolCreateInfo.pPoolSizes = m_descriptorPoolSizes.data();
    poolCreateInfo.maxSets = (amf_uint32)m_hDescriptorSetLayouts.size();
    poolCreateInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    VkResult vkres = GetVulkan()->vkCreateDescriptorPool(m_pVulkanDevice->hDevice, &poolCreateInfo, nullptr, &m_hDescriptorPool);
    ASSERT_RETURN_IF_VK_FAILED(vkres, AMF_VULKAN_FAILED, L"CreateDescriptors() - vkCreateDescriptorPool() failed");

    // Create descriptor sets
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_hDescriptorPool;
    allocInfo.descriptorSetCount = (amf_uint32)m_hDescriptorSetLayouts.size();
    allocInfo.pSetLayouts = m_hDescriptorSetLayouts.data();

    m_hDescriptorSets.resize(m_hDescriptorSetLayouts.size(), NULL);
    vkres = GetVulkan()->vkAllocateDescriptorSets(m_pVulkanDevice->hDevice, &allocInfo, m_hDescriptorSets.data());
    ASSERT_RETURN_IF_VK_FAILED(vkres, AMF_VULKAN_FAILED, L"CreateDescriptors() - vkAllocateDescriptorSets() failed");

    return AMF_OK;
}

amf_uint32 DescriptorHeapVulkan::GetDescriptorCount() const
{
    return (amf_uint32)m_hDescriptorSets.size();
}

VkDescriptorSet DescriptorHeapVulkan::GetDescriptorSet(amf_uint32 setIndex) const
{
    if (setIndex >= m_hDescriptorSets.size())
    {
        AMFTraceError(AMF_FACILITY, L"GetDescriptorSet() - index (%u) out of range, must be in range [0, %zu]", setIndex, m_hDescriptorSets.size() - 1);
        return NULL;
    }

    return m_hDescriptorSets[setIndex];
}

VkDescriptorSetLayout DescriptorHeapVulkan::GetDescriptorSetLayout(amf_uint32 setIndex) const
{
    if (setIndex >= m_hDescriptorSetLayouts.size())
    {
        AMFTraceError(AMF_FACILITY, L"GetDescriptorSetLayout() - index (%u) out of range, must be in range [0, %zu]", setIndex, m_hDescriptorSetLayouts.size() - 1);
        return NULL;
    }

    return m_hDescriptorSetLayouts[setIndex];
}

const amf_vector<VkDescriptorSet>& DescriptorHeapVulkan::GetDescriptorSets() const
{
    return m_hDescriptorSets;
}

const amf_vector<VkDescriptorSetLayout>& DescriptorHeapVulkan::GetDescriptorSetLayouts() const
{
    return m_hDescriptorSetLayouts;
}

AMF_RESULT DescriptorHeapVulkan::UpdateDescriptorSet(const DescriptorVulkan* pDescriptor, amf_uint32 arrayIndex, amf_uint32 count, VkWriteDescriptorSet& writeInfo, bool immediate)
{
    AMF_RETURN_IF_FALSE(pDescriptor != nullptr, AMF_INVALID_ARG, L"UpdateDescriptorSetBuffer() - pDescriptor is NULL");
    AMF_RETURN_IF_FALSE(m_pVulkanDevice != nullptr, AMF_NOT_INITIALIZED, L"UpdateDescriptorSetBuffer() - m_pVulkanDevice is not initialized");
    AMF_RETURN_IF_FALSE(m_pVulkanDevice->hDevice != nullptr, AMF_NOT_INITIALIZED, L"UpdateDescriptorSetBuffer() - m_pVulkanDevice->hDevice is not initialized");

    const VkDescriptorSet descriptorSet = GetDescriptorSet(pDescriptor->setIndex);
    AMF_RETURN_IF_FALSE(descriptorSet != NULL, AMF_NOT_INITIALIZED, L"UpdateDescriptorSetBuffer() - m_hDescriptorSet is not initialized");

    writeInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeInfo.dstSet = descriptorSet;
    writeInfo.dstBinding = pDescriptor->layoutBinding.binding;
    writeInfo.dstArrayElement = arrayIndex;
    writeInfo.descriptorType = pDescriptor->layoutBinding.descriptorType;
    writeInfo.descriptorCount = count;

    if (immediate)
    {
        GetVulkan()->vkUpdateDescriptorSets(m_pVulkanDevice->hDevice, 1, &writeInfo, 0, nullptr);
    }
    else
    {
        m_pendingDescriptorUpdates.push_back(writeInfo);
    }

    return AMF_OK;
}

AMF_RESULT DescriptorHeapVulkan::UpdateDescriptorSet(const DescriptorVulkan* pDescriptor, amf_uint32 arrayIndex, amf_uint32 count, const VkDescriptorBufferInfo* pBufferInfos, bool immediate)
{
    AMF_RETURN_IF_FALSE(pDescriptor != nullptr, AMF_INVALID_ARG, L"UpdateDescriptorSetBuffer() - pDescriptor is NULL");
    AMF_RETURN_IF_FALSE(count > 0, AMF_INVALID_ARG, L"UpdateDescriptorSetBuffer() - count must be greater than 0");

    VkWriteDescriptorSet writeInfo = {};
    ProcessUpdateInfo<VkDescriptorBufferInfo>(count, pBufferInfos, immediate, m_descriptorBufferInfoHeap, &writeInfo.pBufferInfo);

    return UpdateDescriptorSet(pDescriptor, arrayIndex, count, writeInfo, immediate);
}

AMF_RESULT DescriptorHeapVulkan::UpdateDescriptorSet(const DescriptorVulkan* pDescriptor, amf_uint32 arrayIndex, amf_uint32 count, const VkDescriptorImageInfo* pImageInfos, bool immediate)
{
    AMF_RETURN_IF_FALSE(pDescriptor != nullptr, AMF_INVALID_ARG, L"UpdateDescriptorSetBuffer() - pDescriptor is NULL");
    AMF_RETURN_IF_FALSE(count > 0, AMF_INVALID_ARG, L"UpdateDescriptorSetBuffer() - count must be greater than 0");

    VkWriteDescriptorSet writeInfo = {};
    ProcessUpdateInfo<VkDescriptorImageInfo>(count, pImageInfos, immediate, m_descriptorImageInfoHeap, &writeInfo.pImageInfo);
    return UpdateDescriptorSet(pDescriptor, arrayIndex, count, writeInfo, immediate);
}

AMF_RESULT DescriptorHeapVulkan::UpdatePendingDescriptorSets()
{
    AMF_RETURN_IF_FALSE(m_pVulkanDevice != nullptr, AMF_NOT_INITIALIZED, L"UpdateDescriptorSetBuffer() - m_pVulkanDevice is not initialized");
    AMF_RETURN_IF_FALSE(m_pVulkanDevice->hDevice != nullptr, AMF_NOT_INITIALIZED, L"UpdateDescriptorSetBuffer() - m_pVulkanDevice->hDevice is not initialized");

    if (m_pendingDescriptorUpdates.empty())
    {
        return AMF_OK;
    }

    GetVulkan()->vkUpdateDescriptorSets(m_pVulkanDevice->hDevice, (amf_uint32)m_pendingDescriptorUpdates.size(), m_pendingDescriptorUpdates.data(), 0, nullptr);
    m_pendingDescriptorUpdates.clear();
    m_descriptorBufferInfoHeap.clear();
    m_descriptorImageInfoHeap.clear();

    return AMF_OK;
}

//-------------------------------------------------------------------------------------------------
//--------------------------------------- RenderingPipelineVulkan ---------------------------------------
//-------------------------------------------------------------------------------------------------
RenderingPipelineVulkan::RenderingPipelineVulkan() :
    VulkanContext(),
    m_pDescriptorHeap(nullptr),
    m_hRenderPass(NULL),
    m_hPipeline(NULL),
    m_hPipelineLayout(NULL)
{
}

RenderingPipelineVulkan::~RenderingPipelineVulkan()
{
    Terminate();
}

AMF_RESULT RenderingPipelineVulkan::Init(AMFVulkanDevice* pDevice, const VulkanImportTable* pImportTable, DescriptorHeapVulkan* pDescriptorHeap)
{
    AMF_RETURN_IF_FALSE(pDescriptorHeap != nullptr, AMF_INVALID_ARG, L"Init() - pDescriptorHeap is NULL");

    AMF_RESULT res = VulkanContext::Init(pDevice, pImportTable);
    AMF_RETURN_IF_FAILED(res, L"Init() - VulkanObject::Init() failed");

    m_pDescriptorHeap = pDescriptorHeap;

    return AMF_OK;
}

AMF_RESULT RenderingPipelineVulkan::Terminate()
{
    if (m_pVulkanDevice == nullptr)
    {
        return AMF_OK;
    }

    if (m_hPipeline != NULL)
    {
        GetVulkan()->vkDestroyPipeline(m_pVulkanDevice->hDevice, m_hPipeline, nullptr);
        m_hPipeline = NULL;
    }

    if (m_hPipelineLayout != NULL)
    {
        GetVulkan()->vkDestroyPipelineLayout(m_pVulkanDevice->hDevice, m_hPipelineLayout, nullptr);
        m_hPipelineLayout = NULL;
    }

    m_hRenderPass = NULL;

    m_descriptorSetGroupMap.clear();
    m_hDescriptorSetLayouts.clear();

    return VulkanContext::Terminate();
}

AMF_RESULT RenderingPipelineVulkan::RegisterDescriptorSet(amf_uint32 setIndex, amf_uint32 setNum, const amf_uint32* pGroups, amf_uint32 groupCount)
{
    AMF_RETURN_IF_FALSE(m_hPipeline == NULL, AMF_FAIL, L"RegisterDescriptorSet() - pipeline is already initialized, call terminate first");
    AMF_RETURN_IF_FALSE(pGroups != nullptr, AMF_INVALID_ARG, L"RegisterDescriptorSet() - pGroups is NULL");
    AMF_RETURN_IF_FALSE(groupCount > 0, AMF_INVALID_ARG, L"RegisterDescriptorSet() - group count cannot be 0");

    const VkDescriptorSetLayout hLayout = m_pDescriptorHeap->GetDescriptorSetLayout(setIndex);
    AMF_RETURN_IF_FALSE(hLayout != NULL, AMF_UNEXPECTED, L"RegisterDescriptorSet() - GetDescriptorSetLayout() returned NULL");
    m_hDescriptorSetLayouts.push_back(hLayout);

    for (amf_uint i = 0; i < groupCount; ++i)
    {
        const amf_uint32 groupNum = pGroups[i];
        DescriptorSetGroup& setGroup = m_descriptorSetGroupMap[groupNum];

        if (setNum >= setGroup.descriptorSetIndices.size())
        {
            setGroup.descriptorSetIndices.resize(setNum + 1, UINT32_MAX);
        }

        AMF_RETURN_IF_FALSE(setGroup.descriptorSetIndices[setNum] == UINT32_MAX, AMF_INVALID_ARG, L"RegisterDescriptorSet() - descriptor already binded to group %u set number %u", groupNum, setNum);

        setGroup.descriptorSetIndices[setNum] = setIndex;
    }

    return AMF_OK;
}

AMF_RESULT RenderingPipelineVulkan::SetDefaultInfo(PipelineCreateInfo& createInfo)
{
    createInfo = {};

    // No viewports or scissors, set dynamically on resize

    // Input Assembly
    createInfo.inputAssemblyInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    createInfo.inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    createInfo.inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

    // Rasterization state
    createInfo.rasterizationStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    createInfo.rasterizationStateInfo.depthClampEnable = VK_FALSE; // VK_TRUE
    createInfo.rasterizationStateInfo.rasterizerDiscardEnable = VK_FALSE;
    createInfo.rasterizationStateInfo.polygonMode = VK_POLYGON_MODE_FILL;
    createInfo.rasterizationStateInfo.cullMode = VK_CULL_MODE_NONE; //VK_CULL_MODE_BACK_BIT;
    createInfo.rasterizationStateInfo.frontFace = VK_FRONT_FACE_CLOCKWISE;//VK_FRONT_FACE_COUNTER_CLOCKWISE;
    createInfo.rasterizationStateInfo.depthBiasEnable = VK_FALSE;
    createInfo.rasterizationStateInfo.lineWidth = 1.0f;

    // Multisampling
    createInfo.multiSamplingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    createInfo.multiSamplingInfo.sampleShadingEnable = VK_FALSE;
    createInfo.multiSamplingInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Color blend attributes, sets up color / alpha blending.
    createInfo.colorBlendAttachments.resize(1);
    createInfo.colorBlendAttachments[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_A_BIT;
    createInfo.colorBlendAttachments[0].blendEnable = VK_FALSE;

    createInfo.colorBlendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    createInfo.colorBlendInfo.logicOpEnable = VK_FALSE;
    createInfo.colorBlendInfo.logicOp = VK_LOGIC_OP_COPY;
    // Attachments get set when creating pipeline

    // Depth stencil state
    createInfo.depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    createInfo.depthStencilState.depthTestEnable = VK_FALSE;
    createInfo.depthStencilState.depthWriteEnable = VK_FALSE;
    createInfo.depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    createInfo.depthStencilState.depthBoundsTestEnable = VK_FALSE;
    createInfo.depthStencilState.back.failOp = VK_STENCIL_OP_KEEP;
    createInfo.depthStencilState.back.passOp = VK_STENCIL_OP_KEEP;
    createInfo.depthStencilState.back.compareOp = VK_COMPARE_OP_ALWAYS;
    createInfo.depthStencilState.stencilTestEnable = VK_FALSE;
    createInfo.depthStencilState.front = createInfo.depthStencilState.back;

    createInfo.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

    return AMF_OK;
}

AMF_RESULT RenderingPipelineVulkan::CreatePipeline(PipelineCreateInfo& createInfo, VkRenderPass hRenderPass, amf_uint32 subpass)
{
    AMF_RETURN_IF_FALSE(m_pVulkanDevice != nullptr, AMF_NOT_INITIALIZED, L"CreatePipeline() - m_pVulkanDevice is not initialized");
    AMF_RETURN_IF_FALSE(m_pVulkanDevice->hDevice != nullptr, AMF_NOT_INITIALIZED, L"CreatePipeline() - m_pVulkanDevice->hDevice is not initialized");
    AMF_RETURN_IF_FALSE(hRenderPass != NULL, AMF_NOT_INITIALIZED, L"CreatePipeline() - hRenderPass is not initialized");
    AMF_RETURN_IF_FALSE(m_hRenderPass == NULL, AMF_ALREADY_INITIALIZED, L"CreatePipeline() - m_hRenderPass is already initialized");
    AMF_RETURN_IF_FALSE(m_hPipelineLayout == NULL, AMF_ALREADY_INITIALIZED, L"CreatePipeline() - m_hPipelineLayout is already initialized");
    AMF_RETURN_IF_FALSE(m_hPipeline == NULL, AMF_ALREADY_INITIALIZED, L"CreatePipeline() - m_hPipeline is already initialized");

    m_hRenderPass = hRenderPass;

    createInfo.inputAssemblyInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    createInfo.rasterizationStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    createInfo.multiSamplingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    createInfo.depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineVertexInputStateCreateInfo vertInputInfo = {};
        vertInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertInputInfo.vertexBindingDescriptionCount = (amf_uint32)createInfo.vertexBindingDescs.size();
        vertInputInfo.pVertexBindingDescriptions = createInfo.vertexBindingDescs.empty() ? nullptr : createInfo.vertexBindingDescs.data();
        vertInputInfo.vertexAttributeDescriptionCount = (amf_uint32)createInfo.vertexAttributeDescs.size();
        vertInputInfo.pVertexAttributeDescriptions = createInfo.vertexAttributeDescs.empty() ? nullptr : createInfo.vertexAttributeDescs.data();

    // Viewport state
    VkPipelineViewportStateCreateInfo viewportState = {};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = (amf_uint32)createInfo.viewports.size();
        viewportState.pViewports = createInfo.viewports.empty() ? nullptr : createInfo.viewports.data();
        viewportState.scissorCount = (amf_uint32)createInfo.scissors.size();
        viewportState.pScissors = createInfo.scissors.empty() ? nullptr : createInfo.scissors.data();

    // Color blend info
    createInfo.colorBlendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    createInfo.colorBlendInfo.attachmentCount = (amf_uint32)createInfo.colorBlendAttachments.size();;
    createInfo.colorBlendInfo.pAttachments = createInfo.colorBlendAttachments.empty() ? nullptr : createInfo.colorBlendAttachments.data();

    VkPipelineDynamicStateCreateInfo dynamicInfo = {};
        dynamicInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicInfo.dynamicStateCount = (amf_uint32)createInfo.dynamicStates.size();
        dynamicInfo.pDynamicStates = createInfo.dynamicStates.empty() ? nullptr : createInfo.dynamicStates.data();

    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = (amf_uint32)m_hDescriptorSetLayouts.size();
        pipelineLayoutInfo.pSetLayouts = m_hDescriptorSetLayouts.empty() ? nullptr : m_hDescriptorSetLayouts.data();

    VkResult vkres = GetVulkan()->vkCreatePipelineLayout(m_pVulkanDevice->hDevice, &pipelineLayoutInfo, nullptr, &m_hPipelineLayout);
    ASSERT_RETURN_IF_VK_FAILED(vkres, AMF_VULKAN_FAILED, L"CreatePipeline() - vkCreatePipelineLayout() failed");

    // Create pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = (amf_uint32)createInfo.shaderStages.size();
        pipelineInfo.pStages = createInfo.shaderStages.empty() ? nullptr : createInfo.shaderStages.data();
        pipelineInfo.pVertexInputState = &vertInputInfo;
        pipelineInfo.pInputAssemblyState = &createInfo.inputAssemblyInfo;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &createInfo.rasterizationStateInfo;
        pipelineInfo.pDepthStencilState = &createInfo.depthStencilState;
        pipelineInfo.pColorBlendState = &createInfo.colorBlendInfo;
        pipelineInfo.pMultisampleState = &createInfo.multiSamplingInfo;
        pipelineInfo.layout = m_hPipelineLayout;
        pipelineInfo.renderPass = m_hRenderPass;
        pipelineInfo.subpass = subpass;
        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
        pipelineInfo.pDynamicState = &dynamicInfo;

    vkres = GetVulkan()->vkCreateGraphicsPipelines(m_pVulkanDevice->hDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_hPipeline);
    ASSERT_RETURN_IF_VK_FAILED(vkres, AMF_VULKAN_FAILED, L"CreatePipeline() - vkCreateGraphicsPipelines() failed");

    // Setup descriptor groups
    const amf_uint32 descriptorCount = m_pDescriptorHeap->GetDescriptorCount();

    for (std::pair<const amf_uint32, DescriptorSetGroup>& it : m_descriptorSetGroupMap)
    {
        const amf_uint32 groupNum = it.first;
        DescriptorSetGroup& group = it.second;
        group.descriptorSetHandles.clear();
        group.descriptorSetHandles.reserve(group.descriptorSetIndices.size());

        for (const amf_uint32 setIndex : group.descriptorSetIndices)
        {
            const amf_uint32 setNum = (amf_uint32)group.descriptorSetHandles.size();
            AMF_RETURN_IF_FALSE(setIndex < descriptorCount, AMF_OUT_OF_RANGE, L"CreatePipeline() - group descriptor set index (%u) out of bounds, must be in [0, %zu] at group %u, set %zu", 
                                setIndex, descriptorCount, groupNum, setNum);

            const VkDescriptorSet hDescriptorSet = m_pDescriptorHeap->GetDescriptorSet(setIndex);
            AMF_RETURN_IF_FALSE(hDescriptorSet != NULL, AMF_UNEXPECTED, L"CreatePipeline() - GetDescriptorSet() returned NULL");

            group.descriptorSetHandles.push_back(hDescriptorSet);
        }
    }

    return AMF_OK;
}

AMF_RESULT RenderingPipelineVulkan::SetStates(VkCommandBuffer hCommandBuffer, amf_uint32 groupNum)
{
    AMF_RETURN_IF_FALSE(hCommandBuffer != nullptr, AMF_INVALID_ARG, L"SetStates() - hCommandBuffer is NULL");
    AMF_RETURN_IF_FALSE(m_descriptorSetGroupMap.find(groupNum) != m_descriptorSetGroupMap.end(), AMF_INVALID_ARG, L"SetStates() - Invalid group number (%u) not registered", groupNum);
    AMF_RETURN_IF_FALSE(m_hPipeline != NULL, AMF_NOT_INITIALIZED, L"SetStates() - m_hPipeline is not initialized");
    AMF_RETURN_IF_FALSE(m_hPipelineLayout != NULL, AMF_NOT_INITIALIZED, L"SetStates() - m_hPipelineLayout is not initialized");

    GetVulkan()->vkCmdBindPipeline(hCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_hPipeline);

    const amf_vector<VkDescriptorSet>& descriptorSets = m_descriptorSetGroupMap[groupNum].descriptorSetHandles;
    GetVulkan()->vkCmdBindDescriptorSets(hCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_hPipelineLayout, 0, (amf_uint32)descriptorSets.size(), descriptorSets.data(), 0, nullptr);
    return AMF_OK;
}
