#pragma once

#include <QGlyphRun>
#include <QList>
#include <QPointF>
#include <QString>

#include <functional>
#include <optional>
#include <utility>

class QPainter;

// Text in the overlay pictures (titles, gauges) drawn so that a picture
// comes out the same, byte for byte, whatever the process drew before it.
//
// Qt keeps one FreeType face per font file and thread. Rasterising a glyph
// for Qt's glyph cache (text up to 64 px) leaves that glyph's sub-pixel
// offset in the face's transform, and Qt's later outline loads on the face
// -- the text it fills as paths, QRawFont::pathForGlyph -- are moved by it.
// So text would depend on what else the thread drew before, at any size,
// the GUI thread's widgets included. Overlay pictures therefore draw their
// text only as outlines, never through the glyph cache, and only on one
// thread that draws nothing else: no glyph is rasterised there, so the
// offset stays zero.
namespace overlaytext {

// Runs `job` on the overlay text thread and waits for it to finish (at
// once, if called there). Once the application is shutting down, on the
// caller's thread.
void run(const std::function<void()> &job);

template <class F> auto onTextThread(F &&f)
{
    std::optional<decltype(f())> result;
    run([&] { result.emplace(f()); });
    return std::move(*result);
}

// The glyphs of `runs`, each placed at its position plus `offset`, filled in
// the painter's pen colour as QPainter would draw them as text. A colour
// font's glyphs (emoji) are pictures, drawn as QPainter draws them: that
// leaves an offset in the colour font's face, whose outlines are never read.
void fillGlyphRuns(QPainter &p, const QList<QGlyphRun> &runs, QPointF offset);

// `text` in the painter's font and pen with its baseline starting at
// `baseline`, as QPainter::drawText(QPointF, QString) draws it -- as outlines.
void drawText(QPainter &p, QPointF baseline, const QString &text);

} // namespace overlaytext
