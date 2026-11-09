// GX command processor (gx.cpp) and video output (video.cpp).
#pragma once
#include <cstdint>

// PE interrupts (OS interrupt numbers registered by GXInit).
constexpr uint32_t kGxInterruptToken = 18;   // GXSetDrawSync token reached
constexpr uint32_t kGxInterruptFinish = 19;  // GXSetDrawDone reached

struct GxStats
{
    uint64_t bytes = 0;
    uint64_t draws = 0;
    uint64_t vertices = 0;
    uint64_t bpWrites = 0;
    uint64_t xfLoads = 0;
    uint64_t displayLists = 0;
    uint64_t xfbCopies = 0;
    uint64_t textureCopies = 0;
    uint64_t unknown = 0;
};

// PE interrupts raised by the stream since the last call (bit n = OS interrupt n).
uint32_t GxTakePendingInterrupts();
const GxStats& GxGetStats();
// True once after the stream went out of sync.
bool GxStreamLost();

// Video output: a window that shows the XFB the game asked VI to scan out.
void VideoInit();
void* VideoGetWindow();
// True once after the window changed size (the presenter resizes the swap chain).
bool VideoTakeResize();
// The image's aspect ratio (dse.ini aspecto: 4:3, 16:9, 21:9 or 32:9).
float VideoAspect();
// For 21:9 and 32:9: (16/9) / aspect, by which 3D views are widened (1 otherwise).
float VideoWideFactor();
// The image's rectangle in a w x h client area (aspect kept, centered, black bars).
void VideoImageRect(int w, int h, int& x, int& y, int& iw, int& ih);
void VideoSetFramebuffer(uint32_t xfb);
void VideoSetMode(uint32_t width, uint32_t height);
// Called on every retrace: converts the current XFB and shows it.
void VideoPresent();
