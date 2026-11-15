// SPDX-License-Identifier: MIT OR Apache-2.0
#include <orbis-core/dji/DjiMeta.h>

#include <orbis-core/media/Mp4Box.h>
#include <orbis-core/Protobuf.h>

#include <algorithm>
#include <cmath>

namespace orbiscore {

const DjiSchema& DjiSchemaEagle4()
{
    static const DjiSchema s{"dvtm_eagle4_wa530.proto", 5};
    return s;
}

const DjiSchema& DjiSchemaOq101()
{
    static const DjiSchema s{"dvtm_oq101.proto", 6};
    return s;
}

const char* DjiSlotName(DjiSlot s)
{
    switch (s) {
    case DjiSlot::NativeRefineFar: return "native_refine_far";
    case DjiSlot::NativeRefine: return "native_refine";
    case DjiSlot::Native: break;
    }
    return "native";
}

namespace {

using Bytes = std::vector<uint8_t>;

double floatOr(const Bytes& m, uint32_t f, double fallback = 0.0)
{
    const auto v = ProtobufFloat(m, f);
    return v ? *v : fallback;
}

uint64_t varintOr(const Bytes& m, uint32_t f, uint64_t fallback = 0)
{
    const auto v = ProtobufVarint(m, f);
    return v ? *v : fallback;
}

std::string stringOr(const Bytes& m, uint32_t f)
{
    const auto v = ProtobufString(m, f);
    return v ? *v : std::string();
}

DjiDewarp parseDewarp(const Bytes& m)
{
    DjiDewarp d;
    d.fx = floatOr(m, 1);
    d.fy = floatOr(m, 2);
    d.cx = floatOr(m, 3);
    d.cy = floatOr(m, 4);
    d.k[0] = floatOr(m, 5);
    d.k[1] = floatOr(m, 6);
    d.k[2] = floatOr(m, 7);
    d.k[3] = floatOr(m, 8);
    d.k[4] = floatOr(m, 15);
    d.width = floatOr(m, 10);
    d.height = floatOr(m, 11);
    d.yaw = floatOr(m, 12);
    d.pitch = floatOr(m, 13);
    d.roll = floatOr(m, 14);
    // The tangential pair is field 20; field 27 (tangent_coeff) is zero on
    // the Avata 360 and not what the seam needs.
    const std::vector<float> p = ProtobufFloats(m, 20);
    if (p.size() >= 2) {
        d.p[0] = p[0];
        d.p[1] = p[1];
    }
    d.lensModel = floatOr(m, 24);
    if (const auto q = ProtobufBytes(m, 26)) {
        for (int k = 0; k < 4; ++k)
            d.imuQ[k] = floatOr(*q, static_cast<uint32_t>(k + 1));
        const double n = std::sqrt(d.imuQ[0] * d.imuQ[0] + d.imuQ[1] * d.imuQ[1] +
                                   d.imuQ[2] * d.imuQ[2] + d.imuQ[3] * d.imuQ[3]);
        d.hasImuQ = n > 0.5;
        if (d.hasImuQ)
            for (double& c : d.imuQ)
                c /= n;
        else
            d.imuQ[0] = 1, d.imuQ[1] = d.imuQ[2] = d.imuQ[3] = 0;
    }
    return d;
}

} // namespace

std::string DjiProductName(const std::vector<uint8_t>& sample)
{
    const auto header = ProtobufMessage(sample, {1, 1});
    return header ? stringOr(*header, 10) : std::string();
}

std::optional<DjiClipMeta> ParseDjiClipMeta(const std::vector<uint8_t>& sample,
                                            const DjiSchema& schema)
{
    const auto clip = ProtobufBytes(sample, 1);
    if (!clip)
        return std::nullopt;
    DjiClipMeta m;
    if (const auto h = ProtobufBytes(*clip, 1)) {
        m.protoFileName = stringOr(*h, 1);
        m.productSerial = stringOr(*h, 5);
        m.firmware = stringOr(*h, 6);
        m.clipTimestampUs = varintOr(*h, 9);
        m.productName = stringOr(*h, 10);
    }
    if (const auto v = ProtobufMessage(*clip, {8}))
        m.imuRateHz = static_cast<uint32_t>(varintOr(*v, 1));
    if (const auto v = ProtobufMessage(*clip, {9}))
        m.sensorFps = floatOr(*v, 1);
    if (const auto v = ProtobufMessage(*clip, {11}))
        m.lroNsPerLine = static_cast<uint32_t>(varintOr(*v, 1));
    if (const auto v = ProtobufMessage(*clip, {12})) {
        m.sensorWidth = static_cast<uint32_t>(varintOr(*v, 1));
        m.sensorHeight = static_cast<uint32_t>(varintOr(*v, 2));
    }

    const auto stream = ProtobufBytes(sample, 2);
    if (!stream)
        return m;
    if (const auto cam = ProtobufBytes(*stream, 2)) {
        if (const auto dev = ProtobufBytes(*cam, 1))
            m.deviceName = stringOr(*dev, 4);
        if (const auto sn = ProtobufBytes(*cam, 3))
            m.deviceSerial = stringOr(*sn, 1);
        if (const auto cm = ProtobufBytes(*cam, 4)) {
            // proto3 leaves a 0 (COLOR_MODE_DEFAULT) out: a present record
            // with no value is the default mode.
            m.hasColorMode = true;
            m.colorMode = static_cast<uint32_t>(varintOr(*cm, 1));
        }
    }
    if (const auto v = ProtobufBytes(*stream, 3)) {
        m.width = static_cast<uint32_t>(varintOr(*v, 1));
        m.height = static_cast<uint32_t>(varintOr(*v, 2));
        m.framerate = floatOr(*v, 3);
        m.bitDepth = static_cast<uint32_t>(varintOr(*v, 5));
    }
    if (const auto pano = ProtobufBytes(*stream, schema.panoField)) {
        // Slave / master field pairs, in the order a pair is taken.
        struct Pair {
            DjiSlot slot;
            uint32_t slave, master;
        };
        for (const Pair& pr : {Pair{DjiSlot::NativeRefineFar, 3, 4},
                               Pair{DjiSlot::NativeRefine, 1, 2}, Pair{DjiSlot::Native, 11, 12}}) {
            const auto s = ProtobufBytes(*pano, pr.slave);
            const auto ma = ProtobufBytes(*pano, pr.master);
            if (!s || !ma)
                continue;
            const DjiDewarp ds = parseDewarp(*s), dm = parseDewarp(*ma);
            if (!ds.filled() || !dm.filled())
                continue;
            m.calibrated = true;
            m.slot = pr.slot;
            m.slave = ds;
            m.master = dm;
            break;
        }
    }
    return m;
}

std::string DjiColorModeName(uint32_t mode)
{
    switch (mode) {
    case 0: return "Normal";
    case 1: return "D-Cinelike";
    case 2: return "D-Log";
    case 9: return "HLG";
    case 19: return "D-Log M";
    case 22: return "D-Log2";
    }
    return {};
}

std::string DjiLogProfile(uint32_t mode)
{
    switch (mode) {
    case 2: return "D-Log";
    case 19: return "D-Log M";
    case 22: return "D-Log2";
    }
    return {};
}

std::optional<DjiFrameMeta> ParseDjiFrameMeta(const std::vector<uint8_t>& sample)
{
    const auto frame = ProtobufBytes(sample, 3);
    if (!frame)
        return std::nullopt;
    DjiFrameMeta f;
    if (const auto h = ProtobufBytes(*frame, 1))
        f.timestampUs = varintOr(*h, 2);
    if (const auto cur = ProtobufMessage(*frame, {3, 2, 1})) {
        f.attitudeUs = varintOr(*cur, 1);
        WalkProtobuf(*cur, [&](uint32_t field, int wire, uint64_t, const Bytes& q) {
            if (field != 3 || wire != 2)
                return;
            f.attitude.push_back({static_cast<float>(floatOr(q, 1)), static_cast<float>(floatOr(q, 2)),
                                  static_cast<float>(floatOr(q, 3)), static_cast<float>(floatOr(q, 4))});
        });
    }
    const auto drone = ProtobufBytes(*frame, 4);
    if (!drone)
        return f;
    f.hasDrone = true;
    if (const auto v = ProtobufBytes(*drone, 2)) {
        f.velocityN = floatOr(*v, 1);
        f.velocityE = floatOr(*v, 2);
        f.velocityD = floatOr(*v, 3);
    }
    if (const auto a = ProtobufBytes(*drone, 3)) {
        // int32 in tenths of a degree; a negative one is a 10-byte varint.
        const auto deci = [&](uint32_t n) {
            return static_cast<double>(static_cast<int32_t>(ProtobufSigned(varintOr(*a, n)))) * 0.1;
        };
        f.pitchDeg = deci(1);
        f.rollDeg = deci(2);
        f.yawDeg = deci(3);
    }
    if (const auto gps = ProtobufBytes(*drone, 4)) {
        if (const auto pos = ProtobufBytes(*gps, 1)) {
            const auto lat = ProtobufDouble(*pos, 2), lon = ProtobufDouble(*pos, 3);
            // Degrees. The unit field (1) would say radians when left out,
            // as the Avata 360 leaves it, but its values are degrees
            // (docs/formats/dji-360.md §2.1).
            if (lat && lon && (*lat != 0.0 || *lon != 0.0)) {
                f.latitude = *lat;
                f.longitude = *lon;
                f.hasPosition = true;
            }
        }
        f.altitudeM =
            static_cast<double>(static_cast<int32_t>(ProtobufSigned(varintOr(*gps, 2)))) * 1e-3;
    }
    return f;
}

std::optional<std::string> ReadDjiHeldName(const std::string& file_path)
{
    const auto box = ReadMp4UserBox(file_path, "fsid", 1024);
    if (!box)
        return std::nullopt;
    std::string path(box->begin(), std::find(box->begin(), box->end(), uint8_t{0}));
    if (path.empty() ||
        !std::all_of(path.begin(), path.end(), [](char c) { return c >= ' ' && c < 0x7f; }))
        return std::nullopt;
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.empty())
        return std::nullopt;
    return name;
}

} // namespace orbiscore
