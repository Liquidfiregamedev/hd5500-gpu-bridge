#version 450
// v13.47 test: the instance buffer lives in SET 1. The pipeline's layout has no storage buffer in
// set 0, but the descriptor sets are bound with another layout whose set 0 holds one (Waylith-
// style: sets bound with a sibling layout; lavapipe/real drivers address by set+binding).
layout(set = 1, binding = 0) buffer Inst { vec4 off[]; } inst;
layout(push_constant) uniform Push { vec4 shift; } pc;
vec2 pos[3] = vec2[](vec2(-0.3, 0.3), vec2(0.3, 0.3), vec2(0.0, -0.3));
void main() { gl_Position = vec4(pos[gl_VertexIndex] + inst.off[gl_InstanceIndex].xy + pc.shift.xy, 0.0, 1.0); }
