#pragma once

class QWidget;
class EquirectViewportWindow;

// Help > About: version, license, credits, and a diagnostics block for bug
// reports -- GPU and driver, Vulkan video decode, NVENC, and whether tracking,
// the plate scan and the voice modes can run here -- with a button that copies
// it.
void showAboutDialog(QWidget *parent, EquirectViewportWindow *viewport);
