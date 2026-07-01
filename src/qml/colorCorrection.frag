#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D source;

layout(std140, binding = 0) uniform cbuffer {
    mat4 qt_Matrix;
    float qt_Opacity;
    float redScale;
    float greenScale;
    float blueScale;
    float saturation;
};

void main() {
    vec4 c = texture(source, qt_TexCoord0);

    vec3 corrected = vec3(
        clamp(c.r * redScale,   0.0, 1.0),
        clamp(c.g * greenScale, 0.0, 1.0),
        clamp(c.b * blueScale,  0.0, 1.0)
    );

    float lum = dot(corrected, vec3(0.299, 0.587, 0.114));
    corrected = clamp(mix(vec3(lum), corrected, saturation), 0.0, 1.0);

    fragColor = vec4(corrected, c.a) * qt_Opacity;
}
