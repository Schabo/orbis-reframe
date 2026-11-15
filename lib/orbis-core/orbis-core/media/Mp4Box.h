// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// Reading boxes from any MP4 (ISO-BMFF) file's header, without FFmpeg: the
// camera formats keep some of their metadata there (Insta360's card path in
// `udta` / `free`, DJI's `fsid`).

namespace orbiscore {

// The first box of `type` (four characters) among those in [pos, end) of
// `file`: its payload's start and end. False when there is none, or on a
// read error or a malformed size.
bool FindMp4Box(std::ifstream& file, int64_t pos, int64_t end, const char* type,
                int64_t& payload, int64_t& payloadEnd);

// The payload of the MP4 header's user-data box `type` (`moov` / `udta` /
// `type`, four characters), up to `max_bytes` of it; std::nullopt when the
// file has none. Any MP4, not only an .insv (DJI's `fsid`).
std::optional<std::vector<uint8_t>> ReadMp4UserBox(const std::string& file_path, const char* type,
                                                   size_t max_bytes);

} // namespace orbiscore
