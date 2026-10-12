#include "ClipThumbnail.h"

#include <InsvTrailer.h>

#include <algorithm>

namespace {

uchar clamp8(int v) { return uchar(std::clamp(v, 0, 255)); }

} // namespace

QImage clipThumbnail(const QString &path, const QSize &size)
{
    if (size.isEmpty())
        return {};
    const auto thumb = insv::ReadInsvThumbnail(path.toStdString());
    if (!thumb)
        return {};
    const int w = thumb->width, h = thumb->height;

    // The middle of the equirectangular frame is straight ahead of the
    // camera's front lens. A 120-degree-wide window there, cut to the card's
    // shape, reads as a picture; the equirectangular bend at its edges is
    // small at this size.
    const double aspect = double(size.width()) / size.height();
    int cw = std::min(w / 3, int(h * aspect));
    int ch = std::min(h, int(cw / aspect));
    cw &= ~1;
    ch &= ~1;
    const int x0 = ((w - cw) / 2) & ~1;
    const int y0 = ((h - ch) / 2) & ~1;

    // NV12, BT.601 limited range (what the camera writes; checked against
    // FFmpeg's conversion of the same bytes).
    QImage crop(cw, ch, QImage::Format_RGB32);
    const uint8_t *yPlane = thumb->nv12.data();
    const uint8_t *uvPlane = yPlane + size_t(w) * h;
    for (int y = 0; y < ch; ++y) {
        const uint8_t *yRow = yPlane + size_t(y0 + y) * w + x0;
        const uint8_t *uvRow = uvPlane + size_t((y0 + y) / 2) * w + x0;
        auto *out = reinterpret_cast<QRgb *>(crop.scanLine(y));
        for (int x = 0; x < cw; ++x) {
            const int c = 298 * (yRow[x] - 16);
            const int d = uvRow[x & ~1] - 128;
            const int e = uvRow[(x & ~1) + 1] - 128;
            out[x] = qRgb(clamp8((c + 409 * e + 128) >> 8),
                          clamp8((c - 100 * d - 208 * e + 128) >> 8),
                          clamp8((c + 516 * d + 128) >> 8));
        }
    }
    return crop.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}
