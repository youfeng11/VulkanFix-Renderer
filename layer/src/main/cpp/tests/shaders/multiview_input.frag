#version 450
#extension GL_EXT_multiview : require
layout(location=0) flat in vec4 color;
layout(input_attachment_index=0,set=0,binding=1) uniform subpassInput source;
layout(location=0) out vec4 outputColor;
void main() {
    vec3 value = subpassLoad(source).rgb;
    outputColor = vec4(color.rgb + vec3(0,float(gl_ViewIndex)/255.0,0) + value,1);
}
