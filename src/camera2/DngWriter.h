// DngWriter — minimal, dependency-free DNG (TIFF/EP) writer for a single-frame
// CFA (Bayer) raw, built from Camera2 RAW16 buffers + the sensor's DNG color
// characteristics.  Produces a spec-compliant, color-accurate DNG that darktable,
// RawTherapee, Lightroom and libraw/dcraw all open.
//
// The raw is stored uncompressed in one IFD0 strip (PhotometricInterpretation =
// ColorFilterArray).  All values are little-endian ("II").
#pragma once

#include <cstdint>
#include <string>

struct DngParams {
    int      width          = 0;     // RAW16 image width  (pixels)
    int      height         = 0;     // RAW16 image height (pixels)
    int      rowStrideBytes = 0;     // source bytes per row (>= width*2)

    int      whiteLevel     = 1023;
    int      blackLevel[4]  = {64, 64, 64, 64};  // 2x2 CFA order (row-major)
    int      cfaArrangement = 0;     // 0=RGGB 1=GRBG 2=GBRG 3=BGGR

    // Valid pixel region within the raw frame: {top,left,bottom,right} (exclusive
    // right/bottom).  Zeroes => whole frame.
    int      activeTop = 0, activeLeft = 0, activeBottom = 0, activeRight = 0;

    // Color calibration (row-major 3x3).  Matrix1 is mandatory; matrix2/forward
    // are optional (haveColor2 / haveForward gate them).
    double   colorMatrix1[9]   = {0};   // XYZ(illuminant1)->camera  (DNG ColorMatrix1)
    double   colorMatrix2[9]   = {0};
    double   forwardMatrix1[9] = {0};   // camera->XYZ(D50)          (DNG ForwardMatrix1)
    double   forwardMatrix2[9] = {0};
    bool     haveColor2  = false;
    bool     haveForward = false;
    int      illuminant1 = 21;          // EXIF LightSource code (21=D65, 17=Std A)
    int      illuminant2 = 17;

    double   asShotNeutral[3] = {1, 1, 1};   // camera-neutral of the scene white

    int      iso        = 0;            // for EXIF ISOSpeedRatings (0 = omit)
    int64_t  exposureNs = 0;            // for EXIF ExposureTime    (0 = omit)
    int      orientationDeg = 0;        // 0/90/180/270 clockwise to upright

    std::string uniqueModel = "FuriPhone FLX1s";
    std::string make        = "FuriPhone";
    std::string model       = "FLX1s";
};

// Writes `pixels` (16-bit little-endian, `height` rows of `rowStrideBytes`) as a
// DNG at `path`.  Returns false and (if non-null) fills *err on failure.
bool writeDng(const std::string& path, const uint16_t* pixels,
              const DngParams& p, std::string* err = nullptr);
