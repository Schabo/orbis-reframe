// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// A bounds-checked protobuf wire-format reader, for the cameras that write
// their metadata as protobuf messages (Insta360's trailer records, DJI's
// djmd track). It knows the wire format only: which field holds what is the
// caller's (each camera's format document).

namespace orbiscore {

// One varint at b[i], i moved past it; false when it runs off the end.
bool ReadVarint(const std::vector<uint8_t>& b, size_t& i, uint64_t& v);

// Flat protobuf walk: calls `visit(field, wire_type, varint, bytes)` for
// each field in order; `bytes` holds the payload of fixed64 / fixed32 /
// length-delimited fields. Stops at the first malformed field or a group
// (wire types 3 / 4) rather than misparse.
void WalkProtobuf(const std::vector<uint8_t>& b,
                  const std::function<void(uint32_t, int, uint64_t,
                                           const std::vector<uint8_t>&)>& visit);

// The first value of field `f` in the message `b`, read as its type. A field
// that isn't there, or is there with another wire type, is nullopt.
std::optional<std::vector<uint8_t>> ProtobufBytes(const std::vector<uint8_t>& b, uint32_t f);
std::optional<std::string> ProtobufString(const std::vector<uint8_t>& b, uint32_t f);
std::optional<uint64_t> ProtobufVarint(const std::vector<uint8_t>& b, uint32_t f);
std::optional<float> ProtobufFloat(const std::vector<uint8_t>& b, uint32_t f);   // fixed32
std::optional<double> ProtobufDouble(const std::vector<uint8_t>& b, uint32_t f); // fixed64
// A repeated float, packed (one length-delimited field) or not (one fixed32
// field per value); empty when absent.
std::vector<float> ProtobufFloats(const std::vector<uint8_t>& b, uint32_t f);

// The message at a path of nested fields (each the first of its number):
// ProtobufMessage(b, {2, 5, 4}) is field 4 of field 5 of field 2. nullopt
// when a step is missing.
std::optional<std::vector<uint8_t>> ProtobufMessage(const std::vector<uint8_t>& b,
                                                    std::initializer_list<uint32_t> path);

// A varint read as the signed int32 / int64 it encodes (two's complement).
inline int64_t ProtobufSigned(uint64_t v)
{
    return static_cast<int64_t>(v);
}

} // namespace orbiscore
