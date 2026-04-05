uniform sampler2D source;
uniform lowp float qt_Opacity;
uniform lowp float redScale;
uniform lowp float greenScale;
uniform lowp float blueScale;
uniform lowp float saturation;
varying highp vec2 qt_TexCoord0;

void main() {
    lowp vec4 c = texture2D(source, qt_TexCoord0);

    // Per-channel color correction
    lowp vec3 corrected = vec3(
        clamp(c.r * redScale,   0.0, 1.0),
        clamp(c.g * greenScale, 0.0, 1.0),
        clamp(c.b * blueScale,  0.0, 1.0)
    );

    // Saturation adjustment (mix toward luminance)
    lowp float lum = dot(corrected, vec3(0.299, 0.587, 0.114));
    corrected = clamp(mix(vec3(lum), corrected, saturation), 0.0, 1.0);

    gl_FragColor = vec4(corrected, c.a) * qt_Opacity;
}
