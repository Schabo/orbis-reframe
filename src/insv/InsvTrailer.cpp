// SPDX-License-Identifier: MIT OR Apache-2.0
#include "InsvTrailer.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <numbers>
#include <vector>

namespace insv {

namespace {

constexpr char kMagic[] = "8db42d694ccc418790edff439fe026bf"; // literal ASCII, 32 chars
constexpr size_t kMagicLen = 32;
constexpr size_t kFooterLen = 78;
constexpr int kMaxRecords = 200000; // safety cap against a malformed/truncated trailer

uint16_t ReadU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64LE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

double ReadDoubleLE(const uint8_t* p) {
    const uint64_t bits = ReadU64LE(p);
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

double ReadFloatLE(const uint8_t* p) {
    const uint32_t bits = ReadU32LE(p);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Walks the trailer's records from EOF toward the start, calling
// `visit(record_id, payload)` for each data record. `visit` returns false to
// stop the walk early. Returns false if the file has no recognizable trailer
// (magic mismatch / too short).
//
// Two traversal modes, matching ExifTool's ProcessInsta360:
//   - sequential: each record's 6-byte (id, len) header sits immediately before
//     the next record's payload; step back by len + 6 each time.
//   - directory table: if a record 0x0 with a non-empty payload is seen, it is
//     a table of 10-byte {id:u16, size:u32, offset:u32} entries locating every
//     record by its offset from the start of the trailer. Newer cameras (X5
//     included) only make the 0x300/0x400 records reachable this way — the
//     sequential chain dead-ends at a run of empty 0x0 records after 0x200.
//     The entry also gives the record's id and size, so no header is read.
// The 0x0 directory record itself is consumed here, never handed to `visit`.
// `wanted`, when set, picks the record ids to read: the others are stepped
// over without reading their payload (the IMU record of a long clip is
// hundreds of MB).
bool WalkTrailerRecords(
    std::istream& file, int64_t file_size,
    const std::function<bool(uint16_t, const std::vector<uint8_t>&, int64_t)>& visit,
    const std::function<bool(uint16_t)>& wanted = {}) {
    if (file_size < static_cast<int64_t>(kFooterLen)) return false;

    std::vector<uint8_t> footer(kFooterLen);
    file.seekg(file_size - static_cast<int64_t>(kFooterLen), std::ios::beg);
    file.read(reinterpret_cast<char*>(footer.data()), kFooterLen);
    if (!file) return false;

    if (std::memcmp(footer.data() + (kFooterLen - kMagicLen), kMagic, kMagicLen) != 0) {
        return false; // not an Insta360 trailer (or a format this doesn't recognize)
    }
    const int64_t trailer_len = static_cast<int64_t>(ReadU32LE(footer.data() + 38));

    // epos is a signed offset relative to EOF. At epos == -78 (the footer's own
    // start) the first 6 bytes of the footer are the (id, len) header of the
    // record whose payload sits immediately before the footer.
    int64_t epos = -static_cast<int64_t>(kFooterLen);
    uint16_t id = ReadU16LE(footer.data());
    uint32_t len = ReadU32LE(footer.data() + 2);

    std::vector<uint8_t> dir_table;
    size_t dir_pos = 0;
    bool using_dir = false;

    for (int record = 0; record < kMaxRecords; ++record) {
        epos -= static_cast<int64_t>(len);
        if (epos + trailer_len < 0) break;

        const int64_t payload_off = file_size + epos; // byte offset of the payload in `file`
        const bool read = id == 0x0 || !wanted || wanted(id);
        file.seekg(payload_off, std::ios::beg);
        std::vector<uint8_t> payload(read ? len : 0);
        if (read && len > 0) {
            file.read(reinterpret_cast<char*>(payload.data()), len);
            if (!file) break;
        }

        if (id == 0x0 && len > 0 && !using_dir) {
            dir_table = std::move(payload); // record-locator table, not a data record
            using_dir = true;
        } else if (read && !(id == 0x0 && len == 0)) {
            if (!visit(id, payload, payload_off)) return true;
        }

        // Advance to the next record. With the Offsets table, its entry gives
        // the record's type and size: the Antigravity A1 puts each record in a
        // 256 KiB slot with no header after it, and on the X5 the header
        // repeats the entry. The entry is {type, format}, the header
        // {format, type}, so swap to the header's id.
        if (using_dir) {
            bool advanced = false;
            while (dir_pos + 10 <= dir_table.size()) {
                const uint16_t d_id = ReadU16LE(&dir_table[dir_pos]);
                const uint32_t d_siz = ReadU32LE(&dir_table[dir_pos + 2]);
                const uint32_t d_off = ReadU32LE(&dir_table[dir_pos + 6]);
                dir_pos += 10;
                if (d_id != 0 && d_siz != 0 &&
                    static_cast<int64_t>(d_off) + d_siz < trailer_len) {
                    epos = static_cast<int64_t>(d_off) + d_siz - trailer_len;
                    id = static_cast<uint16_t>((d_id >> 8) | (d_id << 8));
                    len = d_siz;
                    advanced = true;
                    break;
                }
            }
            if (!advanced) break;
            continue;
        }
        epos -= 6;
        if (epos + trailer_len < 0) break;

        file.seekg(file_size + epos, std::ios::beg);
        uint8_t header[6];
        file.read(reinterpret_cast<char*>(header), 6);
        if (!file) break;
        id = ReadU16LE(header);
        len = ReadU32LE(header + 2);
    }
    return true;
}

// Record 0x101 ("maker notes") packs up to 4 sub-fields as (tag:1B, len:1B,
// value:len bytes) run sequentially from the start of the payload.
void ParseMakerNotes(const std::vector<uint8_t>& payload, InsvMakerNotes& out) {
    size_t p = 0;
    for (int i = 0; i < 4 && p + 2 <= payload.size(); ++i) {
        const uint8_t tag = payload[p];
        const uint8_t n = payload[p + 1];
        if (p + 2 + n > payload.size()) break;
        std::string val(reinterpret_cast<const char*>(&payload[p + 2]), n);
        switch (tag) {
            case 0x0a: out.serial_number = val; break;
            case 0x12: out.model = val; break;
            case 0x1a: out.firmware = val; break;
            case 0x2a: // Parameters: underscore-delimited in the file
                std::replace(val.begin(), val.end(), '_', ' ');
                out.parameters = val;
                break;
            default: break;
        }
        p += 2 + n;
    }
}

// The subset of record 0x101 (the `ExtraMetadata` protobuf, docs/formats/insta360-x5.md
// §5) that timing and IMU scaling need, and the colour profile.
struct ImuMetadata {
    uint64_t first_frame_timestamp = 0; // field 24
    double gyro_timestamp_ms = 0;       // field 28
    double rolling_shutter_ms = 0;      // field 25
    int is_raw_gyro = -1;               // field 62 (-1 = absent)
    double acc_range_g = 16;            // field 65.1; telemetry-parser's defaults
    double gyro_range_dps = 2000;       // field 65.2
    std::string gamma_mode;             // field 22
    std::vector<double> imu_mount;      // field 69, {w, x, y, z} when complete
};

bool ReadVarint(const std::vector<uint8_t>& b, size_t& i, uint64_t& v) {
    v = 0;
    for (int shift = 0; i < b.size() && shift < 64; shift += 7) {
        const uint8_t c = b[i++];
        v |= static_cast<uint64_t>(c & 0x7f) << shift;
        if (!(c & 0x80)) return true;
    }
    return false;
}

// Flat protobuf walk: calls `visit(field, wire_type, varint, bytes)`; `bytes`
// holds the payload of fixed64 / fixed32 / length-delimited fields.
void WalkProtobuf(const std::vector<uint8_t>& b,
                  const std::function<void(uint32_t, int, uint64_t,
                                           const std::vector<uint8_t>&)>& visit) {
    size_t i = 0;
    while (i < b.size()) {
        uint64_t key, v = 0;
        if (!ReadVarint(b, i, key)) return;
        const int wt = static_cast<int>(key & 7);
        size_t n = 0;
        if (wt == 0) {
            if (!ReadVarint(b, i, v)) return;
        } else if (wt == 1) {
            n = 8;
        } else if (wt == 5) {
            n = 4;
        } else if (wt == 2) {
            if (!ReadVarint(b, i, v)) return;
            n = static_cast<size_t>(v);
        } else {
            return; // groups / unknown: stop rather than misparse
        }
        if (n > b.size() - i) return;
        const std::vector<uint8_t> bytes(b.begin() + i, b.begin() + i + n);
        i += n;
        visit(static_cast<uint32_t>(key >> 3), wt, v, bytes);
    }
}

ImuMetadata ParseImuMetadata(const std::vector<uint8_t>& payload) {
    ImuMetadata m;
    WalkProtobuf(payload, [&](uint32_t f, int wt, uint64_t v, const std::vector<uint8_t>& b) {
        if (f == 24 && wt == 0) m.first_frame_timestamp = v;
        else if (f == 25 && wt == 1) m.rolling_shutter_ms = ReadDoubleLE(b.data());
        else if (f == 28 && wt == 1) m.gyro_timestamp_ms = ReadDoubleLE(b.data());
        else if (f == 62 && wt == 0) m.is_raw_gyro = v ? 1 : 0;
        else if (f == 22 && wt == 2) m.gamma_mode.assign(b.begin(), b.end());
        // Repeated floats; the A1 writes them unpacked, accept packed too.
        else if (f == 69 && wt == 5) m.imu_mount.push_back(ReadFloatLE(b.data()));
        else if (f == 69 && wt == 2)
            for (size_t k = 0; k + 4 <= b.size(); k += 4)
                m.imu_mount.push_back(ReadFloatLE(&b[k]));
        else if (f == 65 && wt == 2) {
            WalkProtobuf(b, [&](uint32_t g, int gwt, uint64_t gv, const std::vector<uint8_t>&) {
                if (gwt != 0 || gv == 0) return;
                if (g == 1) m.acc_range_g = static_cast<double>(gv);
                else if (g == 2) m.gyro_range_dps = static_cast<double>(gv);
            });
        }
    });
    return m;
}

// Record 0x300 (IMU). Two on-disk layouts: 20-byte records pack 6 raw 16-bit
// counts (value = raw - 0x8000) scaled by the metadata's full-scale ranges
// (field 65: count / (32768 / range) -> g and deg/s; docs/formats/insta360-x5.md §6.1);
// 56-byte records pack 6 little-endian doubles already in g and rad/s. Both
// are prefixed by a uint64 device timestamp. First 3 values are the
// accelerometer, last 3 the gyro. The layout comes from field 62 (is_raw_gyro)
// when present, else ExifTool's length heuristic.
void ParseImu(const std::vector<uint8_t>& p, const ImuMetadata& meta, std::size_t max_samples,
              std::vector<ImuSample>& out) {
    const size_t len = p.size();
    if (len < 20) return;

    int dlen;
    if (meta.is_raw_gyro == 1 && len % 20 == 0) {
        dlen = 20;
    } else if (meta.is_raw_gyro == 0 && len % 56 == 0) {
        dlen = 56;
    } else if (len % 20 != 0 && len % 56 == 0) {
        dlen = 56;
    } else if (len % 56 != 0 && len % 20 == 0) {
        dlen = 20;
    } else {
        // Divisible by both (or neither): a 56-byte record's bytes 16..18 are
        // the top of a double and read as 0x000000; a 20-byte record's are a
        // scaled sample and don't.
        dlen = (len >= 19 && p[16] == 0 && p[17] == 0 && p[18] == 0) ? 56 : 20;
    }

    constexpr double kDegToRad = std::numbers::pi / 180.0;
    const double accelPerCount = meta.acc_range_g / 32768.0;                // g
    const double gyroPerCount = meta.gyro_range_dps / 32768.0 * kDegToRad; // rad/s
    for (size_t off = 0; off + static_cast<size_t>(dlen) <= len; off += dlen) {
        if (max_samples != 0 && out.size() >= max_samples) break;
        ImuSample s;
        s.timestamp = ReadU64LE(&p[off]);
        if (dlen == 56) {
            for (int i = 0; i < 3; ++i) s.accel[i] = ReadDoubleLE(&p[off + 8 + 8 * i]);
            for (int i = 0; i < 3; ++i) s.gyro[i] = ReadDoubleLE(&p[off + 32 + 8 * i]);
        } else {
            for (int i = 0; i < 6; ++i) {
                const uint16_t raw = ReadU16LE(&p[off + 8 + 2 * i]);
                const double count = static_cast<int>(raw) - 0x8000;
                if (i < 3)
                    s.accel[i] = count * accelPerCount;
                else
                    s.gyro[i - 3] = count * gyroPerCount;
            }
        }
        out.push_back(s);
    }
}

// Record 128 (TimeMap): {u32 headerSize (32), u32 trimCount, u32 pairBytes,
// u32 frameCount, ...} header, then trimCount x 3 doubles (duration, source
// start, source end; ms), then pairBytes/16 (video ms, source ms) f64 pairs
// (layout per telemetry-parser; field meanings from the sample "-trim" clip).
// Returns the median source-minus-video offset, ms; 0 if unparseable.
double ParseTimeMapOffsetMs(const std::vector<uint8_t>& p) {
    if (p.size() < 16) return 0.0;
    const uint32_t hdr = ReadU32LE(&p[0]);
    const uint32_t trims = ReadU32LE(&p[4]);
    const uint32_t pairBytes = ReadU32LE(&p[8]);
    const size_t start = static_cast<size_t>(hdr) + static_cast<size_t>(trims) * 24;
    if (hdr < 16 || trims > 1000 || start + pairBytes > p.size()) return 0.0;
    std::vector<double> diff;
    diff.reserve(pairBytes / 16);
    for (size_t off = start; off + 16 <= start + pairBytes; off += 16)
        diff.push_back(ReadDoubleLE(&p[off + 8]) - ReadDoubleLE(&p[off]));
    if (diff.empty()) return 0.0;
    std::nth_element(diff.begin(), diff.begin() + diff.size() / 2, diff.end());
    return diff[diff.size() / 2];
}

// Field 145 of record 0x101: repeated {1: name, 2: 6 x double poly} profiles,
// then repeated {1: name, 2: double fov} entries (docs/formats/insta360-x5.md §5.4).
void ParseLensProfiles(const std::vector<uint8_t>& payload, std::vector<LensProfile>& out) {
    auto entry = [&](const std::string& name) -> LensProfile& {
        for (auto& p : out)
            if (p.name == name) return p;
        out.push_back(LensProfile{name});
        return out.back();
    };
    WalkProtobuf(payload, [&](uint32_t f, int wt, uint64_t, const std::vector<uint8_t>& b) {
        if (f != 145 || wt != 2) return;
        WalkProtobuf(b, [&](uint32_t g, int gwt, uint64_t, const std::vector<uint8_t>& e) {
            if ((g != 1 && g != 2) || gwt != 2) return;
            std::string name;
            std::vector<double> vals;
            WalkProtobuf(e, [&](uint32_t h, int hwt, uint64_t, const std::vector<uint8_t>& v) {
                if (h == 1 && hwt == 2) {
                    name.assign(v.begin(), v.end());
                } else if (h == 2 && hwt == 1) { // double (what the X5 writes)
                    vals.push_back(ReadDoubleLE(v.data()));
                } else if (h == 2 && hwt == 5) {
                    vals.push_back(ReadFloatLE(v.data()));
                } else if (h == 2 && hwt == 2) { // packed doubles
                    for (size_t k = 0; k + 8 <= v.size(); k += 8) vals.push_back(ReadDoubleLE(&v[k]));
                }
            });
            if (name.empty()) return;
            LensProfile& p = entry(name);
            if (g == 1) {
                for (size_t k = 0; k < vals.size() && k < 6; ++k) p.poly[k] = vals[k];
            } else if (!vals.empty()) {
                p.fov_deg = vals[0];
            }
        });
    });
}

// Record 0x400 (exposure): fixed 16-byte records — uint64 timestamp, double
// exposure time in seconds.
void ParseExposure(const std::vector<uint8_t>& p, std::vector<ExposureSample>& out) {
    constexpr size_t kRec = 16;
    for (size_t off = 0; off + kRec <= p.size(); off += kRec) {
        ExposureSample s;
        s.timestamp = ReadU64LE(&p[off]);
        s.exposure_time = ReadDoubleLE(&p[off + 8]);
        out.push_back(s);
    }
}

// Record 0x2500 (attitude, the A1's flight controller): fixed 36-byte records
// -- u64 timestamp, quaternion x, y, z, w, velocity north, east, down, all
// float (docs/formats/antigravity-a1.md). Non-unit quaternions are dropped.
void ParseAttitude(const std::vector<uint8_t>& p, std::vector<AttitudeSample>& out) {
    constexpr size_t kRec = 36;
    out.reserve(out.size() + p.size() / kRec);
    for (size_t off = 0; off + kRec <= p.size(); off += kRec) {
        AttitudeSample s;
        s.timestamp = ReadU64LE(&p[off]);
        const double x = ReadFloatLE(&p[off + 8]), y = ReadFloatLE(&p[off + 12]),
                     z = ReadFloatLE(&p[off + 16]), w = ReadFloatLE(&p[off + 20]);
        const double n = std::sqrt(w * w + x * x + y * y + z * z);
        if (!(std::fabs(n - 1.0) < 0.01)) continue;
        s.q[0] = w / n, s.q[1] = x / n, s.q[2] = y / n, s.q[3] = z / n;
        for (int k = 0; k < 3; ++k)
            s.velocity_ned[k] = ReadFloatLE(&p[off + 24 + 4 * k]);
        out.push_back(s);
    }
}

// Record 0x700 (GPS): fixed 53-byte records -- u64 Unix seconds, u16 ms, then
// an 'A' (acquired) / 'V' (void) byte ahead of the position. Counts records
// and fixes; the position fields are never read. The X5 writes records at
// 10 Hz but the fix only updates once a second, so each fix repeats over ~10
// records with the same timestamp. The rate is the median step between
// distinct timestamps, not count / span: the track can drop out for minutes
// mid-recording.
void CountGps(const std::vector<uint8_t>& p, InsvTelemetry& out) {
    constexpr size_t kRec = 53;
    const size_t n = p.size() / kRec;
    if (n == 0) return;
    out.gps_records = n;
    for (size_t i = 0; i < n; ++i)
        if (p[i * kRec + 10] == 'A') ++out.gps_fixes;
    auto timeMs = [&](size_t i) {
        return static_cast<double>(ReadU64LE(&p[i * kRec])) * 1000.0 +
               ReadU16LE(&p[i * kRec + 8]);
    };
    std::vector<double> stepsMs;
    for (size_t i = 1; i < n; ++i) {
        const double d = timeMs(i) - timeMs(i - 1);
        if (d > 0) stepsMs.push_back(d);
    }
    if (stepsMs.empty()) return;
    const size_t distinct = stepsMs.size() + 1;
    auto mid = stepsMs.begin() + stepsMs.size() / 2;
    std::nth_element(stepsMs.begin(), mid, stepsMs.end());
    out.gps_rate_hz = 1000.0 / *mid;
    out.gps_track_s = distinct * *mid / 1000.0;
}

// Field 98 of record 0x101: {1: first frame device us, 2: last, 3: first
// frame Unix ms, 4: last, 5: frame count, ...} (docs/formats/insta360-x5.md
// §5.1). 0 when absent.
int64_t FirstFrameUnixMs(const std::vector<uint8_t>& payload) {
    int64_t out = 0;
    WalkProtobuf(payload, [&](uint32_t f, int wt, uint64_t, const std::vector<uint8_t>& b) {
        if (f != 98 || wt != 2) return;
        WalkProtobuf(b, [&](uint32_t g, int gwt, uint64_t gv, const std::vector<uint8_t>&) {
            if (g == 3 && gwt == 0) out = static_cast<int64_t>(gv);
        });
    });
    return out;
}

} // namespace

std::vector<InsvGpsFix> ParseInsvGps(const std::vector<uint8_t>& p) {
    // u64 Unix seconds, u16 ms, 'A' / 'V', f64 latitude, 'N' / 'S',
    // f64 longitude, 'E' / 'W' (ExifTool has also seen 'O' for west),
    // f64 speed m/s, f64 track degrees, f64 altitude m.
    constexpr size_t kRec = 53;
    std::vector<InsvGpsFix> out;
    for (size_t off = 0; off + kRec <= p.size(); off += kRec) {
        const uint8_t* r = &p[off];
        if (r[10] != 'A') continue;
        InsvGpsFix f;
        f.unix_ms = static_cast<int64_t>(ReadU64LE(r)) * 1000 + ReadU16LE(r + 8);
        // A repeat of the fix already kept (the 10 Hz records, a lost
        // signal), or one out of order.
        if (!out.empty() && f.unix_ms <= out.back().unix_ms) continue;
        f.latitude = std::abs(ReadDoubleLE(r + 11));
        if (r[19] == 'S') f.latitude = -f.latitude;
        f.longitude = std::abs(ReadDoubleLE(r + 20));
        if (r[28] == 'W' || r[28] == 'O') f.longitude = -f.longitude;
        f.speed_mps = ReadDoubleLE(r + 29);
        f.heading_deg = ReadDoubleLE(r + 37);
        f.altitude_m = ReadDoubleLE(r + 45);
        if (!std::isfinite(f.latitude) || !std::isfinite(f.longitude) || f.latitude > 90.0 ||
            f.longitude > 180.0 || !std::isfinite(f.speed_mps) || !std::isfinite(f.heading_deg) ||
            !std::isfinite(f.altitude_m))
            continue;
        out.push_back(f);
    }
    return out;
}

std::optional<InsvGps> ReadInsvGps(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return std::nullopt;
    file.seekg(0, std::ios::end);
    const int64_t file_size = static_cast<int64_t>(file.tellg());
    InsvGps out;
    const bool ok = WalkTrailerRecords(
        file, file_size,
        [&](uint16_t id, const std::vector<uint8_t>& payload, int64_t) {
            if (id == 0x101) out.first_frame_unix_ms = FirstFrameUnixMs(payload);
            else out.fixes = ParseInsvGps(payload);
            return true;
        },
        [](uint16_t id) { return id == 0x101 || id == 0x700; });
    if (!ok) return std::nullopt;
    return out;
}

const LensProfile* FindLensProfile(const InsvMakerNotes& notes, const std::string& name) {
    for (const auto& p : notes.lens_profiles)
        if (p.name == name) return &p;
    return nullptr;
}

std::optional<InsvMakerNotes> ReadInsvMakerNotes(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return std::nullopt;
    file.seekg(0, std::ios::end);
    const int64_t file_size = static_cast<int64_t>(file.tellg());

    InsvMakerNotes result;
    bool found = false;
    const bool ok = WalkTrailerRecords(
        file, file_size, [&](uint16_t id, const std::vector<uint8_t>& payload, int64_t) {
            if (id == 0x101) {
                ParseMakerNotes(payload, result);
                ParseLensProfiles(payload, result.lens_profiles);
                WalkProtobuf(payload, [&](uint32_t f, int wt, uint64_t v,
                                          const std::vector<uint8_t>& b) {
                    if (f == 7 && wt == 0) result.creation_time = v;
                    if (f == 54 && wt == 2) result.offset_v3.assign(b.begin(), b.end());
                    if (f != 27 || wt != 2) return;
                    WalkProtobuf(b, [&](uint32_t g, int gwt, uint64_t gv,
                                        const std::vector<uint8_t>&) {
                        // {1: srcW, 2: srcH, 3: dstW, 4: dstH}
                        if (g == 3 && gwt == 0)
                            result.crop_dst_w = static_cast<double>(gv);
                    });
                });
                found = true;
                return false; // this is the only record type we need
            }
            return true;
        });

    if (!ok || !found) return std::nullopt;
    return result;
}

std::optional<InsvTelemetry> ReadInsvTelemetry(const std::string& file_path,
                                               std::size_t max_imu_samples, double tick_us) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return std::nullopt;
    file.seekg(0, std::ios::end);
    const int64_t file_size = static_cast<int64_t>(file.tellg());

    InsvTelemetry out;
    // The IMU payload is scaled by ranges from 0x101, and the walk order isn't
    // guaranteed, so hold it until the walk is done.
    std::vector<uint8_t> imuPayload;
    ImuMetadata meta;
    const bool ok = WalkTrailerRecords(
        file, file_size, [&](uint16_t id, const std::vector<uint8_t>& payload, int64_t) {
            if (id == 0x101) {
                meta = ParseImuMetadata(payload);
            } else if (id == 0x300) {
                imuPayload = payload;
            } else if (id == 0x400) {
                ParseExposure(payload, out.exposure);
            } else if (id == 0x2500) {
                ParseAttitude(payload, out.attitude);
            } else if (id == 0x700) {
                CountGps(payload, out);
            } else if (id == 0x8000) {
                out.time_map_offset_ms = ParseTimeMapOffsetMs(payload);
            }
            return true; // want every telemetry record in the trailer
        });

    if (!ok) return std::nullopt;
    ParseImu(imuPayload, meta, max_imu_samples, out.imu);
    out.first_frame_timestamp = meta.first_frame_timestamp;
    if (tick_us != 1.0) {
        const auto us = [tick_us](uint64_t t) {
            return static_cast<uint64_t>(std::llround(static_cast<double>(t) * tick_us));
        };
        for (ImuSample& s : out.imu)
            s.timestamp = us(s.timestamp);
        for (ExposureSample& e : out.exposure)
            e.timestamp = us(e.timestamp);
        for (AttitudeSample& a : out.attitude)
            a.timestamp = us(a.timestamp);
        out.first_frame_timestamp = us(out.first_frame_timestamp);
    }
    if (meta.imu_mount.size() == 4) {
        const double* m = meta.imu_mount.data();
        const double n = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2] + m[3] * m[3]);
        if (std::fabs(n - 1.0) < 0.01) {
            out.has_imu_mount = true;
            for (int k = 0; k < 4; ++k)
                out.imu_mount[k] = m[k] / n;
        }
    }
    out.gyro_timestamp_ms = meta.gyro_timestamp_ms;
    out.rolling_shutter_ms = meta.rolling_shutter_ms;
    out.gamma_mode = meta.gamma_mode;
    return out;
}

std::optional<InsvThumbnail> ReadInsvThumbnail(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return std::nullopt;
    file.seekg(0, std::ios::end);
    const int64_t file_size = static_cast<int64_t>(file.tellg());

    // 40-byte header, then the picture: u32 1, u32 record size, u32 1, u32 0,
    // u32 width, u32 height, zeros (docs/formats/insta360-x5.md §4.3).
    constexpr size_t kHeader = 40;
    std::optional<InsvThumbnail> out;
    WalkTrailerRecords(
        file, file_size,
        [&](uint16_t, const std::vector<uint8_t>& p, int64_t) {
            if (p.size() < kHeader) return false;
            const uint32_t w = ReadU32LE(&p[16]);
            const uint32_t h = ReadU32LE(&p[20]);
            if (w == 0 || h == 0 || w % 2 || h % 2 || w > 8192 || h > 8192 ||
                p.size() != kHeader + size_t(w) * h * 3 / 2)
                return false; // another layout (other models: JPEG / H.264)
            out = InsvThumbnail{static_cast<int>(w), static_cast<int>(h),
                                std::vector<uint8_t>(p.begin() + kHeader, p.end())};
            return false;
        },
        [](uint16_t id) { return id == 0x200; });
    return out;
}

std::optional<std::vector<uint8_t>> ReadInsvRecord(const std::string& file_path, uint16_t id) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return std::nullopt;
    file.seekg(0, std::ios::end);
    const int64_t file_size = static_cast<int64_t>(file.tellg());
    std::optional<std::vector<uint8_t>> out;
    WalkTrailerRecords(
        file, file_size,
        [&](uint16_t, const std::vector<uint8_t>& p, int64_t) {
            out = p;
            return false;
        },
        [id](uint16_t r) { return r == id; });
    return out;
}

std::optional<std::string> ReadInsvExtendedCalibrationString(const std::string& file_path) {
    const std::vector<uint8_t> trailer = ExtractRawTrailer(file_path);
    if (trailer.empty()) return std::nullopt;

    auto is_num = [](uint8_t c) {
        return (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E';
    };
    auto is_sep = [](uint8_t c) { return c == '_' || c == ' '; };

    std::string best;
    int best_tokens = 0;
    const size_t n = trailer.size();
    for (size_t i = 0; i + 2 < n; ++i) {
        // Start of a numeric run: a digit not preceded by a run char.
        if (!(trailer[i] >= '0' && trailer[i] <= '9')) continue;
        if (i > 0 && (is_num(trailer[i - 1]) || is_sep(trailer[i - 1]))) continue;
        // Consume [num|sep]* -- the calibration string is one such run.
        size_t j = i;
        int tokens = 1;
        bool prev_sep = false;
        while (j < n && (is_num(trailer[j]) || is_sep(trailer[j]))) {
            if (is_sep(trailer[j]) && !prev_sep && j > i) ++tokens;
            prev_sep = is_sep(trailer[j]);
            ++j;
        }
        if (tokens < 55) { i = j; continue; }
        // Must look like the extended lens string: leading count "2".
        std::string run(reinterpret_cast<const char*>(&trailer[i]), j - i);
        if (run.rfind("2_", 0) != 0 && run.rfind("2 ", 0) != 0) { i = j; continue; }
        if (tokens > best_tokens) {
            best_tokens = tokens;
            best = std::move(run);
        }
        i = j;
    }
    if (best_tokens < 55) return std::nullopt;
    return best;
}

std::vector<uint8_t> ExtractRawTrailer(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return {};

    file.seekg(0, std::ios::end);
    const int64_t file_size = static_cast<int64_t>(file.tellg());

    int64_t pos = 0;
    while (pos + 8 <= file_size) {
        file.seekg(pos, std::ios::beg);
        uint8_t header[8];
        file.read(reinterpret_cast<char*>(header), 8);
        if (!file) break;

        const uint32_t size32 = (static_cast<uint32_t>(header[0]) << 24) |
                                 (static_cast<uint32_t>(header[1]) << 16) |
                                 (static_cast<uint32_t>(header[2]) << 8) | header[3];
        const bool is_moov = std::memcmp(header + 4, "moov", 4) == 0;

        int64_t box_size;
        if (size32 == 1) { // 64-bit extended size follows the 8-byte header
            uint8_t ext[8];
            file.read(reinterpret_cast<char*>(ext), 8);
            if (!file) break;
            box_size = 0;
            for (uint8_t b : ext) box_size = (box_size << 8) | b;
        } else if (size32 == 0) {
            box_size = file_size - pos; // box extends to EOF
        } else {
            box_size = size32;
        }
        if (box_size < 8) break; // malformed box, bail out

        if (is_moov) {
            const int64_t moov_end = pos + box_size;
            if (moov_end >= file_size) return {}; // nothing follows moov
            std::vector<uint8_t> trailer(static_cast<size_t>(file_size - moov_end));
            file.seekg(moov_end, std::ios::beg);
            file.read(reinterpret_cast<char*>(trailer.data()), trailer.size());
            if (!file) return {};
            return trailer;
        }
        pos += box_size;
    }
    return {}; // no moov box found
}

} // namespace insv
