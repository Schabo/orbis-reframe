// SPDX-License-Identifier: MIT OR Apache-2.0
#include "Protobuf.h"

#include <cstring>

namespace insv {

bool ReadVarint(const std::vector<uint8_t>& b, size_t& i, uint64_t& v)
{
    v = 0;
    for (int shift = 0; i < b.size() && shift < 64; shift += 7) {
        const uint8_t c = b[i++];
        v |= static_cast<uint64_t>(c & 0x7f) << shift;
        if (!(c & 0x80))
            return true;
    }
    return false;
}

void WalkProtobuf(const std::vector<uint8_t>& b,
                  const std::function<void(uint32_t, int, uint64_t,
                                           const std::vector<uint8_t>&)>& visit)
{
    size_t i = 0;
    while (i < b.size()) {
        uint64_t key, v = 0;
        if (!ReadVarint(b, i, key))
            return;
        const int wt = static_cast<int>(key & 7);
        size_t n = 0;
        if (wt == 0) {
            if (!ReadVarint(b, i, v))
                return;
        } else if (wt == 1) {
            n = 8;
        } else if (wt == 5) {
            n = 4;
        } else if (wt == 2) {
            if (!ReadVarint(b, i, v))
                return;
            if (v > b.size())
                return;
            n = static_cast<size_t>(v);
        } else {
            return; // groups / unknown: stop rather than misparse
        }
        if (n > b.size() - i)
            return;
        const std::vector<uint8_t> bytes(b.begin() + i, b.begin() + i + n);
        i += n;
        visit(static_cast<uint32_t>(key >> 3), wt, v, bytes);
    }
}

namespace {

// The first field `f` with wire type `wt`: its varint or its bytes.
bool find(const std::vector<uint8_t>& b, uint32_t f, int wt, uint64_t& v,
          std::vector<uint8_t>& bytes)
{
    bool found = false;
    WalkProtobuf(b, [&](uint32_t g, int gwt, uint64_t gv, const std::vector<uint8_t>& gb) {
        if (found || g != f || gwt != wt)
            return;
        found = true;
        v = gv;
        bytes = gb;
    });
    return found;
}

} // namespace

std::optional<std::vector<uint8_t>> ProtobufBytes(const std::vector<uint8_t>& b, uint32_t f)
{
    uint64_t v = 0;
    std::vector<uint8_t> bytes;
    if (!find(b, f, 2, v, bytes))
        return std::nullopt;
    return bytes;
}

std::optional<std::string> ProtobufString(const std::vector<uint8_t>& b, uint32_t f)
{
    const auto bytes = ProtobufBytes(b, f);
    if (!bytes)
        return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

std::optional<uint64_t> ProtobufVarint(const std::vector<uint8_t>& b, uint32_t f)
{
    uint64_t v = 0;
    std::vector<uint8_t> bytes;
    if (!find(b, f, 0, v, bytes))
        return std::nullopt;
    return v;
}

std::optional<float> ProtobufFloat(const std::vector<uint8_t>& b, uint32_t f)
{
    uint64_t v = 0;
    std::vector<uint8_t> bytes;
    if (!find(b, f, 5, v, bytes) || bytes.size() != 4)
        return std::nullopt;
    float x;
    std::memcpy(&x, bytes.data(), 4);
    return x;
}

std::optional<double> ProtobufDouble(const std::vector<uint8_t>& b, uint32_t f)
{
    uint64_t v = 0;
    std::vector<uint8_t> bytes;
    if (!find(b, f, 1, v, bytes) || bytes.size() != 8)
        return std::nullopt;
    double x;
    std::memcpy(&x, bytes.data(), 8);
    return x;
}

std::vector<float> ProtobufFloats(const std::vector<uint8_t>& b, uint32_t f)
{
    std::vector<float> out;
    WalkProtobuf(b, [&](uint32_t g, int wt, uint64_t, const std::vector<uint8_t>& gb) {
        if (g != f)
            return;
        if (wt == 5 && gb.size() == 4) {
            float x;
            std::memcpy(&x, gb.data(), 4);
            out.push_back(x);
        } else if (wt == 2) {
            for (size_t k = 0; k + 4 <= gb.size(); k += 4) {
                float x;
                std::memcpy(&x, gb.data() + k, 4);
                out.push_back(x);
            }
        }
    });
    return out;
}

std::optional<std::vector<uint8_t>> ProtobufMessage(const std::vector<uint8_t>& b,
                                                    std::initializer_list<uint32_t> path)
{
    std::vector<uint8_t> cur = b;
    for (const uint32_t f : path) {
        auto next = ProtobufBytes(cur, f);
        if (!next)
            return std::nullopt;
        cur = std::move(*next);
    }
    return cur;
}

} // namespace insv
