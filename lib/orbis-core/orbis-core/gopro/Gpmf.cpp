// SPDX-License-Identifier: MIT OR Apache-2.0
#include <orbis-core/gopro/Gpmf.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// GoPro GPMF, as a Max writes it: see Gpmf.h for the framing and why two of
// its details are stated at length. Everything here was checked against
// MAX clips (docs/formats/gopro-max.md, *Telemetry*).

namespace orbiscore {
namespace {

bool isTag(std::string_view s) {
    if (s.size() != 4)
        return false;
    for (char c : s) {
        const bool upper = c >= 'A' && c <= 'Z';
        const bool digit = c >= '0' && c <= '9';
        if (!upper && !digit)
            return false;
    }
    return true;
}

std::string_view tagAt(const uint8_t* d, size_t off) {
    return std::string_view(reinterpret_cast<const char*>(d + off), 4);
}

bool isTypeChar(char c) {
    return std::string_view("cCsSLldfdFBJJqQnNgG").find(c) != std::string_view::npos;
}

uint16_t be16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
                                         uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

size_t pad4(size_t n) { return (n + 3) & ~size_t(3); }

// One element of a payload, as a double: GPMF's numbers are big endian.
double element(std::string_view p, char type, size_t i) {
    const uint8_t* q = reinterpret_cast<const uint8_t*>(p.data());
    switch (type) {
    case 's':
    case 'S':
        if (i * 2 + 2 > p.size())
            return 0;
        return type == 's' ? double(int16_t(be16(q + i * 2))) : double(be16(q + i * 2));
    case 'l':
    case 'L':
        if (i * 4 + 4 > p.size())
            return 0;
        return type == 'l' ? double(int32_t(be32(q + i * 4))) : double(be32(q + i * 4));
    case 'q':
    case 'Q':
    case 'j':
    case 'J':
        if (i * 8 + 8 > p.size())
            return 0;
        return type == 'q' || type == 'j' ? double(int64_t(be64(q + i * 8)))
                                          : double(be64(q + i * 8));
    case 'f': {
        if (i * 4 + 4 > p.size())
            return 0;
        const uint32_t bits = be32(q + i * 4);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    case 'd': {
        if (i * 8 + 8 > p.size())
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

// Walk the tags in [off, end), returning the offset reached. A struct's
// declared size is an aggregate for the whole track, not for this buffer (a
// 10 KB sample declares a 76 KB `DEVC`), so it is clamped to `end`.
size_t walkRange(const uint8_t* d, size_t off, size_t end, const GpmfVisitor& visit, int depth) {
    constexpr int kMaxDepth = 16;
    while (off + 8 <= end) {
        const std::string_view tag = tagAt(d, off);
        if (!isTag(tag))
            return off;
        const uint8_t* h = d + off + 4;

        if (isTypeChar(char(h[0]))) {
            const uint64_t per = h[1];
            const uint64_t count = be16(h + 2);
            const uint64_t total = per * (count ? count : 1);
            const size_t next = off + 8 + pad4(size_t(total));
            // A scalar only when its payload fits and another tag (or the end)
            // follows it: that check is what separates a scalar from a struct
            // whose payload happens to start with a type char.
            if (total && off + 8 + total <= end && (next >= end || isTag(tagAt(d, next)))) {
                GpmfEvent ev;
                ev.tag = tag;
                ev.type = char(h[0]);
                ev.count = count;
                ev.payload = std::string_view(reinterpret_cast<const char*>(d + off + 8),
                                              size_t(total));
                if (!visit(ev))
                    return next;
                off = next;
                continue;
            }
        }

        const uint32_t sz = be32(h);
        if (sz && off + 12 <= end && isTag(tagAt(d, off + 8))) {
            GpmfEvent ev;
            ev.tag = tag;
            ev.isStruct = true;
            ev.depth = depth;
            if (!visit(ev))
                return off + 8;
            if (depth < kMaxDepth)
                walkRange(d, off + 8, std::min(off + size_t(sz) + 8, end), visit, depth + 1);
            off += 8 + std::min<size_t>(sz, end - off - 8);
            continue;
        }
        return off;
    }
    return end;
}

// Collects the streams of one sample. A `STRM` starts one: the streams nest
// (the gyroscope sits inside the accelerometer's), and every field of a
// stream comes before the stream nested in it, so starting a new one on entry
// is what the layout asks for.
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
    return walkRange(data, 0, size, visit, 0) == size;
}

GpmfSample ParseGpmfSensorSample(const uint8_t* data, size_t size) {
    GpmfSample sample;
    if (!data || size < 12)
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
    if (!data || size < 12)
        return samples;
    // Where each sample starts. A reader that demuxes the track gets these for
    // free (one packet each, with its PTS) and should call
    // ParseGpmfSensorSample per packet; this is for a track pulled out in one
    // go, which has nothing else to go on.
    //
    // The declared sizes can't do it: they are aggregates, so walking by size
    // jumps clean over the following samples — a 10 KB sample declares a 76 KB
    // `DEVC` holding a 67 KB `STRM`. What does mark a start is the sample's
    // own shape: a `DEVC` whose payload opens with `DVID`.
    std::vector<size_t> start;
    for (size_t off = 0; off + 16 <= size; off += 4) {
        if (tagAt(data, off) != "DEVC")
            continue;
        const uint32_t sz = be32(data + off + 4);
        if (sz && tagAt(data, off + 8) == "DVID")
            start.push_back(off);
    }
    for (size_t i = 0; i + 1 < start.size(); ++i) {
        GpmfSample s = ParseGpmfSensorSample(data + start[i], start[i + 1] - start[i]);
        if (!s.empty())
            samples.push_back(std::move(s));
    }
    if (!start.empty()) {
        GpmfSample s = ParseGpmfSensorSample(data + start.back(), size - start.back());
        if (!s.empty())
            samples.push_back(std::move(s));
    }
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
