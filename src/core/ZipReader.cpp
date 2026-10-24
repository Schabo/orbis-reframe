#include "ZipReader.h"

#include <zlib.h>

#include <fstream>
#include <iterator>

// Layout from PKWARE's APPNOTE.TXT (sections 4.3.6 - 4.3.16): the end of
// central directory record at the end of the file points at the central
// directory, one header per entry, each pointing at the entry's local header,
// which the data follows. Sizes and CRCs are taken from the central directory:
// a local header may leave them zero and put them in a data descriptor after
// the data (flag bit 3).

namespace zip {

namespace {

constexpr std::uint32_t kEndOfCentralDir = 0x06054b50;
constexpr std::uint32_t kCentralHeader = 0x02014b50;
constexpr std::uint32_t kLocalHeader = 0x04034b50;
constexpr std::size_t kEndOfCentralDirSize = 22;
constexpr std::size_t kCentralHeaderSize = 46;
constexpr std::size_t kLocalHeaderSize = 30;
constexpr std::uint16_t kStored = 0;
constexpr std::uint16_t kDeflated = 8;
constexpr std::uint16_t kEncrypted = 1; // general purpose flag bit 0

std::uint16_t u16(const std::string &b, std::size_t at)
{
    return std::uint16_t(std::uint8_t(b[at]) | std::uint8_t(b[at + 1]) << 8);
}

std::uint32_t u32(const std::string &b, std::size_t at)
{
    return std::uint32_t(u16(b, at)) | std::uint32_t(u16(b, at + 2)) << 16;
}

template <typename T>
T fail(std::string *error, const char *message, T result)
{
    if (error)
        *error = message;
    return result;
}

} // namespace

std::unique_ptr<Archive> Archive::open(const std::string &path, std::string *error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return fail(error, "the file can't be read", nullptr);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad())
        return fail(error, "the file can't be read", nullptr);
    return fromBytes(std::move(bytes), error);
}

std::unique_ptr<Archive> Archive::fromBytes(std::string bytes, std::string *error)
{
    const char *notZip = "it isn't a zip file, or it is damaged";
    if (bytes.size() < kEndOfCentralDirSize)
        return fail(error, notZip, nullptr);
    // The record sits at the very end, followed only by an archive comment of
    // at most 65535 bytes: search back from the end.
    const std::size_t lowest =
        bytes.size() > kEndOfCentralDirSize + 0xffff ? bytes.size() - kEndOfCentralDirSize - 0xffff : 0;
    std::size_t eocd = std::string::npos;
    for (std::size_t at = bytes.size() - kEndOfCentralDirSize + 1; at-- > lowest;) {
        if (u32(bytes, at) == kEndOfCentralDir) {
            eocd = at;
            break;
        }
    }
    if (eocd == std::string::npos)
        return fail(error, notZip, nullptr);
    const std::uint16_t thisDisk = u16(bytes, eocd + 4);
    const std::uint16_t cdDisk = u16(bytes, eocd + 6);
    const std::uint16_t count = u16(bytes, eocd + 10);
    const std::uint32_t cdSize = u32(bytes, eocd + 12);
    const std::uint32_t cdOffset = u32(bytes, eocd + 16);
    if (thisDisk != 0 || cdDisk != 0)
        return fail(error, "it is split over several files", nullptr);
    if (count == 0xffff || cdSize == 0xffffffff || cdOffset == 0xffffffff)
        return fail(error, "it is a zip64 archive, which isn't supported", nullptr);
    if (std::uint64_t(cdOffset) + cdSize > eocd)
        return fail(error, notZip, nullptr);

    auto archive = std::unique_ptr<Archive>(new Archive);
    std::size_t at = cdOffset;
    for (int i = 0; i < count; ++i) {
        if (at + kCentralHeaderSize > eocd || u32(bytes, at) != kCentralHeader)
            return fail(error, notZip, nullptr);
        const std::uint16_t flags = u16(bytes, at + 8);
        Location loc;
        loc.method = u16(bytes, at + 10);
        loc.crc = u32(bytes, at + 16);
        loc.compressedSize = u32(bytes, at + 20);
        const std::uint32_t size = u32(bytes, at + 24);
        const std::size_t nameLen = u16(bytes, at + 28);
        const std::size_t extraLen = u16(bytes, at + 30);
        const std::size_t commentLen = u16(bytes, at + 32);
        loc.localHeader = u32(bytes, at + 42);
        if (at + kCentralHeaderSize + nameLen > eocd)
            return fail(error, notZip, nullptr);
        Entry entry;
        entry.name = bytes.substr(at + kCentralHeaderSize, nameLen);
        entry.size = size;
        if (flags & kEncrypted)
            return fail(error, "it is password-protected", nullptr);
        archive->m_entries.push_back(std::move(entry));
        archive->m_locations.push_back(loc);
        at += kCentralHeaderSize + nameLen + extraLen + commentLen;
    }
    archive->m_bytes = std::move(bytes);
    return archive;
}

bool Archive::read(std::size_t index, std::string *out, std::string *error) const
{
    out->clear();
    const char *damaged = "the zip file is damaged";
    if (index >= m_entries.size())
        return fail(error, "no such file in the zip", false);
    const Entry &entry = m_entries[index];
    const Location &loc = m_locations[index];
    if (loc.method != kStored && loc.method != kDeflated)
        return fail(error, "it is compressed with a method that isn't supported", false);
    if (entry.size > kMaxEntrySize)
        return fail(error, "the file in the zip is too large", false);
    const std::uint64_t header = loc.localHeader;
    if (header + kLocalHeaderSize > m_bytes.size() || u32(m_bytes, header) != kLocalHeader)
        return fail(error, damaged, false);
    const std::uint64_t data =
        header + kLocalHeaderSize + u16(m_bytes, header + 26) + u16(m_bytes, header + 28);
    if (data + loc.compressedSize > m_bytes.size())
        return fail(error, damaged, false);
    const char *src = m_bytes.data() + data;

    if (loc.method == kStored) {
        if (loc.compressedSize != entry.size)
            return fail(error, damaged, false);
        out->assign(src, entry.size);
    } else {
        // Raw deflate (negative window bits: no zlib header or trailer).
        z_stream z{};
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
            return fail(error, "the zip file can't be unpacked", false);
        // Both sizes fit zlib's 32-bit counts (kMaxEntrySize; the
        // compressed size is a 32-bit field), so one call does it.
        out->resize(entry.size);
        z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(src));
        z.avail_in = uInt(loc.compressedSize);
        z.next_out = reinterpret_cast<Bytef *>(out->data());
        z.avail_out = uInt(entry.size);
        const int rc = inflate(&z, Z_FINISH);
        const bool whole = rc == Z_STREAM_END && z.total_out == entry.size;
        inflateEnd(&z);
        if (!whole) {
            out->clear();
            return fail(error, damaged, false);
        }
    }
    const uLong crc =
        crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const Bytef *>(out->data()), uInt(out->size()));
    if (crc != loc.crc) {
        out->clear();
        return fail(error, damaged, false);
    }
    return true;
}

} // namespace zip
