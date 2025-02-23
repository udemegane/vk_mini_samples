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

#include "device_host.h"
#include "functions.hlsli"

// Per-vertex attributes to be assembled from bound vertex buffers.
struct VSin
{
  [[vk::location(0)]] float3 position : POSITION;
  [[vk::location(1)]] float3 normal : NORMAl;
  [[vk::location(2)]] float2 uv : TEXCOORD0;
};

// Output of the vertex shader, and input to the fragment shader.
struct PSin
{
  float3 position : POSITION;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
};

// Output of the vertex shader
struct VSout
{
  PSin stage;
  float4 sv_position : SV_Position;
};

// Output of the fragment shader
struct PSout
{
  float4 color : SV_Target;
};


[[vk::push_constant]] ConstantBuffer<PushConstant> pushConst;
[[vk::binding(0, 0)]] ConstantBuffer<FrameInfo> frameInfo;
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] Texture2D myTexture;
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] SamplerState mySampler;

// Vertex  Shader
[shader("vertex")]
VSout vertexMain(VSin input)
{
  float4 pos = mul(pushConst.transfo, float4(input.position.xyz, 1.0));

  VSout output;
  output.sv_position = mul(frameInfo.proj, mul(frameInfo.view, float4(pos)));
  output.stage.normal = input.normal;
  output.stage.position = pos.xyz;
  output.stage.uv = input.uv;

  return output;
}


// Fragment Shader
[shader("pixel")]
PSout fragmentMain(PSin stage)
{
  float3 V = normalize(frameInfo.camPos - stage.position);
  float3 color = simpleShading(V, V, stage.normal);
  color *= myTexture.Sample(mySampler, stage.uv).xyz;
  
  PSout output;
  output.color = float4(color, pushConst.color.w);
  
  return output;
}
