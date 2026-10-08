#include "nvg_soft.hpp"
using namespace nvgsoft;
// Renders the same labels at 1x (what an OpenGL NanoVG draws on a normal screen) and at 2x (HiDPI), for several sizes.
static void sheet(const char* out, float scale, const char* dir) {
    Renderer r; r.ssaa = scale; NVGcontext* vg = createContext(r);
    char p[512];
    std::snprintf(p, sizeof p, "%s/Inter-Regular.ttf", dir);  const int fr = nvgCreateFont(vg, "r", p);
    std::snprintf(p, sizeof p, "%s/Inter-SemiBold.ttf", dir); const int fs = nvgCreateFont(vg, "s", p);
    std::snprintf(p, sizeof p, "%s/Inter-Bold.ttf", dir);     const int fb = nvgCreateFont(vg, "b", p);
    if (fr < 0 || fs < 0 || fb < 0) { std::printf("font load failed %d %d %d\n", fr, fs, fb); return; }
    const int W = 420, H = 190;
    nvgBeginFrame(vg, W, H, scale);
    nvgBeginPath(vg); nvgRect(vg, 0, 0, W, H); nvgFillColor(vg, nvgRGB(22, 19, 18)); nvgFill(vg);
    const float sizes[] = {8, 9, 10, 11, 13};
    for (int i = 0; i < 5; ++i) {
        const float y = 16 + float(i) * 34;
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFontSize(vg, sizes[i]); nvgFontFaceId(vg, fb); nvgTextLetterSpacing(vg, 1.0f); nvgFillColor(vg, nvgRGB(190, 175, 168));
        nvgText(vg, 12, y, "PRE-DELAY  STRETCH  AUTO TRIM  ATTACK", nullptr);
        nvgTextLetterSpacing(vg, 0); nvgFontFaceId(vg, fs); nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgText(vg, 12, y + 14, "0.0 dB   -inf   2000 ms   48 kHz   Q 1.40   LATENCY 0 smp", nullptr);
        char lab[16]; std::snprintf(lab, sizeof lab, "%.0f px", sizes[i]); nvgFontFaceId(vg, fr); nvgFontSize(vg, 9); nvgFillColor(vg, nvgRGB(120, 108, 100)); nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE); nvgText(vg, W - 8, y, lab, nullptr);
    }
    nvgEndFrame(vg); writePNG(out, r.downsample(), r.W, r.H); nvgDeleteInternal(vg);
}
int main(int argc, char** argv) { const char* dir = argc > 1 ? argv[1] : "."; sheet("/tmp/font_1x.png", 1.0f, dir); sheet("/tmp/font_2x.png", 2.0f, dir); std::printf("ok\n"); return 0; }
