#pragma once

// Zips for tests, laid out as PKWARE's APPNOTE.TXT says: stored or deflated
// entries (zlib), no data descriptors, no zip64.

#include <zlib.h>

#include <cstdint>
#include <string>
#include <vector>

namespace testzip {

struct ZipFile {
    std::string name;
    std::string data;
    bool deflate = true;
};

inline void put16(std::string &s, std::uint16_t v)
{
    s += char(v & 0xff);
    s += char(v >> 8);
}

inline void put32(std::string &s, std::uint32_t v)
{
    put16(s, std::uint16_t(v & 0xffff));
    put16(s, std::uint16_t(v >> 16));
}

inline std::string rawDeflate(const std::string &in)
{
    z_stream z{};
    deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    std::string out(deflateBound(&z, uLong(in.size())), '\0');
    z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
    z.avail_in = uInt(in.size());
    z.next_out = reinterpret_cast<Bytef *>(out.data());
    z.avail_out = uInt(out.size());
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

// A zip as APPNOTE.TXT lays it out; `flags` goes into every header.
inline std::string makeZip(const std::vector<ZipFile> &files, std::uint16_t flags = 0)
{
    std::string zip, central;
    for (const ZipFile &f : files) {
        const std::string body = f.deflate ? rawDeflate(f.data) : f.data;
        const std::uint32_t crc =
            crc32(0, reinterpret_cast<const Bytef *>(f.data.data()), uInt(f.data.size()));
        const std::uint32_t offset = std::uint32_t(zip.size());
        auto header = [&](std::string &s, bool local) {
            put32(s, local ? 0x04034b50 : 0x02014b50);
            if (!local)
                put16(s, 20); // made by
            put16(s, 20);     // needed to extract
            put16(s, flags);
            put16(s, f.deflate ? 8 : 0);
            put32(s, 0); // time, date
            put32(s, crc);
            put32(s, std::uint32_t(body.size()));
            put32(s, std::uint32_t(f.data.size()));
            put16(s, std::uint16_t(f.name.size()));
            put16(s, 0); // extra
            if (!local) {
                put16(s, 0); // comment
                put16(s, 0); // disk
                put16(s, 0); // internal attributes
                put32(s, 0); // external attributes
                put32(s, offset);
            }
            s += f.name;
        };
        header(zip, true);
        zip += body;
        header(central, false);
    }
    const std::uint32_t cdOffset = std::uint32_t(zip.size());
    zip += central;
    put32(zip, 0x06054b50);
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, std::uint16_t(files.size()));
    put16(zip, std::uint16_t(files.size()));
    put32(zip, std::uint32_t(central.size()));
    put32(zip, cdOffset);
    put16(zip, 0); // comment
    return zip;
}

} // namespace testzip
