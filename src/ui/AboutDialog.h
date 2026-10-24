#pragma once

class QString;
class QWidget;
class EquirectViewportWindow;

// Help > About: version, license, credits, and a diagnostics block for bug
// reports -- GPU and driver, Vulkan video decode, NVENC, and whether tracking,
// the plate scan and the voice modes can run here -- with a button that copies
// it.
// `audioOutput` is the sound output's description (AudioPlayer).
void showAboutDialog(QWidget *parent, EquirectViewportWindow *viewport, const QString &audioOutput);
