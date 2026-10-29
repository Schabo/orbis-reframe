// Telemetry: a clip's GPS values between the fixes, the gaps where there are
// none, and the heading through a stop; MotionTelemetry: the lean and
// G-forces between their samples, gaps and ends. Exit code is the failure
// count.

#include "support/ModelTest.h"

#include "MotionTelemetry.h"
#include "Telemetry.h"

namespace {

bool near(double a, double b, double eps = 1e-9)
{
    return std::abs(a - b) <= eps;
}

insv::GpsFix fix(int64_t ms, double speed, double heading = 0.0, double lat = 59.0,
                 double lon = 18.0, double alt = 10.0)
{
    return {ms, lat, lon, speed, heading, alt};
}

// Through each fix, a straight line where the fixes lie on one, and never
// past a fix's neighbours: a steady speed stays steady up to a stop, and
// the stop doesn't dip below zero.
void testCurve()
{
    const Telemetry line(insv::GpsTrack{{fix(0, 10), fix(1000, 12), fix(2000, 14), fix(3000, 16)}});
    CHECK(line.at(1000) && line.at(1000)->speedMps == 12.0);
    CHECK(line.at(500) && near(line.at(500)->speedMps, 11.0));
    CHECK(line.at(2750) && near(line.at(2750)->speedMps, 15.5));

    const Telemetry stop(insv::GpsTrack{
        {fix(0, 10), fix(1000, 10), fix(2000, 10), fix(3000, 0), fix(4000, 0), fix(5000, 0.5)}});
    double prev = 11.0;
    for (int ms = 0; ms <= 5000; ms += 10) {
        const auto v = stop.at(ms);
        CHECK(v && v->speedMps >= 0.0 && v->speedMps <= 10.0 + 1e-9);
        if (v && ms <= 2000)
            CHECK(near(v->speedMps, 10.0));
        if (v && ms >= 2000 && ms <= 3000) { // easing down, without a bounce
            CHECK(v->speedMps <= prev + 1e-12);
            prev = v->speedMps;
        }
        if (v && ms >= 3000 && ms <= 4000)
            CHECK(v->speedMps == 0.0);
    }
    // Position and altitude on the same kind of curve.
    const Telemetry moving(insv::GpsTrack{{fix(0, 5, 0, 59.0, 18.0, 10), fix(1000, 5, 0, 59.001, 18.002, 12),
                                           fix(2000, 5, 0, 59.002, 18.004, 14)}});
    const auto mid = moving.at(1500);
    CHECK(mid && near(mid->latitude, 59.0015) && near(mid->longitude, 18.003) &&
          near(mid->altitudeM, 13.0));
    CHECK(Telemetry().isEmpty() && !Telemetry().at(0));
}

// Fixes more than kMaxGapMs apart: no data between them, each side held for
// kHoldMs; the track's ends likewise. A repeated or out-of-order time is
// dropped.
void testGaps()
{
    const Telemetry t(insv::GpsTrack{{fix(0, 1), fix(1000, 2), fix(1000, 9), fix(500, 9),
                                      fix(5000, 3), fix(6000, 4), fix(9000, 5)}});
    CHECK(t.at(1000) && t.at(1000)->speedMps == 2.0);
    CHECK(Telemetry::kHoldMs == 1500);
    CHECK(t.at(-1500) && t.at(-1500)->speedMps == 1.0);
    CHECK(!t.at(-1501));
    CHECK(t.at(2500) && t.at(2500)->speedMps == 2.0);
    CHECK(!t.at(2501) && !t.at(3000) && !t.at(3499));
    CHECK(t.at(3500) && t.at(3500)->speedMps == 3.0);
    // 3000 ms apart is no gap yet.
    CHECK(t.at(7500) && t.at(7500)->speedMps > 4.0 && t.at(7500)->speedMps < 5.0);
    CHECK(t.at(10500) && t.at(10500)->speedMps == 5.0);
    CHECK(!t.at(10501));

    // One fix on its own.
    const Telemetry single(insv::GpsTrack{{fix(2000, 7)}});
    CHECK(single.at(3500) && single.at(3500)->speedMps == 7.0 && !single.at(3501));
}

// Angles the short way round: the heading through north, the longitude
// through 180 degrees.
void testWrap()
{
    const Telemetry north(insv::GpsTrack{{fix(0, 5, 350), fix(1000, 5, 10), fix(2000, 5, 30)}});
    const auto h = north.at(500);
    CHECK(h && h->headingDeg && (*h->headingDeg > 359.0 || *h->headingDeg < 1.0));
    CHECK(north.at(1000) && near(*north.at(1000)->headingDeg, 10.0));
    for (int ms = 0; ms <= 2000; ms += 50) {
        const auto v = north.at(ms);
        CHECK(v && v->headingDeg && *v->headingDeg >= 0.0 && *v->headingDeg < 360.0);
    }

    const Telemetry dateLine(insv::GpsTrack{{fix(0, 5, 90, -17.0, 179.9995), fix(1000, 5, 90, -17.0, -179.9995)}});
    const auto l = dateLine.at(500);
    CHECK(l && near(std::abs(l->longitude), 180.0, 1e-6));
    CHECK(dateLine.at(250) && near(dateLine.at(250)->longitude, 179.99975, 1e-6));
}

// The heading the camera doesn't know (NaN: standing still) is the last one
// it had, at the start the first; a stretch with none has none.
void testUnknownHeading()
{
    const double unknown = std::nan("");
    const Telemetry stop(insv::GpsTrack{{fix(0, 0, unknown), fix(1000, 3, 90), fix(2000, 0, unknown),
                                         fix(3000, 0, unknown), fix(4000, 4, 100)}});
    CHECK(stop.at(0) && near(*stop.at(0)->headingDeg, 90.0));
    CHECK(stop.at(2500) && near(*stop.at(2500)->headingDeg, 90.0));
    CHECK(stop.at(4000) && near(*stop.at(4000)->headingDeg, 100.0));

    const Telemetry parked(insv::GpsTrack{{fix(0, 0, unknown), fix(1000, 0, unknown),
                                           fix(9000, 2, 45)}});
    CHECK(parked.at(500) && !parked.at(500)->headingDeg);
    CHECK(parked.at(9000) && near(*parked.at(9000)->headingDeg, 45.0));
}

// Straight between the 10 ms samples; nothing where the IMU had none (a
// sample next to a gap is held for half a step), and the ends held for one
// step.
void testMotion()
{
    CHECK(MotionTelemetry().isEmpty() && !MotionTelemetry().at(0));
    const float none = std::nanf("");
    insv::MotionTrack m;
    m.leanDeg = {0, 10, 20, none, none, 50, 60};
    m.lateralG = {0, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    m.forwardG = {0, -0.1f, -0.2f, -0.3f, -0.4f, -0.5f, -0.6f};
    const MotionTelemetry t(m);
    CHECK(!t.isEmpty());
    CHECK(t.at(0) && t.at(0)->leanDeg == 0.0);
    const auto mid = t.at(15);
    CHECK(mid && near(mid->leanDeg, 15.0, 1e-4) && near(mid->lateralG, 0.15, 1e-6) &&
          near(mid->forwardG, -0.15, 1e-6));
    CHECK(t.at(24) && near(t.at(24)->leanDeg, 20.0, 1e-4)); // nearer the sample before the gap
    CHECK(!t.at(26) && !t.at(35) && !t.at(44));
    CHECK(t.at(46) && near(t.at(46)->leanDeg, 50.0, 1e-4)); // ... after it
    CHECK(t.at(-9) && t.at(-9)->leanDeg == 0.0 && !t.at(-11));
    CHECK(t.at(69) && near(t.at(69)->leanDeg, 60.0, 1e-4) && !t.at(71));
}

} // namespace

int main()
{
    testCurve();
    testGaps();
    testWrap();
    testUnknownHeading();
    testMotion();
    return modeltest::finish("telemetry_test");
}
