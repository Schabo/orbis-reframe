// Before any Qt header (EquirectViewportRenderer.h has the reason).
#include <vulkan/vulkan.h>

#include "EquirectViewportWindow.h"
#include "EquirectViewportRenderer.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QMutexLocker>
#include <QVulkanInstance>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <vector>

// The viewport window: the state the renderer draws (pose, frames, insets,
// marker, outlines) behind its locks, and the mouse and keys that change it.

namespace {

constexpr float kMaxPitch = 1.55334f;             // ~89 degrees
constexpr float kMouseSensitivity = 0.005f;        // radians per pixel dragged
constexpr double kWheelFovStep = 0.035;            // field of view change per wheel notch, log

} // namespace

QVulkanWindowRenderer *EquirectViewportWindow::createRenderer()
{
    return new EquirectViewportRenderer(this);
}

void EquirectViewportWindow::SetFrame(int width, int height, std::vector<uint8_t> rgba,
                                      const float stab[9], std::vector<RegionOutline> outlines,
                                      const FrameView &view)
{
    QMutexLocker lock(&m_frameMutex);
    m_pendingWidth = width;
    m_pendingHeight = height;
    m_pendingRgba = std::move(rgba);
    m_pendingLens.reset();
    m_pendingOutlines = std::move(outlines);
    m_framePending = true;
    m_pendingIsClear = false;
    ++m_framesHanded;
    m_pendingHasStab = stab != nullptr;
    if (stab)
        std::copy(stab, stab + 9, m_pendingStab);
    m_pendingHasOverlay = false;
    m_pendingView = view;
}

void EquirectViewportWindow::SetFrame(int width, int height, std::vector<uint8_t> rgba,
                                      const float stab[9], const FrameOverlay &overlay)
{
    SetFrame(width, height, std::move(rgba), stab);
    QMutexLocker lock(&m_frameMutex);
    m_pendingOverlay = overlay;
    m_pendingHasOverlay = true;
}

void EquirectViewportWindow::SetLensFrame(std::shared_ptr<const render::LensFrame> frame,
                                          const float stab[9], std::vector<RegionOutline> outlines,
                                          const FrameView &view)
{
    QMutexLocker lock(&m_frameMutex);
    m_pendingLens = std::move(frame); // a replaced pending frame goes straight back to its ring
    m_pendingRgba.clear();
    m_pendingOutlines = std::move(outlines);
    m_framePending = true;
    m_pendingIsClear = false;
    ++m_framesHanded;
    m_pendingHasStab = stab != nullptr;
    if (stab)
        std::copy(stab, stab + 9, m_pendingStab);
    m_pendingHasOverlay = false;
    m_pendingView = view;
}

void EquirectViewportWindow::ClearPicture()
{
    {
        // The renderer's placeholder (initResources). Not a frame: the
        // counters are about decoded pictures.
        QMutexLocker lock(&m_frameMutex);
        m_pendingWidth = 1;
        m_pendingHeight = 1;
        m_pendingRgba.assign(4, 128);
        m_pendingLens.reset();
        m_pendingOutlines.clear();
        m_framePending = true;
        m_pendingIsClear = true;
        m_pendingView = {};
        m_pendingHasStab = true;
        std::fill(m_pendingStab, m_pendingStab + 9, 0.0f);
        m_pendingStab[0] = m_pendingStab[4] = m_pendingStab[8] = 1.0f;
        m_pendingHasOverlay = false;
    }
    SetColorGrade(nullptr);
    SetInsets({});
    EndFrameOverlays();
    QMutexLocker lock(&m_paneMutex);
    m_split = pane::Split{};
}

void EquirectViewportWindow::EndFrameOverlays()
{
    {
        QMutexLocker lock(&m_frameMutex);
        m_pendingHasOverlay = false;
    }
    ClearMarker();
}

bool EquirectViewportWindow::TakePendingFrame(int &width, int &height, std::vector<uint8_t> &rgba,
                                              std::shared_ptr<const render::LensFrame> &lens)
{
    QMutexLocker lock(&m_frameMutex);
    if (!m_framePending) return false;
    width = m_pendingWidth;
    height = m_pendingHeight;
    rgba = std::move(m_pendingRgba);
    lens = std::move(m_pendingLens);
    m_outlines = std::move(m_pendingOutlines);
    m_pendingOutlines.clear();
    m_framePending = false;
    if (!m_pendingIsClear)
        ++m_framesTaken;
    if (m_pendingHasStab) // the renderer reads m_stab right after this, same thread
        std::copy(m_pendingStab, m_pendingStab + 9, m_stab);
    if (m_pendingView.hasPanes) // ... and the split, before the active pane's view
        SetPanes(m_pendingView.split, m_pendingView.panePoses);
    if (m_pendingView.hasPose) // ... and the view and grade
        SetPose(m_pendingView.pose);
    if (m_pendingView.hasGrade)
        SetColorGrade(&m_pendingView.grade);
    m_pendingView = {};
    if (m_pendingHasOverlay) { // ... and the pose / marker
        const FrameOverlay &o = m_pendingOverlay;
        SetPose(o.pose);
        if (o.hasMarker)
            SetMarker(o.marker, o.markerStyle);
        else
            ClearMarker();
        m_pendingHasOverlay = false;
    }
    { // ... and the insets that go with it (RenderInsets)
        QMutexLocker insetLock(&m_insetMutex);
        m_insetsHeld = false;
    }
    return true;
}

void EquirectViewportWindow::SetPose(const view::Pose &pose)
{
    m_pose.yaw = pose.yaw;
    m_pose.pitch = std::clamp(pose.pitch, -kMaxPitch, kMaxPitch);
    m_pose.roll = pose.roll;
    m_pose.distortion = pose.distortionIsAuto() ? view::kAutoDistortion
                                                : std::clamp(pose.distortion, 0.0f, 1.0f);
    m_pose.fov = std::clamp(pose.fov, view::kMinFov, view::maxFov(m_pose));
}

void EquirectViewportWindow::SetPanes(const pane::Split &split,
                                      const view::Pose poses[pane::kMaxPanes])
{
    QMutexLocker lock(&m_paneMutex);
    m_split = split;
    std::copy(poses, poses + pane::kMaxPanes, m_panePoses);
}

pane::Split EquirectViewportWindow::Split() const
{
    QMutexLocker lock(&m_paneMutex);
    return m_split;
}

int EquirectViewportWindow::EffectiveActivePane() const
{
    const int n = Split().count();
    const int active = m_activePane.load();
    return active < n ? active : 0;
}

void EquirectViewportWindow::SetActivePane(int pane)
{
    pane = std::clamp(pane, 0, pane::kMaxPanes - 1);
    if (pane == m_activePane.load())
        return;
    const int before = EffectiveActivePane();
    m_activePane.store(pane);
    const int after = EffectiveActivePane();
    if (after == before)
        return;
    // The pane left keeps the view it showed; the window takes the new one's.
    view::Pose to;
    {
        QMutexLocker lock(&m_paneMutex);
        m_panePoses[before] = m_pose;
        to = m_panePoses[after];
    }
    SetPose(to);
}

void EquirectViewportWindow::PanePoses(view::Pose out[pane::kMaxPanes]) const
{
    {
        QMutexLocker lock(&m_paneMutex);
        std::copy(m_panePoses, m_panePoses + pane::kMaxPanes, out);
    }
    out[EffectiveActivePane()] = m_pose;
}

void EquirectViewportWindow::SetOutputSize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    QMutexLocker lock(&m_paneMutex);
    m_outputW = width;
    m_outputH = height;
}

void EquirectViewportWindow::OutputSize(int &width, int &height) const
{
    QMutexLocker lock(&m_paneMutex);
    width = m_outputW;
    height = m_outputH;
}

std::vector<QRectF> EquirectViewportWindow::PaneFractions() const
{
    int w = 0, h = 0;
    OutputSize(w, h);
    std::vector<QRectF> out;
    for (const pane::Rect &r : pane::rects(Split(), w, h))
        out.emplace_back(double(r.x) / w, double(r.y) / h, double(r.w) / w, double(r.h) / h);
    if (out.empty())
        out.emplace_back(0.0, 0.0, 1.0, 1.0);
    return out;
}

float EquirectViewportWindow::ViewAspect() const
{
    int w = 0, h = 0;
    OutputSize(w, h);
    const std::vector<pane::Rect> r = pane::rects(Split(), w, h);
    const int active = EffectiveActivePane();
    if (r.size() > 1 && active < static_cast<int>(r.size()))
        return static_cast<float>(r[size_t(active)].aspect());
    const QRectF rect = outputRect();
    return rect.height() > 0 ? float(rect.width() / rect.height()) : 1.0f;
}

QRectF EquirectViewportWindow::paneRect(int i) const
{
    const QRectF out = outputRect();
    const std::vector<QRectF> f = PaneFractions();
    if (i < 0 || i >= static_cast<int>(f.size()))
        return out;
    const QRectF &p = f[size_t(i)];
    return QRectF(out.x() + p.x() * out.width(), out.y() + p.y() * out.height(),
                  p.width() * out.width(), p.height() * out.height());
}

int EquirectViewportWindow::paneAt(QPointF pos) const
{
    const int n = static_cast<int>(PaneFractions().size());
    for (int i = 0; i < n; ++i)
        if (paneRect(i).contains(pos))
            return i;
    return -1;
}

void EquirectViewportWindow::SetStabilization(const float rowMajor3x3[9])
{
    for (int i = 0; i < 9; ++i)
        m_stab[i] = rowMajor3x3[i];
}

void EquirectViewportWindow::StabRowMajor(float out[9]) const
{
    for (int i = 0; i < 9; ++i)
        out[i] = m_stab[i];
}

void EquirectViewportWindow::SetColorGrade(const color::Grade *grade)
{
    QMutexLocker lock(&m_gradeMutex);
    m_colorGrade = grade ? *grade : color::Grade{};
}

void EquirectViewportWindow::SetSurroundColor(const QColor &c)
{
    m_surround[0] = c.redF();
    m_surround[1] = c.greenF();
    m_surround[2] = c.blueF();
    requestUpdate();
}

void EquirectViewportWindow::SetMarkerColors(const QColor &select, const QColor &tracking,
                                             const QColor &lost)
{
    const QColor cs[3] = {select, tracking, lost};
    for (int i = 0; i < 3; ++i) {
        m_markerColors[i][0] = cs[i].redF();
        m_markerColors[i][1] = cs[i].greenF();
        m_markerColors[i][2] = cs[i].blueF();
    }
}

color::Grade EquirectViewportWindow::CurrentColorGrade() const
{
    QMutexLocker lock(&m_gradeMutex);
    return m_colorGrade;
}

void EquirectViewportWindow::SetFrameInsets(std::vector<InsetDraw> insets)
{
    // Under m_frameMutex throughout, so TakePendingFrame can't come in
    // between (it takes m_insetMutex inside m_frameMutex too).
    QMutexLocker frameLock(&m_frameMutex);
    {
        QMutexLocker lock(&m_insetMutex);
        m_insetsHeld = m_framePending; // until TakePendingFrame takes the picture
    }
    SetInsets(std::move(insets));
}

std::vector<EquirectViewportWindow::InsetDraw> EquirectViewportWindow::RenderInsets()
{
    std::vector<InsetDraw> now = Insets();
    QMutexLocker lock(&m_insetMutex);
    if (!m_insetsHeld)
        m_renderInsets = std::move(now);
    return m_renderInsets;
}

void EquirectViewportWindow::SetInsets(std::vector<InsetDraw> insets)
{
    QMutexLocker lock(&m_insetMutex);
    m_insets = std::move(insets);
    if (m_clearOverride) {
        // Released: drop the override once the dropped position has come
        // back from the model (or the clip is gone, or it never does).
        bool back = ++m_clearOverrideTicks > 60;
        bool present = false;
        for (const InsetDraw &d : m_insets)
            if (d.clipId == m_overrideClipId) {
                present = true;
                back |= std::abs(d.look.centerX - m_overrideCx) < 1e-4f &&
                        std::abs(d.look.centerY - m_overrideCy) < 1e-4f;
            }
        if (back || !present) {
            m_clearOverride = false;
            m_overrideClipId = 0;
        }
    }
}

std::vector<EquirectViewportWindow::InsetDraw> EquirectViewportWindow::Insets() const
{
    QMutexLocker lock(&m_insetMutex);
    std::vector<InsetDraw> out = m_insets;
    if (m_overrideClipId)
        for (InsetDraw &d : out)
            if (d.clipId == m_overrideClipId) {
                d.look.centerX = m_overrideCx;
                d.look.centerY = m_overrideCy;
            }
    return out;
}

QRectF EquirectViewportWindow::outputRect() const
{
    const double w = width(), h = height();
    const double winAspect = h > 0 ? w / h : 1.0;
    const double outAspect = m_outputAspect > 0.0f ? m_outputAspect : winAspect;
    double rw = w, rh = h;
    if (outAspect > winAspect)
        rh = w / outAspect;
    else
        rw = h * outAspect;
    return QRectF((w - rw) / 2.0, (h - rh) / 2.0, rw, rh);
}

bool EquirectViewportWindow::sphereDirAt(QPointF pos, float out[3]) const
{
    const int i = paneAt(pos);
    const QRectF rect = paneRect(i);
    if (i < 0 || rect.width() <= 0 || rect.height() <= 0)
        return false;
    view::Pose poses[pane::kMaxPanes];
    PanePoses(poses);
    sphereDir((pos.x() - rect.x()) / rect.width(), (pos.y() - rect.y()) / rect.height(),
              poses[i], float(rect.width() / rect.height()), out);
    return true;
}

void EquirectViewportWindow::SphereDirAtFraction(double fx, double fy, float out[3]) const
{
    sphereDir(fx, fy, m_pose, ViewAspect(), out);
}

void EquirectViewportWindow::sphereDir(double fx, double fy, const view::Pose &pose, float aspect,
                                       float out[3]) const
{
    // equirect.frag's ray: view ray from the NDC (ViewProjection.h), roll,
    // pitch, yaw, then the stabilization.
    const float ndcX = float(2.0 * fx - 1.0);
    const float ndcY = float(2.0 * fy - 1.0);
    const view::Lens lens = view::lens(pose, aspect);
    float v[3];
    view::ndcToRay(ndcX, ndcY, view::halfHeight(lens.fovY, lens.distortion), aspect,
                   lens.distortion, v);
    const float cr = std::cos(pose.roll), sr = std::sin(pose.roll);
    const float cp = std::cos(pose.pitch), sp = std::sin(pose.pitch);
    const float cy = std::cos(pose.yaw), sy = std::sin(pose.yaw);
    const float x1 = v[0] * cr - v[1] * sr, y1 = v[0] * sr + v[1] * cr, z1 = v[2];
    const float y2 = y1 * cp - z1 * sp, z2 = y1 * sp + z1 * cp;
    const float p[3] = {x1 * cy + z2 * sy, y2, -x1 * sy + z2 * cy};
    float m[9];
    StabRowMajor(m);
    float n = 0.0f;
    for (int i = 0; i < 3; ++i) {
        out[i] = m[i * 3] * p[0] + m[i * 3 + 1] * p[1] + m[i * 3 + 2] * p[2];
        n += out[i] * out[i];
    }
    n = std::sqrt(n);
    for (int i = 0; i < 3; ++i)
        out[i] /= n;
}

int EquirectViewportWindow::insetAt(QPointF pos) const
{
    const QRectF out = outputRect();
    const std::vector<InsetDraw> insets = Insets();
    const int n = std::min<int>(static_cast<int>(insets.size()), kMaxInsets);
    for (int i = n - 1; i >= 0; --i) { // topmost first
        const InsetDraw &d = insets[size_t(i)];
        if (d.look.opacity <= 0.0f || d.locked)
            continue;
        const inset::Rect r =
            inset::place(d.look, d.frame, d.aspect, int(std::lround(out.width())),
                         int(std::lround(out.height())));
        const double x = pos.x() - out.x(), y = pos.y() - out.y();
        if (!r.empty() && inset::shade(x, y, r).alpha > 0.5f)
            return d.clipId;
    }
    return 0;
}

void EquirectViewportWindow::SetBoxSelectMode(bool on)
{
    m_boxSelect = on;
    m_boxDragging = false;
    setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
}

void EquirectViewportWindow::SetMarker(const QRectF &fractions, MarkerStyle style)
{
    QMutexLocker lock(&m_insetMutex);
    m_marker = fractions;
    m_markerStyle = style;
    m_hasMarker = true;
}

void EquirectViewportWindow::ClearMarker()
{
    QMutexLocker lock(&m_insetMutex);
    m_hasMarker = false;
}

bool EquirectViewportWindow::Marker(float out[4], MarkerStyle *style) const
{
    QMutexLocker lock(&m_insetMutex);
    if (!m_hasMarker)
        return false;
    if (style)
        *style = m_markerStyle;
    out[0] = float(m_marker.x());
    out[1] = float(m_marker.y());
    out[2] = float(m_marker.width());
    out[3] = float(m_marker.height());
    return true;
}

void EquirectViewportWindow::mousePressEvent(QMouseEvent *event)
{
    if (m_boxSelect) {
        if (event->button() == Qt::LeftButton) {
            m_boxDragging = true;
            m_boxStart = event->position();
            ClearMarker();
        }
        return;
    }
    // A press on an inset picks it up to move it, instead of looking
    // around.
    if (event->button() == Qt::LeftButton) {
        if (const int clipId = insetAt(event->position())) {
            for (const InsetDraw &d : Insets())
                if (d.clipId == clipId) {
                    m_insetDragCx0 = d.look.centerX;
                    m_insetDragCy0 = d.look.centerY;
                }
            m_insetDragClipId = clipId;
            m_insetDragStart = event->position();
            m_insetDragMoved = false;
            emit insetPressed(clipId);
            return;
        }
    }
    // A press on another pane of a split picture makes it the one steered.
    if (Split().isSplit()) {
        const int i = paneAt(event->position());
        if (i >= 0 && i != EffectiveActivePane()) {
            SetActivePane(i);
            emit activePaneChanged(i);
        }
    }
    m_dragging = true;
    m_dragButton = event->button();
    m_lastMousePos = event->pos();
    m_pressYaw = m_pose.yaw;
    m_pressPitch = m_pose.pitch;
    emit draggingChanged(true);
}

void EquirectViewportWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_boxSelect) {
        if (m_boxDragging) {
            const QRectF out = paneRect(EffectiveActivePane());
            auto frac = [&out](QPointF p) {
                return QPointF(std::clamp((p.x() - out.x()) / std::max(1.0, out.width()), 0.0, 1.0),
                               std::clamp((p.y() - out.y()) / std::max(1.0, out.height()), 0.0, 1.0));
            };
            SetMarker(QRectF(frac(m_boxStart), frac(event->position())).normalized());
        }
        return;
    }
    if (m_insetDragClipId) {
        const QRectF out = outputRect();
        const QPointF d = event->position() - m_insetDragStart;
        if (!m_insetDragMoved && d.manhattanLength() < 3.0)
            return;
        m_insetDragMoved = true;
        inset::Look l;
        l.centerX = m_insetDragCx0 + float(d.x() / std::max(1.0, out.width()));
        l.centerY = m_insetDragCy0 + float(d.y() / std::max(1.0, out.height()));
        l = l.clamped();
        QMutexLocker lock(&m_insetMutex);
        m_overrideClipId = m_insetDragClipId;
        m_overrideCx = l.centerX;
        m_overrideCy = l.centerY;
        m_clearOverride = false;
        return;
    }
    if (!event->buttons()) { // hover: show that an inset can be moved
        setCursor(insetAt(event->position()) ? Qt::SizeAllCursor : Qt::ArrowCursor);
        return;
    }
    if (!m_dragging) return;
    const QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();

    m_pose.yaw -= delta.x() * kMouseSensitivity;
    m_pose.pitch = std::clamp(m_pose.pitch - delta.y() * kMouseSensitivity, -kMaxPitch, kMaxPitch);
}

void EquirectViewportWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_boxSelect) {
        if (!m_boxDragging || event->button() != Qt::LeftButton)
            return;
        m_boxDragging = false;
        float m[4];
        const QRectF out = paneRect(EffectiveActivePane());
        // At least a few pixels each way, or it was a click: cancel.
        if (Marker(m) && m[2] * out.width() >= 6.0 && m[3] * out.height() >= 6.0) {
            SetBoxSelectMode(false);
            emit boxSelected(QRectF(m[0], m[1], m[2], m[3]));
        } else {
            ClearMarker();
            SetBoxSelectMode(false);
            emit boxSelectCanceled();
        }
        return;
    }
    if (m_insetDragClipId) {
        const int clipId = m_insetDragClipId;
        m_insetDragClipId = 0;
        float cx = 0.0f, cy = 0.0f;
        {
            QMutexLocker lock(&m_insetMutex);
            cx = m_overrideCx;
            cy = m_overrideCy;
            if (m_insetDragMoved) {
                m_clearOverride = true; // until the model's position comes back
                m_clearOverrideTicks = 0;
            }
            else
                m_overrideClipId = 0;
        }
        if (m_insetDragMoved)
            emit insetMoved(clipId, cx, cy);
        return;
    }
    if (!m_dragging) return;
    m_dragging = false;
    emit draggingChanged(false);
    if (m_pose.yaw != m_pressYaw || m_pose.pitch != m_pressPitch) {
        emit lookCommitted();
        return;
    }
    // A right click that didn't look around: the picture's context menu.
    float dir[3];
    if (m_dragButton == Qt::RightButton && event->button() == Qt::RightButton &&
        sphereDirAt(event->position(), dir))
        emit pictureContextMenu(event->globalPosition().toPoint(), {dir[0], dir[1], dir[2]});
}

void EquirectViewportWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    // The second press has already arrived (and started a look drag, ended
    // by its release as usual).
    if (event->button() == Qt::LeftButton && !m_boxSelect && !insetAt(event->position()))
        emit doubleClicked();
}

void EquirectViewportWindow::keyPressEvent(QKeyEvent *event)
{
    if (m_boxSelect && event->key() == Qt::Key_Escape) {
        ClearMarker();
        SetBoxSelectMode(false);
        emit boxSelectCanceled();
        return;
    }
    QVulkanWindow::keyPressEvent(event);
}

void EquirectViewportWindow::wheelEvent(QWheelEvent *event)
{
    if (m_boxSelect)
        return;
    const float notches = event->angleDelta().y() / 120.0f;
    // In even ratios, so a notch changes the picture alike from a close-up
    // to a tiny planet.
    const double before = m_pose.fov;
    m_pose.fov = std::clamp(m_pose.fov * std::exp(-notches * kWheelFovStep), view::kMinFov,
                            view::maxFov(m_pose));
    if (m_pose.fov != before)
        emit lookCommitted();
}
