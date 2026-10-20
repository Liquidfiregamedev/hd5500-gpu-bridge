#version 450
// v13.43 test: per-instance offsets from a storage buffer WITHOUT 'readonly' (like Waylith's
// _InstanceTransforms, which the bridge must expose as a graphics-stage UAV).
layout(set = 0, binding = 0) buffer Inst { vec4 off[]; } inst;
layout(push_constant) uniform Push { vec4 shift; } pc;
vec2 pos[3] = vec2[](vec2(-0.3, 0.3), vec2(0.3, 0.3), vec2(0.0, -0.3));
void main() { gl_Position = vec4(pos[gl_VertexIndex] + inst.off[gl_InstanceIndex].xy + pc.shift.xy, 0.0, 1.0); }
