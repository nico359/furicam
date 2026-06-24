// DngWriter implementation — see DngWriter.h.
//
// Builds a little-endian TIFF/EP with IFD0 holding the uncompressed CFA strip
// plus an EXIF sub-IFD for shutter/ISO.  Entries are added in any order and
// sorted ascending at serialize time (TIFF requires ascending tags).
#include "DngWriter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// TIFF field types.
enum { T_BYTE = 1, T_ASCII = 2, T_SHORT = 3, T_LONG = 4, T_RATIONAL = 5,
       T_UNDEFINED = 7, T_SRATIONAL = 10 };

inline int typeSize(int t) {
    switch (t) {
        case T_BYTE: case T_ASCII: case T_UNDEFINED: return 1;
        case T_SHORT:                                 return 2;
        case T_LONG:                                  return 4;
        case T_RATIONAL: case T_SRATIONAL:            return 8;
    }
    return 1;
}

inline void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x & 0xff)); v.push_back((uint8_t)(x >> 8));
}
inline void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x & 0xff));        v.push_back((uint8_t)((x >> 8) & 0xff));
    v.push_back((uint8_t)((x >> 16) & 0xff)); v.push_back((uint8_t)((x >> 24) & 0xff));
}

struct Entry {
    uint16_t tag = 0, type = 0;
    uint32_t count = 0;
    std::vector<uint8_t> payload;   // count * typeSize bytes
};

// Encode a double as a (numerator, denominator) pair with a fixed denominator.
inline void encodeRational(std::vector<uint8_t>& v, double val, bool sign) {
    const double DEN = 1000000.0;
    if (sign) {
        int32_t num = (int32_t)(val * DEN >= 0 ? val * DEN + 0.5 : val * DEN - 0.5);
        put32(v, (uint32_t)num); put32(v, (uint32_t)(int32_t)DEN);
    } else {
        uint32_t num = (uint32_t)(val * DEN + 0.5);
        put32(v, num); put32(v, (uint32_t)DEN);
    }
}

class Ifd {
public:
    void addShort(uint16_t tag, std::initializer_list<uint16_t> vals) {
        Entry e; e.tag = tag; e.type = T_SHORT; e.count = (uint32_t)vals.size();
        for (uint16_t x : vals) put16(e.payload, x);
        entries_.push_back(std::move(e));
    }
    void addLong(uint16_t tag, std::initializer_list<uint32_t> vals) {
        Entry e; e.tag = tag; e.type = T_LONG; e.count = (uint32_t)vals.size();
        for (uint32_t x : vals) put32(e.payload, x);
        entries_.push_back(std::move(e));
    }
    void addByte(uint16_t tag, std::initializer_list<uint8_t> vals) {
        Entry e; e.tag = tag; e.type = T_BYTE; e.count = (uint32_t)vals.size();
        for (uint8_t x : vals) e.payload.push_back(x);
        entries_.push_back(std::move(e));
    }
    void addUndefined(uint16_t tag, const char* s, int n) {
        Entry e; e.tag = tag; e.type = T_UNDEFINED; e.count = (uint32_t)n;
        for (int i = 0; i < n; ++i) e.payload.push_back((uint8_t)s[i]);
        entries_.push_back(std::move(e));
    }
    void addAscii(uint16_t tag, const std::string& s) {
        Entry e; e.tag = tag; e.type = T_ASCII; e.count = (uint32_t)s.size() + 1;
        e.payload.assign(s.begin(), s.end()); e.payload.push_back(0);
        entries_.push_back(std::move(e));
    }
    void addRational(uint16_t tag, const double* vals, int n, bool sign) {
        Entry e; e.tag = tag; e.type = sign ? T_SRATIONAL : T_RATIONAL; e.count = (uint32_t)n;
        for (int i = 0; i < n; ++i) encodeRational(e.payload, vals[i], sign);
        entries_.push_back(std::move(e));
    }
    // Patch an already-added single-LONG entry (used for forward references like
    // StripOffsets and the EXIF IFD pointer, resolved once the layout is known).
    void setLong1(uint16_t tag, uint32_t value) {
        for (auto& e : entries_)
            if (e.tag == tag) { e.payload.clear(); put32(e.payload, value); return; }
    }

    uint32_t byteSize() const { return 2u + 12u * (uint32_t)entries_.size() + 4u; }
    uint32_t extraSize() const {
        uint32_t n = 0;
        for (const auto& e : entries_)
            if (e.payload.size() > 4) n += (uint32_t)((e.payload.size() + 1) & ~1u);
        return n;
    }

    // Append this IFD (header + entries + nextIfd + out-of-line data) to `out`.
    // `ifdOffset` is this IFD's absolute file offset; `nextIfd` the offset of the
    // following IFD (0 = none).
    void serialize(uint32_t ifdOffset, uint32_t nextIfd, std::vector<uint8_t>& out) const {
        std::vector<Entry> es = entries_;
        std::sort(es.begin(), es.end(),
                  [](const Entry& a, const Entry& b) { return a.tag < b.tag; });

        const uint32_t extraBase = ifdOffset + byteSize();
        std::vector<uint8_t> extra;
        put16(out, (uint16_t)es.size());
        for (const auto& e : es) {
            put16(out, e.tag);
            put16(out, e.type);
            put32(out, e.count);
            if (e.payload.size() <= 4) {
                uint8_t field[4] = {0, 0, 0, 0};
                std::memcpy(field, e.payload.data(), e.payload.size());
                out.insert(out.end(), field, field + 4);
            } else {
                put32(out, extraBase + (uint32_t)extra.size());
                extra.insert(extra.end(), e.payload.begin(), e.payload.end());
                if (extra.size() & 1) extra.push_back(0);   // word-align
            }
        }
        put32(out, nextIfd);
        out.insert(out.end(), extra.begin(), extra.end());
    }

private:
    std::vector<Entry> entries_;
};

}  // namespace

bool writeDng(const std::string& path, const uint16_t* pixels,
              const DngParams& p, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (!pixels || p.width <= 0 || p.height <= 0) return fail("invalid raw params");

    const uint32_t imgBytes = (uint32_t)p.width * (uint32_t)p.height * 2u;

    // CFA pattern (DNG codes 0=R 1=G 2=B), per arrangement.
    uint8_t cfa[4];
    switch (p.cfaArrangement) {
        case 1:  cfa[0]=1; cfa[1]=0; cfa[2]=2; cfa[3]=1; break;  // GRBG
        case 2:  cfa[0]=1; cfa[1]=2; cfa[2]=0; cfa[3]=1; break;  // GBRG
        case 3:  cfa[0]=2; cfa[1]=1; cfa[2]=1; cfa[3]=0; break;  // BGGR
        default: cfa[0]=0; cfa[1]=1; cfa[2]=1; cfa[3]=2; break;  // RGGB
    }

    uint16_t orient;
    switch (((p.orientationDeg % 360) + 360) % 360) {
        case 90:  orient = 6; break;
        case 180: orient = 3; break;
        case 270: orient = 8; break;
        default:  orient = 1; break;
    }

    int aTop = p.activeTop, aLeft = p.activeLeft;
    int aBot = p.activeBottom > 0 ? p.activeBottom : p.height;
    int aRight = p.activeRight > 0 ? p.activeRight : p.width;
    const uint32_t cropW = (uint32_t)(aRight - aLeft), cropH = (uint32_t)(aBot - aTop);

    // ── IFD0 ────────────────────────────────────────────────────────────────
    Ifd ifd0;
    ifd0.addLong (254, {0});                          // NewSubFileType = full-res primary
    ifd0.addLong (256, {(uint32_t)p.width});          // ImageWidth
    ifd0.addLong (257, {(uint32_t)p.height});         // ImageLength
    ifd0.addShort(258, {16});                         // BitsPerSample
    ifd0.addShort(259, {1});                          // Compression = none
    ifd0.addShort(262, {32803});                      // Photometric = CFA
    ifd0.addAscii(271, p.make);                       // Make
    ifd0.addAscii(272, p.model);                      // Model
    ifd0.addLong (273, {0});                          // StripOffsets (patched below)
    ifd0.addShort(274, {orient});                     // Orientation
    ifd0.addShort(277, {1});                          // SamplesPerPixel
    ifd0.addLong (278, {(uint32_t)p.height});         // RowsPerStrip
    ifd0.addLong (279, {imgBytes});                   // StripByteCounts
    ifd0.addShort(284, {1});                          // PlanarConfiguration
    ifd0.addAscii(305, "furicam2");                   // Software
    ifd0.addShort(33421, {2, 2});                     // CFARepeatPatternDim
    ifd0.addByte (33422, {cfa[0], cfa[1], cfa[2], cfa[3]});   // CFAPattern
    ifd0.addLong (34665, {0});                        // ExifIFD pointer (patched below)
    ifd0.addByte (50706, {1, 4, 0, 0});               // DNGVersion 1.4
    ifd0.addByte (50707, {1, 1, 0, 0});               // DNGBackwardVersion 1.1
    ifd0.addAscii(50708, p.uniqueModel);              // UniqueCameraModel
    ifd0.addShort(50713, {2, 2});                     // BlackLevelRepeatDim
    {
        double bl[4] = {(double)p.blackLevel[0], (double)p.blackLevel[1],
                        (double)p.blackLevel[2], (double)p.blackLevel[3]};
        ifd0.addRational(50714, bl, 4, /*sign*/false); // BlackLevel
    }
    ifd0.addShort(50717, {(uint16_t)p.whiteLevel});   // WhiteLevel
    ifd0.addLong (50719, {0, 0});                     // DefaultCropOrigin (rel. ActiveArea)
    ifd0.addLong (50720, {cropW, cropH});             // DefaultCropSize
    ifd0.addRational(50721, p.colorMatrix1, 9, /*sign*/true);   // ColorMatrix1
    {
        double identity[9] = {1,0,0, 0,1,0, 0,0,1};
        ifd0.addRational(50723, identity, 9, true);   // CameraCalibration1
        if (p.haveColor2) ifd0.addRational(50724, identity, 9, true); // CameraCalibration2
    }
    if (p.haveColor2)
        ifd0.addRational(50722, p.colorMatrix2, 9, true);          // ColorMatrix2
    ifd0.addRational(50728, p.asShotNeutral, 3, /*sign*/false);    // AsShotNeutral
    ifd0.addShort(50778, {(uint16_t)p.illuminant1});  // CalibrationIlluminant1
    if (p.haveColor2)
        ifd0.addShort(50779, {(uint16_t)p.illuminant2});           // CalibrationIlluminant2
    ifd0.addLong (50829, {(uint32_t)aTop, (uint32_t)aLeft,
                          (uint32_t)aBot, (uint32_t)aRight});       // ActiveArea
    if (p.haveForward) {
        ifd0.addRational(50964, p.forwardMatrix1, 9, true);        // ForwardMatrix1
        if (p.haveColor2) ifd0.addRational(50965, p.forwardMatrix2, 9, true);
    }

    // ── EXIF sub-IFD ─────────────────────────────────────────────────────────
    Ifd exif;
    exif.addUndefined(36864, "0230", 4);              // ExifVersion 2.30
    if (p.exposureNs > 0) {
        double et = (double)p.exposureNs / 1e9;
        exif.addRational(33434, &et, 1, /*sign*/false);  // ExposureTime
    }
    if (p.iso > 0)
        exif.addShort(34855, {(uint16_t)p.iso});      // ISOSpeedRatings

    // ── Layout & patch forward references ────────────────────────────────────
    const uint32_t ifd0Off = 8;
    const uint32_t exifOff = ifd0Off + ifd0.byteSize() + ifd0.extraSize();
    const uint32_t exifEnd = exifOff + exif.byteSize() + exif.extraSize();
    const uint32_t imgOff  = (exifEnd + 1u) & ~1u;    // word-align the strip

    ifd0.setLong1(273, imgOff);     // StripOffsets
    ifd0.setLong1(34665, exifOff);  // ExifIFD

    // ── Emit ─────────────────────────────────────────────────────────────────
    std::vector<uint8_t> buf;
    buf.reserve(imgOff + imgBytes);
    buf.push_back('I'); buf.push_back('I');           // little-endian
    put16(buf, 42);
    put32(buf, ifd0Off);
    ifd0.serialize(ifd0Off, /*nextIfd*/0, buf);
    exif.serialize(exifOff, /*nextIfd*/0, buf);
    while (buf.size() < imgOff) buf.push_back(0);

    // CFA strip: tightly packed width*2 per row, copied from the strided source.
    const size_t rowBytes = (size_t)p.width * 2;
    const size_t stride = p.rowStrideBytes > 0 ? (size_t)p.rowStrideBytes : rowBytes;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(pixels);
    size_t base = buf.size();
    buf.resize(base + imgBytes);
    for (int y = 0; y < p.height; ++y)
        std::memcpy(buf.data() + base + (size_t)y * rowBytes, src + (size_t)y * stride, rowBytes);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return fail("cannot open DNG for writing");
    bool ok = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (!ok) return fail("short write");
    return true;
}
