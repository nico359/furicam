#include "rawsave.h"

#include <tiffio.h>
#include <cstring>
#include <cstdio>
#include <cmath>

// CFA pattern bytes for TIFF tag 33422, 2x2 layout
static const uint8_t kCFA[4][4] = {
    {0,1,1,2}, // RGGB
    {1,0,2,1}, // GRBG
    {1,2,0,1}, // GBRG
    {2,1,1,0}, // BGGR
};

// Identity color matrix — neutral fallback when sensor doesn't provide one
static const float kIdentity[9] = {1,0,0, 0,1,0, 0,0,1};

bool saveRaw16AsDng(
    const std::vector<uint8_t>& rawBytes,
    int width, int height,
    const QString& outputPath,
    float blackLevel, float whiteLevel,
    int cfaPattern,
    const float* colorMatrix,
    int illuminant)
{
    fprintf(stderr, "rawsave: start %dx%d buf=%zu\n", width, height, rawBytes.size());
    if ((int)rawBytes.size() < width * height * 2) {
        fprintf(stderr, "rawsave: buffer too small\n");
        return false;
    }
    if (!colorMatrix) colorMatrix = kIdentity;

    TIFF* tif = TIFFOpen(outputPath.toLocal8Bit().constData(), "w");
    if (!tif) { fprintf(stderr, "rawsave: TIFFOpen failed\n"); return false; }

    // ── Standard TIFF fields ──────────────────────────────────────────────────
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH,      (uint32_t)width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH,     (uint32_t)height);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)1);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE,   (uint16_t)16);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,     (uint16_t)32803); // CFA
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG,    (uint16_t)PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP,    (uint32_t)1);
    TIFFSetField(tif, TIFFTAG_COMPRESSION,     (uint16_t)COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_MAKE,            "FuriOS");
    TIFFSetField(tif, TIFFTAG_MODEL,           "FuriCam");

    // ── CFA pattern ───────────────────────────────────────────────────────────
    static const uint16_t kCFADim[] = {2, 2};
    TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, kCFADim);
    TIFFSetField(tif, TIFFTAG_CFAPATTERN, 4, kCFA[cfaPattern & 3]);

    // ── DNG tags — libtiff 4.x has all of these registered internally ─────────
    // DNGVersion 1.4 / backward-compatible 1.1
    static const uint8_t kDngVer[4]  = {1,4,0,0};
    static const uint8_t kDngBkVer[4]= {1,1,0,0};
    TIFFSetField(tif, 50706, kDngVer);   // passcount=0, BYTE[4]
    TIFFSetField(tif, 50707, kDngBkVer);

    // BlackLevelRepeatDim: SHORT[2], passcount=0
    static const uint16_t kBlRep[2] = {2, 2};
    TIFFSetField(tif, 50713, kBlRep);

    // BlackLevel: RATIONAL, variable, passcount=1 → (count, float*)
    float bl[1] = {blackLevel};
    TIFFSetField(tif, 50714, (uint32_t)1, bl);

    // WhiteLevel: LONG, variable, passcount=1 → (count, uint32_t*)
    uint32_t wl[1] = {(uint32_t)whiteLevel};
    TIFFSetField(tif, 50717, (uint32_t)1, wl);

    // CalibrationIlluminant1: SHORT[1], passcount=0
    TIFFSetField(tif, 50778, (uint16_t)illuminant);

    // ColorMatrix1: SRATIONAL[9], passcount=1 → (count, float*)
    // Mirrors ACAMERA_SENSOR_COLOR_TRANSFORM1: transforms XYZ → camera native
    TIFFSetField(tif, 50721, (uint32_t)9, colorMatrix);

    // ── Pixel data ────────────────────────────────────────────────────────────
    fprintf(stderr, "rawsave: writing %d scanlines\n", height);
    const uint8_t* rowPtr = rawBytes.data();
    size_t rowBytes = (size_t)width * 2;
    for (int row = 0; row < height; row++) {
        if (TIFFWriteScanline(tif, (void*)rowPtr, row, 0) < 0) {
            fprintf(stderr, "rawsave: TIFFWriteScanline failed at row %d\n", row);
            TIFFClose(tif);
            return false;
        }
        rowPtr += rowBytes;
    }

    TIFFClose(tif);
    fprintf(stderr, "rawsave: wrote %dx%d DNG to %s\n", width, height,
            outputPath.toLocal8Bit().constData());
    return true;
}

