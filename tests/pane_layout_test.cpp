// PaneLayout.h: the split screen's pane rectangles (aligned for the GPU
// export, the gap, each pane's aspect), hit testing and Fill panes' angles.
// The exit code is the failure count.

#include "support/ModelTest.h"

#include "PaneLayout.h"

namespace {

bool aligned(const pane::Rect &r)
{
    return r.x % 4 == 0 && r.w % 4 == 0 && r.y % 2 == 0 && r.h % 2 == 0;
}

void testSingle()
{
    const std::vector<pane::Rect> r = pane::rects(pane::Split{}, 1920, 1080);
    CHECK(r.size() == 1 && r[0].x == 0 && r[0].y == 0 && r[0].w == 1920 && r[0].h == 1080);
    CHECK(pane::rects(pane::Split{}, 0, 1080).empty());
}

void testLayouts()
{
    for (const auto [w, h] : {std::pair{1920, 1080}, {3840, 2160}, {1080, 1920}, {1080, 1080},
                              {1280, 720}, {2704, 1520}}) {
        for (pane::Layout l : {pane::Layout::SideBySide, pane::Layout::Stacked, pane::Layout::Grid}) {
            for (float gap : {0.0f, pane::Split::kDefaultGap, 0.02f}) {
                pane::Split s;
                s.layout = l;
                s.gap = gap;
                const std::vector<pane::Rect> r = pane::rects(s, w, h);
                CHECK(static_cast<int>(r.size()) == pane::count(l));
                int area = 0;
                for (const pane::Rect &p : r) {
                    CHECK(aligned(p));
                    CHECK(p.x >= 0 && p.y >= 0 && p.x + p.w <= w && p.y + p.h <= h);
                    CHECK(p.w > 0 && p.h > 0);
                    area += p.w * p.h;
                }
                // The panes reach the frame's edges.
                CHECK(r.front().x == 0 && r.front().y == 0);
                CHECK(r.back().x + r.back().w == w && r.back().y + r.back().h == h);
                if (gap == 0.0f)
                    CHECK(area == w * h); // touching: they tile the frame
                else
                    CHECK(area < w * h);
                // Halves within one snap step of each other.
                CHECK(std::abs(r.front().w - r.back().w) <= 4);
                CHECK(std::abs(r.front().h - r.back().h) <= 2);
            }
        }
    }
    pane::Split side;
    side.layout = pane::Layout::SideBySide;
    side.gap = 0.0f;
    const std::vector<pane::Rect> r = pane::rects(side, 1920, 1080);
    CHECK(r[0].w == 960 && r[1].x == 960 && r[1].w == 960);
    CHECK(std::abs(r[0].aspect() - 960.0 / 1080.0) < 1e-12);
    // A default gap at 1080p: ~4 px, between the two.
    side.gap = pane::Split::kDefaultGap;
    const std::vector<pane::Rect> g = pane::rects(side, 1920, 1080);
    CHECK(g[1].x - g[0].w >= 4 && g[1].x - g[0].w <= 8);
}

void testHitAndFill()
{
    pane::Split grid;
    grid.layout = pane::Layout::Grid;
    grid.gap = 0.02f;
    const std::vector<pane::Rect> r = pane::rects(grid, 1920, 1080);
    CHECK(pane::at(r, 10, 10) == 0);
    CHECK(pane::at(r, 1900, 10) == 1);
    CHECK(pane::at(r, 10, 1070) == 2);
    CHECK(pane::at(r, 1900, 1070) == 3);
    CHECK(pane::at(r, 960, 540) == -1); // the gap
    CHECK(pane::at(r, -1, 10) == -1);

    // Grid: front, right, left, back -- opposite views diagonal.
    constexpr float pi = 3.14159265f;
    CHECK(pane::fillYawOffset(pane::Layout::Grid, 0) == 0.0f);
    CHECK(std::abs(pane::fillYawOffset(pane::Layout::Grid, 3) - pi) < 1e-6f);
    CHECK(std::abs(pane::fillYawOffset(pane::Layout::Grid, 1) + pane::fillYawOffset(pane::Layout::Grid, 2)) < 1e-6f);
    CHECK(std::abs(pane::fillYawOffset(pane::Layout::Stacked, 1) - pi) < 1e-6f);
    // 90 degrees across a square pane is 2 atan(sqrt 2) corner to corner;
    // a 16:9 one, as wide, has nearer corners.
    CHECK(std::abs(pane::fovFor(pi / 2, 1.0) - 2.0 * std::atan(std::sqrt(2.0))) < 1e-6);
    CHECK(pane::fovFor(pi / 2, 16.0 / 9.0) < pane::fovFor(pi / 2, 1.0));
    CHECK(pane::fovFor(pi / 2, 16.0 / 9.0) > pi / 2);
}

void testNames()
{
    for (pane::Layout l : {pane::Layout::Single, pane::Layout::SideBySide, pane::Layout::Stacked,
                           pane::Layout::Grid})
        CHECK(pane::layoutFromName(pane::layoutName(l)) == l);
    CHECK(pane::layoutFromName("hexagon") == pane::Layout::Single);
    CHECK(pane::layoutFromName(nullptr) == pane::Layout::Single);
}

} // namespace

int main()
{
    testSingle();
    testLayouts();
    testHitAndFill();
    testNames();
    return modeltest::finish("pane_layout_test");
}
