/*
 * Copyright (c) 2022, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2014-2023 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */
//////////////////////////////////////////////////////////////////////////
/*

 This sample creates a 3D cube and render using the builtin camera

*/
//////////////////////////////////////////////////////////////////////////

// clang-format off
#define IM_VEC2_CLASS_EXTRA ImVec2(const nvmath::vec2f& f) {x = f.x; y = f.y;} operator nvmath::vec2f() const { return nvmath::vec2f(x, y); }

// clang-format on
#include <array>
#include <vulkan/vulkan_core.h>

#define VMA_IMPLEMENTATION
#include "imgui/imgui_camera_widget.h"
#include "nvh/primitives.hpp"
#include "nvvk/commands_vk.hpp"
#include "nvvk/debug_util_vk.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/dynamicrendering_vk.hpp"
#include "nvvk/pipeline_vk.hpp"
#include "nvvkhl/alloc_vma.hpp"
#include "nvvkhl/application.hpp"
#include "nvvkhl/element_camera.hpp"
#include "nvvkhl/element_gui.hpp"
#include "nvvkhl/element_testing.hpp"
#include "nvvkhl/gbuffer.hpp"
#include "nvvkhl/pipeline_container.hpp"


#include "nvvk/images_vk.hpp"
#include "shaders/device_host.h"

#if USE_HLSL
#include "_autogen/raster_vertexMain.spirv.h"
#include "_autogen/raster_fragmentMain.spirv.h"
const auto& vert_shd = std::vector<uint8_t>{std::begin(raster_vertexMain), std::end(raster_vertexMain)};
const auto& frag_shd = std::vector<uint8_t>{std::begin(raster_fragmentMain), std::end(raster_fragmentMain)};
#elif USE_SLANG
#include "_autogen/raster_vertexMain.spirv.h"
#include "_autogen/raster_fragmentMain.spirv.h"
const auto& vert_shd = std::vector<uint32_t>{std::begin(raster_vertexMain), std::end(raster_vertexMain)};
const auto& frag_shd = std::vector<uint32_t>{std::begin(raster_fragmentMain), std::end(raster_fragmentMain)};
#else
#include "_autogen/raster.frag.h"
#include "_autogen/raster.vert.h"
const auto& vert_shd = std::vector<uint32_t>{std::begin(raster_vert), std::end(raster_vert)};
const auto& frag_shd = std::vector<uint32_t>{std::begin(raster_frag), std::end(raster_frag)};
#endif  // USE_HLSL


//////////////////////////////////////////////////////////////////////////
/// </summary> Display an image on a quad.
class SimplePolygons : public nvvkhl::IAppElement
{
public:
  SimplePolygons()           = default;
  ~SimplePolygons() override = default;

  void onAttach(nvvkhl::Application* app) override
  {
    m_app    = app;
    m_device = m_app->getDevice();

    m_dutil = std::make_unique<nvvk::DebugUtil>(m_device);                    // Debug utility
    m_alloc = std::make_unique<nvvkhl::AllocVma>(m_app->getContext().get());  // Allocator
    m_dset  = std::make_unique<nvvk::DescriptorSetContainer>(m_device);

    createScene();
    createVkBuffers();
    createPipeline();
  }

  void onDetach() override
  {
    vkDeviceWaitIdle(m_device);
    destroyResources();
  }

  void onResize(uint32_t width, uint32_t height) override { createGbuffers({width, height}); }

  void onUIRender() override
  {
    if(!m_gBuffers)
      return;

    {  // Setting menu
      ImGui::Begin("Settings");
      ImGuiH::CameraWidget();
      ImGui::End();
    }

    {  // Rendering Viewport
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
      ImGui::Begin("Viewport");

      // Deal with mouse interaction only if the window has focus
      if(ImGui::IsWindowHovered(ImGuiFocusedFlags_RootWindow) && ImGui::IsMouseDoubleClicked(0))
      {
        rasterPicking();
      }

      // Display the G-Buffer image
      ImGui::Image(m_gBuffers->getDescriptorSet(), ImGui::GetContentRegionAvail());

      ImGui::End();
      ImGui::PopStyleVar();
    }
  }

  void onRender(VkCommandBuffer cmd) override
  {
    if(!m_gBuffers)
      return;

    const nvvk::DebugUtil::ScopedCmdLabel sdbg = m_dutil->DBG_SCOPE(cmd);

    const float   aspect_ratio = m_viewSize.x / m_viewSize.y;
    nvmath::vec3f eye;
    nvmath::vec3f center;
    nvmath::vec3f up;
    CameraManip.getLookat(eye, center, up);

    // Update Frame buffer uniform buffer
    FrameInfo            finfo{};
    const nvmath::vec2f& clip = CameraManip.getClipPlanes();
    finfo.view                = CameraManip.getMatrix();
    finfo.proj                = nvmath::perspectiveVK(CameraManip.getFov(), aspect_ratio, clip.x, clip.y);
    finfo.camPos              = eye;
    vkCmdUpdateBuffer(cmd, m_frameInfo.buffer, 0, sizeof(FrameInfo), &finfo);

    // Drawing the primitives in a G-Buffer
    nvvk::createRenderingInfo r_info({{0, 0}, m_gBuffers->getSize()}, {m_gBuffers->getColorImageView()},
                                     m_gBuffers->getDepthImageView(), VK_ATTACHMENT_LOAD_OP_CLEAR,
                                     VK_ATTACHMENT_LOAD_OP_CLEAR, m_clearColor);
    r_info.pStencilAttachment = nullptr;

    vkCmdBeginRendering(cmd, &r_info);
    m_app->setViewport(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_dset->getPipeLayout(), 0, 1, m_dset->getSets(), 0, nullptr);
    const VkDeviceSize offsets{0};
    for(const nvh::Node& n : m_nodes)
    {
      PrimitiveMeshVk& m = m_meshVk[n.mesh];
      // Push constant information
      m_pushConst.transfo = n.localMatrix();
      m_pushConst.color   = m_materials[n.material].color;
      vkCmdPushConstants(cmd, m_dset->getPipeLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(PushConstant), &m_pushConst);

      vkCmdBindVertexBuffers(cmd, 0, 1, &m.vertices.buffer, &offsets);
      vkCmdBindIndexBuffer(cmd, m.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
      auto num_indices = static_cast<uint32_t>(m_meshes[n.mesh].triangles.size() * 3);
      vkCmdDrawIndexed(cmd, num_indices, 1, 0, 0, 0);
    }
    vkCmdEndRendering(cmd);
  }

private:
  void createScene()
  {
    // Meshes
    m_meshes.emplace_back(nvh::createSphereMesh(0.5F, 3));
    m_meshes.emplace_back(nvh::createSphereUv(0.5F, 30, 30));
    m_meshes.emplace_back(nvh::createCube(1.0F, 1.0F, 1.0F));
    m_meshes.emplace_back(nvh::createTetrahedron());
    m_meshes.emplace_back(nvh::createOctahedron());
    m_meshes.emplace_back(nvh::createIcosahedron());
    m_meshes.emplace_back(nvh::createConeMesh(0.5F, 1.0F, 32));
    m_meshes.emplace_back(nvh::createTorusMesh(0.5F, 0.25F, 32, 16));

    const int num_meshes = static_cast<int>(m_meshes.size());

    // Materials (colorful)
    for(int i = 0; i < num_meshes; i++)
    {
      const nvmath::vec3f freq = nvmath::vec3f(1.33333F, 2.33333F, 3.33333F) * static_cast<float>(i);
      const nvmath::vec3f v    = static_cast<nvmath::vec3f>(sin(freq) * 0.5F + 0.5F);
      m_materials.push_back({nvmath::vec4f(v, 1)});
    }

    // Instances
    for(int i = 0; i < num_meshes; i++)
    {
      nvh::Node& n  = m_nodes.emplace_back();
      n.mesh        = i;
      n.material    = i;
      n.translation = nvmath::vec3f(-(static_cast<float>(num_meshes) / 2.F) + static_cast<float>(i), 0.F, 0.F);
    }

    CameraManip.setClipPlanes({0.1F, 100.0F});
    CameraManip.setLookat({-0.5F, 0.0F, 5.0F}, {-0.5F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
  }

  void createPipeline()
  {
    m_dset->addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL | VK_SHADER_STAGE_FRAGMENT_BIT);
    m_dset->initLayout();
    m_dset->initPool(1);

    const VkPushConstantRange push_constant_ranges = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                                      sizeof(PushConstant)};
    m_dset->initPipeLayout(1, &push_constant_ranges);

    // Writing to descriptors
    const VkDescriptorBufferInfo      dbi_unif{m_frameInfo.buffer, 0, VK_WHOLE_SIZE};
    std::vector<VkWriteDescriptorSet> writes;
    writes.emplace_back(m_dset->makeWrite(0, 0, &dbi_unif));
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    VkPipelineRenderingCreateInfo prend_info{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    prend_info.colorAttachmentCount    = 1;
    prend_info.pColorAttachmentFormats = &m_colorFormat;
    prend_info.depthAttachmentFormat   = m_depthFormat;

    // Creating the Pipeline
    nvvk::GraphicsPipelineState pstate;
    pstate.rasterizationState.cullMode = VK_CULL_MODE_NONE;
    pstate.addBindingDescriptions({{0, sizeof(nvh::PrimitiveVertex)}});
    pstate.addAttributeDescriptions({
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(nvh::PrimitiveVertex, p))},  // Position
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(nvh::PrimitiveVertex, n))},  // Normal
    });

    nvvk::GraphicsPipelineGenerator pgen(m_device, m_dset->getPipeLayout(), prend_info, pstate);
    pgen.addShader(vert_shd, VK_SHADER_STAGE_VERTEX_BIT, USE_HLSL ? "vertexMain" : "main");
    pgen.addShader(frag_shd, VK_SHADER_STAGE_FRAGMENT_BIT, USE_HLSL ? "fragmentMain" : "main");

    m_graphicsPipeline = pgen.createPipeline();
    m_dutil->setObjectName(m_graphicsPipeline, "Graphics");
    pgen.clearShaders();
  }

  void createGbuffers(const nvmath::vec2f& size)
  {
    m_viewSize = size;
    m_gBuffers = std::make_unique<nvvkhl::GBuffer>(m_device, m_alloc.get(),
                                                   VkExtent2D{static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y)},
                                                   m_colorFormat, m_depthFormat);
  }

  void createVkBuffers()
  {
    VkCommandBuffer cmd = m_app->createTempCmdBuffer();
    m_meshVk.resize(m_meshes.size());
    for(size_t i = 0; i < m_meshes.size(); i++)
    {
      PrimitiveMeshVk& m = m_meshVk[i];
      m.vertices         = m_alloc->createBuffer(cmd, m_meshes[i].vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
      m.indices          = m_alloc->createBuffer(cmd, m_meshes[i].triangles, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
      m_dutil->DBG_NAME_IDX(m.vertices.buffer, i);
      m_dutil->DBG_NAME_IDX(m.indices.buffer, i);
    }

    m_frameInfo = m_alloc->createBuffer(sizeof(FrameInfo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    m_dutil->DBG_NAME(m_frameInfo.buffer);

    m_pixelBuffer = m_alloc->createBuffer(sizeof(float) * 4, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    m_app->submitAndWaitTempCmdBuffer(cmd);
  }


  void destroyResources()
  {
    vkDestroyPipeline(m_device, m_graphicsPipeline, nullptr);

    for(PrimitiveMeshVk& m : m_meshVk)
    {
      m_alloc->destroy(m.vertices);
      m_alloc->destroy(m.indices);
    }
    m_alloc->destroy(m_frameInfo);
    m_alloc->destroy(m_pixelBuffer);

    m_dset->deinit();
    m_gBuffers.reset();
  }

  //--------------------------------------------------------------------------------------------------
  // Find the 3D position under the mouse cursor and set the camera interest to this position
  //
  void rasterPicking()
  {
    nvmath::vec2f       mouse_pos = ImGui::GetMousePos();         // Current mouse pos in window
    const nvmath::vec2f corner    = ImGui::GetCursorScreenPos();  // Corner of the viewport
    mouse_pos                     = mouse_pos - corner;           // Mouse pos relative to center of viewport

    const float          aspect_ratio = m_viewSize.x / m_viewSize.y;
    const nvmath::vec2f& clip         = CameraManip.getClipPlanes();
    const nvmath::mat4f  view         = CameraManip.getMatrix();
    const nvmath::mat4f  proj         = nvmath::perspectiveVK(CameraManip.getFov(), aspect_ratio, clip.x, clip.y);

    // Find the distance under the cursor
    const float d = getDepth(static_cast<int>(mouse_pos.x), static_cast<int>(mouse_pos.y));

    if(d < 1.0F)  // Ignore infinite
    {
      const nvmath::vec3f hit_pos = unprojectScreenPosition({mouse_pos.x, mouse_pos.y, d}, view, proj);

      // Set the interest position
      nvmath::vec3f eye, center, up;
      CameraManip.getLookat(eye, center, up);
      CameraManip.setLookat(eye, hit_pos, up, false);
    }
  }

  //--------------------------------------------------------------------------------------------------
  // Read the depth buffer at the X,Y coordinates
  // Note: depth format is VK_FORMAT_D32_SFLOAT
  //
  float getDepth(int x, int y)
  {
    VkCommandBuffer cmd = m_app->createTempCmdBuffer();

    // Transit the depth buffer image in eTransferSrcOptimal
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    nvvk::cmdBarrierImageLayout(cmd, m_gBuffers->getDepthImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, range);

    // Copy the pixel under the cursor
    VkBufferImageCopy copy_region{};
    copy_region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy_region.imageOffset      = {x, y, 0};
    copy_region.imageExtent      = {1, 1, 1};
    vkCmdCopyImageToBuffer(cmd, m_gBuffers->getDepthImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_pixelBuffer.buffer,
                           1, &copy_region);

    // Put back the depth buffer as  it was
    nvvk::cmdBarrierImageLayout(cmd, m_gBuffers->getDepthImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, range);
    m_app->submitAndWaitTempCmdBuffer(cmd);


    // Grab the value
    float value{1.0F};
    void* mapped = m_alloc->map(m_pixelBuffer);
    switch(m_gBuffers->getDepthFormat())
    {
      case VK_FORMAT_X8_D24_UNORM_PACK32:
      case VK_FORMAT_D24_UNORM_S8_UINT: {
        uint32_t ivalue{0};
        memcpy(&ivalue, mapped, sizeof(uint32_t));
        const uint32_t mask = (1 << 24) - 1;
        ivalue              = ivalue & mask;
        value               = float(ivalue) / float(mask);
      }
      break;
      case VK_FORMAT_D32_SFLOAT: {
        memcpy(&value, mapped, sizeof(float));
      }
      break;
      default:
        assert(!"Wrong Format");
    }
    m_alloc->unmap(m_pixelBuffer);

    return value;
  }

  //--------------------------------------------------------------------------------------------------
  // Return the 3D position of the screen 2D + depth
  //
  nvmath::vec3f unprojectScreenPosition(const nvmath::vec3f& screenPos, const nvmath::mat4f& view, const nvmath::mat4f& proj)
  {
    // Transformation of normalized coordinates between -1 and 1
    const VkExtent2D size = m_gBuffers->getSize();
    nvmath::vec4f    win_norm;
    win_norm.x = screenPos.x / static_cast<float>(size.width) * 2.0F - 1.0F;
    win_norm.y = screenPos.y / static_cast<float>(size.height) * 2.0F - 1.0F;
    win_norm.z = screenPos.z;
    win_norm.w = 1.0;

    // Transform to world space
    const nvmath::mat4f mat       = proj * view;
    const nvmath::mat4f mat_inv   = nvmath::invert(mat);
    nvmath::vec4f       world_pos = mat_inv * win_norm;
    world_pos.w                   = 1.0F / world_pos.w;
    world_pos.x                   = world_pos.x * world_pos.w;
    world_pos.y                   = world_pos.y * world_pos.w;
    world_pos.z                   = world_pos.z * world_pos.w;

    return nvmath::vec3f(world_pos);
  }


  //--------------------------------------------------------------------------------------------------
  //
  //
  nvvkhl::Application*              m_app{nullptr};
  std::unique_ptr<nvvk::DebugUtil>  m_dutil;
  std::shared_ptr<nvvkhl::AllocVma> m_alloc;

  nvmath::vec2f                    m_viewSize    = {0, 0};
  VkFormat                         m_colorFormat = VK_FORMAT_R8G8B8A8_UNORM;       // Color format of the image
  VkFormat                         m_depthFormat = VK_FORMAT_X8_D24_UNORM_PACK32;  // Depth format of the depth buffer
  VkClearColorValue                m_clearColor  = {{0.3F, 0.3F, 0.3F, 1.0F}};     // Clear color
  VkDevice                         m_device      = VK_NULL_HANDLE;                 // Convenient
  std::unique_ptr<nvvkhl::GBuffer> m_gBuffers;                                     // G-Buffers: color + depth
  std::unique_ptr<nvvk::DescriptorSetContainer> m_dset;                            // Descriptor set

  // Resources
  struct PrimitiveMeshVk
  {
    nvvk::Buffer vertices;  // Buffer of the vertices
    nvvk::Buffer indices;   // Buffer of the indices
  };
  std::vector<PrimitiveMeshVk> m_meshVk;
  nvvk::Buffer                 m_frameInfo;
  nvvk::Buffer                 m_pixelBuffer;

  std::vector<VkSampler> m_samplers;

  // Data and setting
  struct Material
  {
    nvmath::vec4f color{1.F};
  };
  std::vector<nvh::PrimitiveMesh> m_meshes;
  std::vector<nvh::Node>          m_nodes;
  std::vector<Material>           m_materials;

  // Pipeline
  PushConstant m_pushConst{};                        // Information sent to the shader
  VkPipeline   m_graphicsPipeline = VK_NULL_HANDLE;  // The graphic pipeline to render
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
int main(int argc, char** argv)
{
  nvvkhl::ApplicationCreateInfo spec;
  spec.name             = PROJECT_NAME " Example";
  spec.vSync            = true;
  spec.vkSetup.apiMajor = 1;
  spec.vkSetup.apiMinor = 3;

  // Create the application
  auto app = std::make_unique<nvvkhl::Application>(spec);

  // Create the test framework
  auto test = std::make_shared<nvvkhl::ElementTesting>(argc, argv);

  // Add all application elements
  app->addElement(test);
  app->addElement(std::make_shared<nvvkhl::ElementCamera>());
  app->addElement(std::make_shared<nvvkhl::ElementDefaultMenu>());
  app->addElement(std::make_shared<nvvkhl::ElementDefaultWindowTitle>());
  app->addElement(std::make_shared<SimplePolygons>());

  app->run();
  app.reset();

  return test->errorCode();
}
