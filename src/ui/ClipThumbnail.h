#pragma once

#include <QImage>
#include <QSize>
#include <QString>

// A small picture of a clip for cards and lists (the project library; later
// the media pool). For an .insv it is the camera's own stitched thumbnail
// from the file's trailer (insv::ReadInsvThumbnail), cropped to the view
// straight ahead -- no video is decoded and no GPU is used, so it is cheap
// and safe on a worker thread. A null image when there is none.
QImage clipThumbnail(const QString &path, const QSize &size);
