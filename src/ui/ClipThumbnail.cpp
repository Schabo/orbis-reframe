#include "ClipThumbnail.h"

#include <ClipMetadata.h> // camera::ReadClipThumbnail
#include <Ycbcr.h>        // insv::YcbcrToRgbRows

#include <algorithm>
#include <cmath>

namespace {

uchar clamp8(float v) { return uchar(std::clamp(std::lround(v * 255.0f), 0L, 255L)); }

} // namespace

QImage clipThumbnail(const QString &path, const QSize &size)
{
    if (size.isEmpty())
        return {};
    const auto thumb = camera::ReadClipThumbnail(path.toStdString());
    if (!thumb)
        return {};
    const int w = thumb->width, h = thumb->height;

    // The middle of an equirectangular frame is straight ahead of the
    // camera's front lens. A 120-degree-wide window there, cut to the card's
    // shape, reads as a picture; the equirectangular bend at its edges is
    // small at this size. A flat view is a picture already: as much of it as
    // the card's shape takes.
    const double aspect = double(size.width()) / size.height();
    int cw = std::min(thumb->flat ? w : w / 3, int(h * aspect));
    int ch = std::min(h, int(cw / aspect));
    cw &= ~1;
    ch &= ~1;
    const int x0 = ((w - cw) / 2) & ~1;
    const int y0 = ((h - ch) / 2) & ~1;

    float m[3][4];
    insv::YcbcrToRgbRows(thumb->coding, 8, m);
    QImage crop(cw, ch, QImage::Format_RGB32);
    const uint8_t *yPlane = thumb->nv12.data();
    const uint8_t *uvPlane = yPlane + size_t(w) * h;
    for (int y = 0; y < ch; ++y) {
        const uint8_t *yRow = yPlane + size_t(y0 + y) * w + x0;
        const uint8_t *uvRow = uvPlane + size_t((y0 + y) / 2) * w + x0;
        auto *out = reinterpret_cast<QRgb *>(crop.scanLine(y));
        for (int x = 0; x < cw; ++x) {
            const float yy = yRow[x] / 255.0f;
            const float cb = uvRow[x & ~1] / 255.0f;
            const float cr = uvRow[(x & ~1) + 1] / 255.0f;
            const auto rgb = [&](int i) {
                return clamp8(m[i][0] * yy + m[i][1] * cb + m[i][2] * cr + m[i][3]);
            };
            out[x] = qRgb(rgb(0), rgb(1), rgb(2));
        }
    }
    return crop.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}
