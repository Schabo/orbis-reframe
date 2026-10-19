// Output pixel -> view-space ray for the pose's projection (ReframeTrack.h's
// Projection). +Y up, -Z forward: the yaw/pitch/roll pose turns this ray, and
// stab follows it -- the pose math around the call is the same whatever the
// projection.
//
// Returns the direction in .xyz and a flag in .w: 0 where the projection
// doesn't reach (outside the mirror ball, past a fisheye's rim) -- shade
// that pixel black. `ndc` in [-1,1]; fovY is each view's zoom.

vec4 viewRayProj(vec2 ndc, float fovY, float aspect, int proj)
{
    const float VPI = 3.14159265358979;
    vec2 p = vec2(ndc.x * aspect, -ndc.y); // x in height units, y up
    float rho = length(p);
    vec2 az = rho > 1e-6 ? p / rho : vec2(0.0); // azimuth in the view

    if (proj == 1) {
        // Stereographic ("tiny planet"): the sphere projected from the pole
        // behind the view, so the view direction sits at the centre and the
        // sphere wraps around it -- point the view down for the planet. The
        // frame's height edge covers 2*fovY of polar angle, capped just
        // under the whole sphere: rho = tan(th/2) / tan(edge/2).
        float edge = min(2.0 * fovY, VPI - 0.02);
        float th = 2.0 * atan(rho * tan(edge * 0.5));
        float s = sin(th);
        return vec4(az.x * s, az.y * s, -cos(th), 1.0);
    }
    if (proj == 2) {
        // Equidistant fisheye: rho maps linearly to the polar angle, the
        // height edge covering fovY of it (2*fovY across the frame).
        float th = rho * fovY;
        float s = sin(th);
        return vec4(az.x * s, az.y * s, -cos(th), th <= VPI ? 1.0 : 0.0);
    }
    if (proj == 3) {
        // Mirror ball: a ball in the frame with the sphere reflected in it --
        // the centre shows what is behind the view, as a real ball mirror
        // does. The ball fills fovY/1.75 of the frame's height; outside it is
        // background.
        float br = min(fovY / 1.75, 1.0);
        vec2 u = p / br;
        float r2 = dot(u, u);
        if (r2 > 1.0)
            return vec4(0.0, 0.0, -1.0, 0.0);
        float nz = sqrt(1.0 - r2);
        return vec4(2.0 * u.x * nz, 2.0 * u.y * nz, 2.0 * nz * nz - 1.0, 1.0);
    }
    // Rectilinear: the plain pinhole ray.
    float t = tan(fovY * 0.5);
    return vec4(normalize(vec3(ndc.x * t * aspect, -ndc.y * t, -1.0)), 1.0);
}
