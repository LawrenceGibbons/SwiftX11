//
//  X11TexturedQuad.metal
//  SwiftX11
//
//  Created by Lawrence Gibbons on 2/21/26.
//

#include <metal_stdlib>
using namespace metal;

struct VSIn {
  float2 pos [[attribute(0)]];
  float2 uv  [[attribute(1)]];
};

struct VSOut {
  float4 position [[position]];
  float2 uv;
};

vertex VSOut texturedQuadVS(VSIn in [[stage_in]]) {
  VSOut out;
  out.position = float4(in.pos, 0.0, 1.0);
  out.uv = in.uv;
  return out;
}

fragment float4 texturedQuadFS(VSOut in [[stage_in]],
                               texture2d<float> tex [[texture(0)]],
                               sampler samp [[sampler(0)]]) {
  // BGRA8Unorm sampled as float4; return directly.
  return tex.sample(samp, in.uv);
}

