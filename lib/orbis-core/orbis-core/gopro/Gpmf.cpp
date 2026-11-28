// SPDX-License-Identifier: MIT OR Apache-2.0
#include <orbis-core/gopro/Gpmf.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// GoPro GPMF, by GoPro's documented framing (Gpmf.h). Checked against MAX
// clips (docs/formats/gopro-max.md, *Telemetry*).

namespace orbiscore {
namespace {

// A FourCC: GoPro's are printable ASCII.
bool isTag(std::string_view s) {
    if (s.size() != 4)
        return false;
    for (char c : s)
        if (c < 0x20 || c > 0x7e)
            return false;
    return true;
}

std::string_view tagAt(const uint8_t* d, size_t off) {
    return std::string_view(reinterpret_cast<const char*>(d + off), 4);
}

uint16_t be16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
                                         uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

size_t pad4(size_t n) { return (n + 3) & ~size_t(3); }

// One element of a payload, as a double: GPMF's numbers are big endian. The
// types are GoPro's: b B s S l L j J the integers, q and Q fixed point
// (Q15.16, Q31.32), f d the floats.
double element(std::string_view p, char type, size_t i) {
    const uint8_t* q = reinterpret_cast<const uint8_t*>(p.data());
    const auto fits = [&](size_t w) { return i * w + w <= p.size(); };
    switch (type) {
    case 'b':
    case 'B':
        if (!fits(1))
            return 0;
        return type == 'b' ? double(int8_t(q[i])) : double(q[i]);
    case 's':
    case 'S':
        if (!fits(2))
            return 0;
        return type == 's' ? double(int16_t(be16(q + i * 2))) : double(be16(q + i * 2));
    case 'l':
    case 'L':
        if (!fits(4))
            return 0;
        return type == 'l' ? double(int32_t(be32(q + i * 4))) : double(be32(q + i * 4));
    case 'j':
    case 'J':
        if (!fits(8))
            return 0;
        return type == 'j' ? double(int64_t(be64(q + i * 8))) : double(be64(q + i * 8));
    case 'q':
        if (!fits(4))
            return 0;
        return double(int32_t(be32(q + i * 4))) / 65536.0;
    case 'Q':
        if (!fits(8))
            return 0;
        return double(int64_t(be64(q + i * 8))) / 4294967296.0;
    case 'f': {
        if (!fits(4))
            return 0;
        const uint32_t bits = be32(q + i * 4);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    case 'd': {
        if (!fits(8))
            return 0;
        const uint64_t bits = be64(q + i * 8);
        double dd;
        std::memcpy(&dd, &bits, 8);
        return dd;
    }
    default:
        return 0;
    }
}

std::string text(std::string_view p) {
    const size_t z = p.find('\0');
    return std::string(p.substr(0, z == std::string_view::npos ? p.size() : z));
}

// Walk the tags in [off, end), a buffer or one nest's payload. True when
// the walk reached `end`; false when the visitor stopped it or a tag didn't
// fit, which leaves the rest of that nest unread. Zero bytes where a tag
// would start are padding to the end.
bool walkRange(const uint8_t* d, size_t off, size_t end, const GpmfVisitor& visit, int depth) {
    constexpr int kMaxDepth = 16;
    while (off < end) {
        if (end - off < 8) {
            for (; off < end; ++off)
                if (d[off])
                    return false;
            return true;
        }
        const std::string_view tag = tagAt(d, off);
        if (tag == std::string_view("\0\0\0\0", 4)) {
            off += 4;
            continue;
        }
        if (!isTag(tag))
            return false;
        const uint8_t* h = d + off + 4;
        const size_t total = size_t(h[1]) * be16(h + 2);
        if (total > end - off - 8)
            return false;
        const size_t body = off + 8;

        GpmfEvent ev;
        ev.tag = tag;
        ev.depth = depth;
        if (h[0] == 0) {
            ev.isStruct = true;
            if (!visit(ev))
                return false;
            if (depth >= kMaxDepth || !walkRange(d, body, body + total, visit, depth + 1))
                return false;
        } else {
            ev.type = char(h[0]);
            ev.count = be16(h + 2);
            ev.payload = std::string_view(reinterpret_cast<const char*>(d + body), total);
            if (!visit(ev))
                return false;
        }
        off = body + std::min(pad4(total), end - body);
    }
    return true;
}

// Collects the streams of one sample: each `STRM` nest is one, the streams
// siblings in the sample's `DEVC`. A stream's tags end where the next
// `STRM` (or the sample) starts.
struct Collector {
    GpmfSample* out;
    GpmfStream cur;
    bool open = false;

    bool operator()(const GpmfEvent& e) {
        if (e.isStruct) {
            if (e.tag == "STRM") {
                flush();
                cur = {};
                open = true;
            }
            return true;
        }
        if (!open)
            return true;
        const std::string_view& t = e.tag;
        if (t == "STNM")
            cur.name = text(e.payload);
        else if (t == "SIUN")
            cur.unit = text(e.payload);
        else if (t == "STMP" && e.count)
            cur.stmpUs = int64_t(element(e.payload, e.type, 0));
        else if (t == "TSMP" && e.count)
            cur.tsmp = uint64_t(element(e.payload, e.type, 0));
        else if (t == "SCAL" && e.count)
            cur.scal = element(e.payload, e.type, 0);
        else if (t == "MTRX" && e.payload.size() >= 36) {
            cur.hasMtrx = true;
            for (int i = 0; i < 9; ++i)
                cur.mtrx[i] = element(e.payload, e.type, size_t(i));
        } else if (!e.isStruct && e.count > 1 && !e.payload.empty()) {
            // The stream's rows, under the stream's own tag: GYRO, ACCL, MAGN,
            // and the rest. Whatever carries more than one row carries them.
            cur.dataTag = std::string(t);
            cur.type = e.type;
            cur.count = e.count;
            cur.payload.assign(reinterpret_cast<const uint8_t*>(e.payload.data()),
                               reinterpret_cast<const uint8_t*>(e.payload.data()) +
                                   e.payload.size());
        }
        return true;
    }
    void flush() {
        if (open && !cur.name.empty())
            out->push_back(cur);
        open = false;
    }
};

} // namespace

bool WalkGpmf(const uint8_t* data, size_t size, const GpmfVisitor& visit) {
    if (!data || !size)
        return false;
    return walkRange(data, 0, size, visit, 0);
}

GpmfSample ParseGpmfSensorSample(const uint8_t* data, size_t size) {
    GpmfSample sample;
    if (!data || size < 8)
        return sample;
    Collector c{&sample, {}, false};
    // Forwarded, not moved: the collector keeps the stream it is filling, and
    // `std::function` copies whatever it is given.
    walkRange(data, 0, size, [&](const GpmfEvent& e) { return c(e); }, 0);
    c.flush();
    return sample;
}

std::vector<GpmfSample> ParseGpmfSensorTrack(const uint8_t* data, size_t size) {
    std::vector<GpmfSample> samples;
    if (!data || size < 8)
        return samples;
    // Each top-level DEVC is one sample: walk them by their sizes and parse
    // each on its own.
    walkRange(data, 0, size, [&](const GpmfEvent& e) {
        if (e.depth != 0)
            return true;
        if (e.isStruct && e.tag == "DEVC") {
            const size_t at = size_t(reinterpret_cast<const uint8_t*>(e.tag.data()) - data);
            const size_t len = size_t(data[at + 5]) * be16(data + at + 6);
            GpmfSample s = ParseGpmfSensorSample(data + at, 8 + len);
            if (!s.empty())
                samples.push_back(std::move(s));
        }
        return true;
    }, 0);
    return samples;
}

std::vector<int16_t> GpmfStream::rowsI16() const {
    std::vector<int16_t> rows;
    if (type != 's' && type != 'S')
        return rows;
    const size_t n = payload.size() / 2;
    rows.reserve(n);
    for (size_t i = 0; i < n; ++i)
        rows.push_back(type == 's' ? int16_t(be16(payload.data() + i * 2))
                                   : int16_t(uint16_t(be16(payload.data() + i * 2))));
    return rows;
}

} // namespace orbiscore
