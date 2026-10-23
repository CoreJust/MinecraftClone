#version 460
#extension GL_GOOGLE_include_directive : require
#include "world_curvature.glsl"

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec3 inCameraOffset;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = inColor;
    if (inCameraOffset.z > 0.5) {
        if (length(inCameraOffset.xy) >= inCameraOffset.z) { discard; }
        outColor.rgb = mix(outColor.rgb, vec3(0.62, 0.75, 0.90), worldVisibilityFog(inCameraOffset.xy, inCameraOffset.z));
    }
}
