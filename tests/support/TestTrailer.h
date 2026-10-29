#pragma once

// Insta360 trailers built record by record, for tests that need a camera's
// metadata, calibration or telemetry without footage. Header-only, no
// dependencies.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace testtrailer {

// Little-endian / protobuf writers for building trailer records.
struct Bytes {
    std::vector<uint8_t> b;
    Bytes& u8(uint8_t v) { b.push_back(v); return *this; }
    Bytes& u16(uint16_t v) { for (int i = 0; i < 2; ++i) b.push_back(uint8_t(v >> (8 * i))); return *this; }
    Bytes& u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); return *this; }
    Bytes& u64(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i))); return *this; }
    Bytes& f64(double v) { uint64_t u; std::memcpy(&u, &v, 8); return u64(u); }
    Bytes& f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u32(u); }
    Bytes& raw(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); return *this; }
    Bytes& str(const std::string& v) { b.insert(b.end(), v.begin(), v.end()); return *this; }
    Bytes& varint(uint64_t v) {
        do {
            uint8_t c = v & 0x7f;
            v >>= 7;
            b.push_back(uint8_t(c | (v ? 0x80 : 0)));
        } while (v);
        return *this;
    }
    // Protobuf fields.
    Bytes& key(uint32_t field, int wt) { return varint((uint64_t(field) << 3) | uint64_t(wt)); }
    Bytes& pbVarint(uint32_t field, uint64_t v) { return key(field, 0).varint(v); }
    Bytes& pbDouble(uint32_t field, double v) { return key(field, 1).f64(v); }
    Bytes& pbFloat(uint32_t field, float v) { return key(field, 5).f32(v); }
    Bytes& pbBytes(uint32_t field, const std::vector<uint8_t>& v) { return key(field, 2).varint(v.size()).raw(v); }
    Bytes& pbString(uint32_t field, const std::string& v) {
        return pbBytes(field, std::vector<uint8_t>(v.begin(), v.end()));
    }
};

// One 53-byte record of the GPS record 0x700 (docs/formats/insta360-x5.md
// §8): Unix time, 'A' / 'V', latitude and longitude as positive degrees with
// their hemisphere letters, speed m/s, heading, altitude m.
inline std::vector<uint8_t> gpsRecord(uint64_t unixMs, char status, double latitude, char ns,
                                      double longitude, char ew, double speedMps,
                                      double headingDeg, double altitudeM)
{
    Bytes r;
    r.u64(unixMs / 1000).u16(uint16_t(unixMs % 1000)).u8(uint8_t(status));
    r.f64(latitude).u8(uint8_t(ns)).f64(longitude).u8(uint8_t(ew));
    r.f64(speedMps).f64(headingDeg).f64(altitudeM);
    return r.b;
}

struct TrailerRecord {
    uint16_t id; // the header's (type << 8 | format)
    std::vector<uint8_t> payload;
};

// The `inst` trailer to EOF: the records, each payload followed by its 6-byte
// header, and the 78-byte footer. `offsetsTable`: as the X5 writes it, with
// an offsets table (record 0x0) locating the records and named by the
// footer's header, so only the table reaches them; else as the ONE X2 writes
// it, a plain chain whose last header is the footer's. `slotBytes` (with the
// table): as the Antigravity A1 writes it, each record at the start of a slot
// this size and no header after it, the rest of the slot filled with bytes
// that read as a bogus header.
inline std::vector<uint8_t> insvTrailer(const std::vector<TrailerRecord>& records,
                                        bool offsetsTable, size_t slotBytes = 0)
{
    Bytes trailer;
    Bytes footer;
    if (offsetsTable) {
        Bytes table;
        for (const TrailerRecord& r : records) {
            const uint32_t offset = uint32_t(trailer.b.size());
            trailer.raw(r.payload);
            if (slotBytes == 0) {
                trailer.u16(r.id).u32(uint32_t(r.payload.size()));
            } else {
                while (trailer.b.size() % slotBytes != 0)
                    trailer.u8(0xa5);
            }
            // Table entries spell the id the other way round: {type, format}.
            table.u8(uint8_t(r.id >> 8)).u8(uint8_t(r.id)).u32(uint32_t(r.payload.size())).u32(offset);
        }
        table.u8(0).u8(0).u32(0).u32(0); // an empty placeholder, skipped
        trailer.raw(table.b);
        footer.u16(0x0).u32(uint32_t(table.b.size()));
    } else {
        for (size_t i = 0; i < records.size(); ++i) {
            trailer.raw(records[i].payload);
            if (i + 1 < records.size())
                trailer.u16(records[i].id).u32(uint32_t(records[i].payload.size()));
        }
        footer.u16(records.back().id).u32(uint32_t(records.back().payload.size()));
    }
    // 32 zero bytes, written a word at a time: GCC 14 mis-sizes a bulk
    // insert or resize here (a -Wstringop-overflow false positive).
    for (int i = 0; i < 4; ++i)
        footer.u64(0);
    const uint32_t trailerLen = uint32_t(trailer.b.size() + 78);
    footer.u32(trailerLen).u32(3).str("8db42d694ccc418790edff439fe026bf");
    trailer.raw(footer.b);
    return trailer.b;
}

} // namespace testtrailer
