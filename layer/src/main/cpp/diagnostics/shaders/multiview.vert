#version 450
#extension GL_EXT_multiview : require

layout(location = 0) out vec4 color;

void main() {
    vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    int code = gl_ViewIndex + 1;
    color = vec4(float(code) / 32.0, float(code & 1),
                 float((code >> 1) & 1), 1.0);
}
