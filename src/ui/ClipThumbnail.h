#pragma once

#include <QImage>
#include <QSize>
#include <QString>

// A small picture of a clip for cards and lists (the project library; later
// the media pool). For a 360 clip it is the camera's own preview from the
// file's trailer (camera::ReadClipThumbnail): an equirect cropped to the view
// straight ahead, or the flat view the camera chose (the Antigravity A1's) cut
// to the card's shape. No video is decoded and no GPU is used, so it is cheap
// and safe on a worker thread. A null image when there is none.
QImage clipThumbnail(const QString &path, const QSize &size);
