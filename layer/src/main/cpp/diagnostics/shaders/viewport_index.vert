#version 450
#extension GL_ARB_shader_viewport_layer_array : require

layout(location = 0) flat out vec4 color;

void main() {
    gl_ViewportIndex = 1;
    vec2 positions[3] = vec2[](
        vec2(-0.75, -0.75),
        vec2( 0.75, -0.75),
        vec2( 0.0,   0.75));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    color = vec4(0.0, 1.0, 0.0, 1.0);
}
