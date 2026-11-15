// SPDX-License-Identifier: MIT OR Apache-2.0
#include <orbis-core/media/Mp4Box.h>

#include <algorithm>
#include <cstring>

namespace orbiscore {

namespace {

// The ISO-BMFF box at `pos`, which must end by `end`: its type, its whole
// size (a 64-bit size and "to the end" resolved) and its header's length.
// False past `end`, on a read error or a malformed size.
bool ReadBoxHeader(std::ifstream& file, int64_t pos, int64_t end, char type[4], int64_t& size,
                   int64_t& header) {
    if (pos + 8 > end) return false;
    file.seekg(pos, std::ios::beg);
    uint8_t h[8];
    file.read(reinterpret_cast<char*>(h), 8);
    if (!file) return false;
    const uint32_t size32 = (static_cast<uint32_t>(h[0]) << 24) |
                             (static_cast<uint32_t>(h[1]) << 16) |
                             (static_cast<uint32_t>(h[2]) << 8) | h[3];
    std::memcpy(type, h + 4, 4);
    header = 8;
    if (size32 == 1) { // 64-bit extended size follows the 8-byte header
        uint8_t ext[8];
        file.read(reinterpret_cast<char*>(ext), 8);
        if (!file) return false;
        size = 0;
        for (uint8_t b : ext) size = (size << 8) | b;
        header = 16;
    } else if (size32 == 0) {
        size = end - pos; // box extends to the end
    } else {
        size = size32;
    }
    return size >= header && pos + size <= end;
}

} // namespace

bool FindMp4Box(std::ifstream& file, int64_t pos, int64_t end, const char* type,
                int64_t& payload, int64_t& payloadEnd) {
    char t[4];
    int64_t size = 0, header = 0;
    while (ReadBoxHeader(file, pos, end, t, size, header)) {
        if (std::memcmp(t, type, 4) == 0) {
            payload = pos + header;
            payloadEnd = pos + size;
            return true;
        }
        pos += size;
    }
    return false;
}

std::optional<std::vector<uint8_t>> ReadMp4UserBox(const std::string& file_path, const char* type,
                                                   size_t max_bytes) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return std::nullopt;
    file.seekg(0, std::ios::end);
    const int64_t file_size = static_cast<int64_t>(file.tellg());
    int64_t moov = 0, moov_end = 0, udta = 0, udta_end = 0, box = 0, box_end = 0;
    if (!FindMp4Box(file, 0, file_size, "moov", moov, moov_end) ||
        !FindMp4Box(file, moov, moov_end, "udta", udta, udta_end) ||
        !FindMp4Box(file, udta, udta_end, type, box, box_end))
        return std::nullopt;
    std::vector<uint8_t> bytes(static_cast<size_t>(std::min<int64_t>(box_end - box,
                                                                     static_cast<int64_t>(max_bytes))));
    file.seekg(box, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) return std::nullopt;
    return bytes;
}

} // namespace orbiscore
