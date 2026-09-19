#version 460

layout(location = 0) in float in_vertical;
layout(location = 0) out vec4 out_color;

void main() {
    const vec3 zenith = vec3(0.25, 0.52, 0.86);
    const vec3 horizon = vec3(0.62, 0.75, 0.90);
    const float blend = smoothstep(0.0, 1.0, in_vertical);
    out_color = vec4(mix(zenith, horizon, blend), 1.0);
}
