#pragma once

// Reading files out of a .zip archive -- what camera makers put their LUTs in.
// Only what such downloads use: stored and deflated entries, one disk, no
// zip64, no encryption; anything else is refused with a reason. The whole
// archive is read into memory (LUT packs are a few MB). No Qt; zlib does the
// inflating.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zip {

struct Entry {
    std::string name;          // the path inside the archive, '/'-separated
    std::uint64_t size = 0;    // uncompressed
    bool isDirectory() const { return !name.empty() && name.back() == '/'; }
};

class Archive {
public:
    // The archive's list of entries, read from `bytes` (the file's contents).
    // nullptr with *error set (in words for the user) when it isn't a zip we
    // can read.
    static std::unique_ptr<Archive> fromBytes(std::string bytes, std::string *error = nullptr);
    // The same for the file at `path` (UTF-8).
    static std::unique_ptr<Archive> open(const std::string &path, std::string *error = nullptr);

    const std::vector<Entry> &entries() const { return m_entries; }

    // The contents of entries()[index], checked against its CRC. False with
    // *error set when it can't be read.
    bool read(std::size_t index, std::string *out, std::string *error = nullptr) const;

    // An entry larger than this is refused (a .cube of the largest size we
    // read, 256^3, is ~400 MB of text): a damaged or hostile size can't make
    // us allocate without bound.
    static constexpr std::uint64_t kMaxEntrySize = 512ull << 20;

private:
    struct Location {
        std::uint16_t method = 0;
        std::uint32_t crc = 0;
        std::uint64_t compressedSize = 0;
        std::uint64_t localHeader = 0;
    };

    std::string m_bytes;
    std::vector<Entry> m_entries;
    std::vector<Location> m_locations; // parallel to m_entries
};

} // namespace zip
