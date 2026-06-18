#pragma once
#include <QString>
#include <cstdint>
#include <vector>

// Write a valid DNG from pre-extracted RAW16 bytes.
// rawBytes must be width*height*2 bytes (packed, no row padding).
// colorMatrix: 9 floats, XYZ→camera (from ACAMERA_SENSOR_COLOR_TRANSFORM1); may be nullptr for identity.
// Call this from a plain C++ thread — no Android NDK calls are made inside.
bool saveRaw16AsDng(
    const std::vector<uint8_t>& rawBytes,
    int     width,
    int     height,
    const QString& outputPath,
    float   blackLevel,
    float   whiteLevel,
    int     cfaPattern,       // 0=RGGB 1=GRBG 2=GBRG 3=BGGR
    const float* colorMatrix, // 9 floats XYZ→camera, or nullptr for identity
    int     illuminant);      // calibration illuminant (21=D65)
