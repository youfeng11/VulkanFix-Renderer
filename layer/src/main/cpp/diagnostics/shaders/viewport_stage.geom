#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices=3) out;
layout(location=0) flat out vec4 color;
void outputVertex(int i) {
    // Rebuild the position rather than simply passing a VS position through.
    const vec2 p[3] = vec2[3](vec2(-0.75,-0.75),vec2(0.75,-0.75),vec2(0,0.75));
    gl_Position = vec4(p[i].x, -p[i].y, 0, 1);
    gl_Position = vec4(gl_Position.x, -gl_Position.y, gl_Position.zw);
    color = vec4(0,1,0,1);
    EmitVertex();
}
void main() {
    for (int i=0; i<3; ++i) outputVertex(i);
    EndPrimitive();
}
