#version 450
#extension GL_EXT_multiview : require
layout(location=0) flat in vec4 color;
layout(location=0) out vec4 outputColor;
void main() {
    outputColor = vec4(color.r,color.g+float(gl_ViewIndex)/255.0,color.b,1);
}
