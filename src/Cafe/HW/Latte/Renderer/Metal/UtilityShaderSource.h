#pragma once

#define __STRINGIFY(x) #x
#define _STRINGIFY(x) __STRINGIFY(x)

constexpr const char* utilityShaderSource = R"(#include <metal_stdlib>
using namespace metal;

#define GET_BUFFER_BINDING(index) (28 + index)
#define GET_TEXTURE_BINDING(index) (29 + index)
#define GET_SAMPLER_BINDING(index) (14 + index)

constant float2 positions[] = {float2(-1.0, -3.0), float2(-1.0, 1.0), float2(3.0, 1.0)};

struct VertexOut {
    float4 position [[position]];
    float2 texCoord;
};

vertex VertexOut vertexFullscreen(ushort vid [[vertex_id]]) {
    VertexOut out;
    out.position = float4(positions[vid], 0.0, 1.0);
    out.texCoord = positions[vid] * 0.5 + 0.5;
    out.texCoord.y = 1.0 - out.texCoord.y;

    return out;
}

//fragment float4 fragmentPresent(VertexOut in [[stage_in]], texture2d<float> tex [[texture(0)]], //sampler samplr [[sampler(0)]]) {
//    return tex.sample(samplr, in.texCoord);
//}

vertex void vertexCopyBufferToBuffer(uint vid [[vertex_id]], device uint8_t* src [[buffer(GET_BUFFER_BINDING(0))]], device uint8_t* dst [[buffer(GET_BUFFER_BINDING(1))]]) {
    dst[vid] = src[vid];
}

fragment float4 fragmentCopyDepthToColor(VertexOut in [[stage_in]], texture2d<float, access::read> src [[texture(GET_TEXTURE_BINDING(0))]]) {
    return float4(src.read(uint2(in.position.xy)).r, 0.0, 0.0, 0.0);
}

struct SurfaceCopyDepthOut {
    float depth [[depth(any)]];
};

fragment SurfaceCopyDepthOut fragmentCopyColorToDepth(VertexOut in [[stage_in]],
                                                       texture2d<float, access::read> src [[texture(GET_TEXTURE_BINDING(0))]]) {
    SurfaceCopyDepthOut out;
    out.depth = src.read(uint2(in.position.xy)).r;
    return out;
}

struct SurfaceResampleParams {
    uint2 sourceExtent;
    uint2 destinationExtent;
};

fragment float4 fragmentResampleColorFloat(VertexOut in [[stage_in]],
                                            texture2d<float> src [[texture(GET_TEXTURE_BINDING(0))]],
                                            sampler surfaceSampler [[sampler(GET_SAMPLER_BINDING(0))]]) {
    return src.sample(surfaceSampler, in.texCoord);
}

fragment uint4 fragmentResampleColorUint(VertexOut in [[stage_in]],
                                         texture2d<uint, access::read> src [[texture(GET_TEXTURE_BINDING(0))]],
                                         constant SurfaceResampleParams& params [[buffer(GET_BUFFER_BINDING(0))]]) {
    uint2 destinationCoord = min(uint2(in.position.xy), params.destinationExtent - 1);
    uint2 sourceCoord = min(((destinationCoord * 2 + 1) * params.sourceExtent) / (params.destinationExtent * 2),
                            params.sourceExtent - 1);
    return src.read(sourceCoord);
}

fragment int4 fragmentResampleColorSint(VertexOut in [[stage_in]],
                                        texture2d<int, access::read> src [[texture(GET_TEXTURE_BINDING(0))]],
                                        constant SurfaceResampleParams& params [[buffer(GET_BUFFER_BINDING(0))]]) {
    uint2 destinationCoord = min(uint2(in.position.xy), params.destinationExtent - 1);
    uint2 sourceCoord = min(((destinationCoord * 2 + 1) * params.sourceExtent) / (params.destinationExtent * 2),
                            params.sourceExtent - 1);
    return src.read(sourceCoord);
}

struct SurfaceDepthOut {
    float depth [[depth(any)]];
};

fragment SurfaceDepthOut fragmentResampleDepth(VertexOut in [[stage_in]],
                                               depth2d<float> src [[texture(GET_TEXTURE_BINDING(0))]],
                                               sampler surfaceSampler [[sampler(GET_SAMPLER_BINDING(0))]]) {
    SurfaceDepthOut out;
    out.depth = src.sample(surfaceSampler, in.texCoord);
    return out;
}

//struct RestrideParams {
//    uint oldStride;
//    uint newStride;
//};

//vertex void vertexRestrideBuffer(uint vid [[vertex_id]], device uint8_t* src [[buffer//(GET_BUFFER_BINDING(0))]], device uint8_t* dst [[buffer(GET_BUFFER_BINDING(1))]], constant //RestrideParams& params [[buffer(GET_BUFFER_BINDING(2))]]) {
//    for (uint32_t i = 0; i < params.oldStride; i++) {
//        dst[vid * params.newStride + i] = src[vid * params.oldStride + i];
//    }
//}
)";
