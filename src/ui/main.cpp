// Before any Qt header: QVulkanInstance defines VK_NO_PROTOTYPES (see
// EquirectViewportWindow.cpp).
#include <vulkan/vulkan.h>

#include <QApplication>
#include <QSettings>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QTextDocument>
#include <QVersionNumber>
#include <QVulkanInstance>
#include <QWidget>

#include "MainWindow.h"
#include "SmokeTest.h"
#include "Theme.h"

#include "../core/DataPaths.h"
#include "../core/Log.h"
#include "../core/Version.h"
#include "../export/ReframeFlatten.h"

extern "C" {
#include <libavutil/log.h>
}

#ifdef INSV_HAVE_VOICE
#include <VoiceIsolator.h>
#endif

#include <clocale>
#include <cstdio>
#include <cstring>

namespace {

// Qt word-wraps a tooltip only when it is rich text; a long plain one is shown
// as one line, as wide as the screen. Turn every long plain tooltip into rich
// text as it is set, keeping its line breaks.
class ToolTipWrapper : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::ToolTipChange) {
            if (auto *w = qobject_cast<QWidget *>(watched)) {
                const QString tip = w->toolTip();
                if (tip.size() > 60 && !Qt::mightBeRichText(tip))
                    w->setToolTip(QStringLiteral("<p>%1</p>")
                                      .arg(tip.toHtmlEscaped().replace(QLatin1Char('\n'),
                                                                       QStringLiteral("<br>"))));
            }
        }
        return false;
    }
};

// FFmpeg's libraries write their own log to stderr, at a level meant for a
// command-line tool: how a container's atoms parsed, which decoder options
// were ignored. In the app that is noise about files the user can't change --
// a screen recording whose metadata atom only parses loosely gets a "UDTA
// parsing failed retrying raw" warning, twice, while the library page makes
// its thumbnail -- and what actually fails is reported in the window instead.
// Send it to the opt-in log, so ORBIS_VERBOSE=1 brings all of it back (with
// FFmpeg's own verbose level on top), and leave errors on stderr, where a bug
// report picks them up.
void routeFfmpegLogToOrbisLog()
{
    av_log_set_level(core::log::Verbose() ? AV_LOG_VERBOSE : AV_LOG_WARNING);
    av_log_set_callback([](void *context, int level, const char *fmt, va_list args) {
        if (level > av_log_get_level() || (level > AV_LOG_ERROR && !core::log::Verbose()))
            return;
        // The context prefix ("[mov,mp4,… @ 0x…]") says which demuxer, decoder
        // or filter spoke, which is the useful half of a verbose log.
        char line[1024];
        int prefix = 1;
        if (av_log_format_line2(context, level, fmt, args, line, sizeof(line), &prefix) < 0)
            return;
        if (level <= AV_LOG_ERROR)
            std::fprintf(stderr, "[ffmpeg] %s", line);
        else
            core::log::Info("[ffmpeg] %s", line);
    });
}

} // namespace

int main(int argc, char *argv[])
{
    // Answered before anything Qt or Vulkan starts, so they work without a
    // Wayland session or GPU (scripts, package checks, bug reports).
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("orbis-reframe %s\n", core::version::string());
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("Usage: orbis-reframe [clip.insv | project.orp]\n"
                        "\n"
                        "Video editor for 360-degree camera footage.\n"
                        "\n"
                        "  --version   print the version and exit\n"
                        "  -h, --help  print this help and exit\n");
            return 0;
        }
    }

    // Wayland only, no X11/XWayland fallback. Setting this
    // before QApplication is constructed means a missing/broken Wayland
    // plugin fails startup instead of silently falling back to xcb.
    qputenv("QT_QPA_PLATFORM", "wayland");

    // Qt 6.8's Wayland plugin (Debian 13) loads the Vulkan loader by its
    // unversioned name, libvulkan.so, which only libvulkan-dev installs:
    // without it Vulkan "isn't there". Name the runtime library instead;
    // respects an explicit override.
    if (qEnvironmentVariableIsEmpty("QT_VULKAN_LIB"))
        qputenv("QT_VULKAN_LIB", "libvulkan.so.1");

    QApplication app(argc, argv);
    // QApplication has applied the user's locale to the C library; a decimal
    // comma there makes strtod / printf in FFmpeg and the other libraries
    // misread and miswrite '.' numbers (an X5 clip's calibration, filter
    // options). Put numbers back to "C" before any thread starts; widgets
    // format through QLocale, which still follows the user's language.
    std::setlocale(LC_NUMERIC, "C");
#ifdef INSV_HAVE_VOICE
    // The voice noise modes' DeepFilterNet model ships with the app (installed
    // next to it, or the source tree in a dev build); ORBIS_DFN_MODEL_DIR wins.
    if (qEnvironmentVariableIsEmpty("ORBIS_DFN_MODEL_DIR"))
        insv::VoiceIsolator::setModelDirectory(core::paths::modelDir("deepfilternet3"));
#endif
    // Qt's Wayland text-input-v3 warns "Try to disable surface X with focusing
    // surface Y" whenever focus has already moved to another surface (e.g. a
    // file dialog closing) before it disables input on the old one -- it
    // disables it regardless, so the warning is noise. QT_LOGGING_RULES
    // still overrides it.
    QLoggingCategory::setFilterRules(QStringLiteral("qt.qpa.wayland.textinput.warning=false"));
    // Before anything opens a file: the encoder probe below is the first.
    routeFfmpegLogToOrbisLog();
    // Explicit rather than taken from the binary name: it names the cache
    // directory (~/.cache/orbis-reframe).
    QApplication::setApplicationName(QStringLiteral("orbis-reframe"));
    QApplication::setApplicationVersion(QString::fromUtf8(core::version::string()));

    // The theme: palette and stylesheet from one set of colour tokens
    // (Theme.h). No system theme under the target Wayland / Sway setup; the
    // user picks one under View ▸ Theme.
    theme::apply(app, theme::byId(QSettings(QStringLiteral("orbis-reframe"),
                                            QStringLiteral("orbis-reframe"))
                                      .value(QStringLiteral("ui/theme"))
                                      .toString()));
    app.installEventFilter(new ToolTipWrapper(&app));

    QVulkanInstance vulkanInstance;
    // 1.1: the viewport imports decoded lens frames from the decoder's device
    // (external memory, dedicated allocation -- core in 1.1).
    vulkanInstance.setApiVersion(QVersionNumber(1, 1));
    // QVulkanWindow on Wayland picks VK_COLOR_SPACE_PASS_THROUGH_EXT whenever
    // the surface lists it (so the driver doesn't create a second colour-
    // management surface), but never enables the extension that defines it --
    // invalid usage the validation layer flags. Enabling it makes that valid.
    if (vulkanInstance.supportedExtensions().contains("VK_EXT_swapchain_colorspace"))
        vulkanInstance.setExtensions({"VK_EXT_swapchain_colorspace"});
#ifndef NDEBUG
    vulkanInstance.setLayers({"VK_LAYER_KHRONOS_validation"});
    // QVulkanWindow keeps one render-done semaphore per frame in flight (2)
    // rather than per swapchain image (3 here), so the layer reports it being
    // re-signalled while a present may still hold it. That is inside Qt and
    // not fixable through its API; hide just that report (swapchain-related
    // VUID-vkQueueSubmit-pSignalSemaphores-00067) so real ones stay visible.
    vulkanInstance.installDebugOutputFilter(
        [](QVulkanInstance::DebugMessageSeverityFlags, QVulkanInstance::DebugMessageTypeFlags,
           const void *message) {
            const auto *data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT *>(message);
            return data->pMessageIdName
                && std::strcmp(data->pMessageIdName, "VUID-vkQueueSubmit-pSignalSemaphores-00067") == 0
                && data->pMessage && std::strstr(data->pMessage, "VkSwapchainKHR");
        });
#endif
    // No Vulkan, or Vulkan without a GPU: say so in a window, not just on a
    // terminal the user may not have.
    QString vulkanProblem;
    if (!vulkanInstance.create()) {
        vulkanProblem = QObject::tr("Vulkan couldn't be started (error %1).").arg(vulkanInstance.errorCode());
    } else {
        uint32_t devices = 0;
        vkEnumeratePhysicalDevices(vulkanInstance.vkInstance(), &devices, nullptr);
        if (devices == 0)
            vulkanProblem = QObject::tr("Vulkan found no graphics card.");
    }
    if (!vulkanProblem.isEmpty()) {
        const QString text =
            vulkanProblem + QLatin1Char(' ') +
            QObject::tr("orbis-reframe needs a graphics card with Vulkan support.\n\n"
                        "Install the card's Vulkan driver and restart the computer. With an "
                        "NVIDIA card, on Debian: sudo apt install nvidia-driver nvidia-vulkan-icd. "
                        "With AMD or Intel: sudo apt install mesa-vulkan-drivers. "
                        "\"vulkaninfo --summary\" (package vulkan-tools) should then list the "
                        "card.");
        std::fprintf(stderr, "%s\n", text.toLocal8Bit().constData());
        QMessageBox::critical(nullptr, QStringLiteral("orbis-reframe"), text);
        return 1;
    }

    // The export encoder probe (a throwaway encoder open per backend, a
    // Vulkan / VAAPI device each on AMD and Intel) on its own thread, so it
    // doesn't hold up the window; the startup warning waits for it.
    exportpipe::probeHwEncodersInBackground();

    MainWindow window(&vulkanInstance);
    window.resize(1280, 760);

    // A path on the command line: a *.orp file loads that project, anything
    // else is opened as a source clip. The open and the first stitch happen on
    // the playback worker thread, so the window shows immediately with a
    // "Buffering…" indicator until the first frame lands. Without one, the
    // window starts on the project library.
    if (argc > 1)
        window.openPath(QString::fromLocal8Bit(argv[1]));
    else
        window.showLibrary();

    window.show();
    // The GPU smoke test (tests/gpu/smoke_test.sh) drives the app from a
    // script; nothing of it exists without the variable.
    if (!qEnvironmentVariableIsEmpty("ORBIS_SMOKE_TEST"))
        new SmokeTest(&window, qEnvironmentVariable("ORBIS_SMOKE_TEST"), &app);
    return app.exec();
}
