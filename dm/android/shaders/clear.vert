#version 450
// The masked clears' full-target triangle (gfx_vk_draw.inc clear_draw): the scissor limits it, depth from the push constants
layout(push_constant) uniform PC { vec4 color; float depth; } pc;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, pc.depth, 1.0);
}
