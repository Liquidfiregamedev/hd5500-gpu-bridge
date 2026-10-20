#version 450
layout(set = 1, binding = 0) uniform sampler2D ui_texture;
layout(set = 1, binding = 1, std140) uniform UiParams {
    vec4 tint;
} params;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = texture(ui_texture, in_uv) * params.tint;
}
