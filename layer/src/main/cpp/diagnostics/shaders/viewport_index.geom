#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices=3) out;
layout(location=0) flat out vec4 color;
void main() {
    for (int i=0; i<3; ++i) {
        gl_Position = gl_in[i].gl_Position;
        gl_ViewportIndex = 1;
        color = vec4(0,1,0,1);
        EmitVertex();
    }
    EndPrimitive();
}
