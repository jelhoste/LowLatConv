#include "nvg_soft.hpp"
using namespace nvgsoft;
int main(int argc, char** argv) {
    Renderer r; r.ssaa = 3.0f;
    NVGcontext* vg = createContext(r);
    const int font = nvgCreateFont(vg, "ui", argc > 1 ? argv[1] : "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
    if (font < 0) { std::printf("font load failed\n"); return 1; }
    const int W = 640, H = 360;
    nvgBeginFrame(vg, float(W), float(H), r.ssaa);
    nvgBeginPath(vg); nvgRect(vg, 0, 0, W, H); nvgFillColor(vg, nvgRGB(14, 12, 12)); nvgFill(vg);
    // card with a gradient border
    NVGpaint g = nvgLinearGradient(vg, 0, 0, 0, 200, nvgRGBA(40, 34, 32, 255), nvgRGBA(24, 20, 20, 255));
    nvgBeginPath(vg); nvgRoundedRect(vg, 20, 20, 280, 160, 14); nvgFillPaint(vg, g); nvgFill(vg);
    nvgBeginPath(vg); nvgRoundedRect(vg, 20.5f, 20.5f, 279, 159, 14); nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 30)); nvgStrokeWidth(vg, 1); nvgStroke(vg);
    // soft shadow (box gradient)
    NVGpaint sh = nvgBoxGradient(vg, 20, 214, 280, 90, 14, 24, nvgRGBA(0, 0, 0, 180), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg); nvgRect(vg, -20, 180, 360, 170); nvgRoundedRect(vg, 20, 200, 280, 90, 14); nvgPathWinding(vg, NVG_HOLE); nvgFillPaint(vg, sh); nvgFill(vg);
    nvgBeginPath(vg); nvgRoundedRect(vg, 20, 200, 280, 90, 14); nvgFillColor(vg, nvgRGB(34, 28, 27)); nvgFill(vg);
    // knob: ring + value arc + dot
    const float cx = 90, cy = 100, rad = 28;
    nvgBeginPath(vg); nvgArc(vg, cx, cy, rad, 0.75f * 3.14159f, 2.25f * 3.14159f, NVG_CW); nvgStrokeColor(vg, nvgRGB(60, 52, 50)); nvgStrokeWidth(vg, 5); nvgLineCap(vg, NVG_ROUND); nvgStroke(vg);
    nvgBeginPath(vg); nvgArc(vg, cx, cy, rad, 0.75f * 3.14159f, 1.55f * 3.14159f, NVG_CW); nvgStrokeColor(vg, nvgRGB(232, 128, 70)); nvgStroke(vg);
    NVGpaint rg = nvgRadialGradient(vg, cx, cy, 0, 20, nvgRGBA(46, 40, 38, 255), nvgRGBA(10, 8, 8, 255));
    nvgBeginPath(vg); nvgCircle(vg, cx, cy, 20); nvgFillPaint(vg, rg); nvgFill(vg);
    // text, with letter spacing and a clip
    nvgFontFaceId(vg, font); nvgFontSize(vg, 11); nvgTextLetterSpacing(vg, 1.5f); nvgFillColor(vg, nvgRGB(200, 190, 185)); nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgText(vg, cx, 150, "PRE-DELAY", nullptr);
    nvgTextLetterSpacing(vg, 0); nvgFontSize(vg, 16); nvgFillColor(vg, nvgRGB(255, 255, 255)); nvgText(vg, cx, 168, "0.0 dB", nullptr);
    nvgSave(vg); nvgScissor(vg, 200, 40, 70, 30); nvgFontSize(vg, 20); nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP); nvgFillColor(vg, nvgRGB(232, 128, 70)); nvgText(vg, 190, 44, "CLIPPED TEXT", nullptr); nvgRestore(vg);
    // waveform-like polyline + translucent fill
    nvgBeginPath(vg); nvgMoveTo(vg, 340, 120);
    for (int i = 0; i <= 260; ++i) { const float x = 340.0f + float(i), e = std::exp(-float(i) / 90.0f); nvgLineTo(vg, x, 120.0f - 60.0f * e * std::sin(float(i) * 0.9f)); }
    nvgStrokeColor(vg, nvgRGBA(240, 160, 110, 255)); nvgStrokeWidth(vg, 1.2f); nvgLineJoin(vg, NVG_ROUND); nvgStroke(vg);
    nvgBeginPath(vg); nvgMoveTo(vg, 340, 120); nvgLineTo(vg, 600, 120); nvgLineTo(vg, 600, 180); nvgLineTo(vg, 340, 180); nvgClosePath(vg); nvgFillColor(vg, nvgRGBA(232, 128, 70, 60)); nvgFill(vg);
    nvgGlobalAlpha(vg, 0.5f); nvgBeginPath(vg); nvgCircle(vg, 480, 300, 30); nvgFillColor(vg, nvgRGB(120, 200, 200)); nvgFill(vg); nvgBeginPath(vg); nvgCircle(vg, 500, 300, 30); nvgFillColor(vg, nvgRGB(220, 120, 160)); nvgFill(vg);
    nvgEndFrame(vg);
    writePNG("/tmp/nvg_soft_demo.png", r.downsample(), r.W, r.H);
    std::printf("ok %dx%d\n", r.W, r.H); nvgDeleteInternal(vg); return 0;
}
