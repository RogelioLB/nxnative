#version 450
// The clear colour to every target: the pipeline's write masks keep the one being cleared, and the channels asked for
layout(push_constant) uniform PC { vec4 color; float depth; } pc;
layout(location = 0) out vec4 o0; layout(location = 1) out vec4 o1; layout(location = 2) out vec4 o2; layout(location = 3) out vec4 o3;
layout(location = 4) out vec4 o4; layout(location = 5) out vec4 o5; layout(location = 6) out vec4 o6; layout(location = 7) out vec4 o7;
void main() { o0 = o1 = o2 = o3 = o4 = o5 = o6 = o7 = pc.color; }
