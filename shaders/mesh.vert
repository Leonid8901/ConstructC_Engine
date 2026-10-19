#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) flat out uint vInstance;

void main() {
    uint idx = drawIndices[gl_InstanceIndex];
    Instance inst = instances[idx];
    vec4 wp = inst.model * vec4(inPos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = mat3(inst.model) * (inNormal * inst.invScale2.xyz);  // M * S^-2 = нормальная матрица для TRS
    vInstance = idx;
    gl_Position = frame.viewProj * wp;
}
