// Software back-end for NanoVG (CPU rasteriser) used ONLY to check the UI drawing code without OpenGL.
// The real NanoVG core (nanovg.c, fontstash, stb_truetype) does the tessellation and text layout; this file
// replaces the GL renderer: it rasterises the paths / strokes / text triangles that NanoVG hands over, with the
// paint model of nanovg_gl (linear / radial / box gradients, scissor, premultiplied source-over blending).
// Anti-aliasing is obtained by rendering at `ssaa` x resolution and box-filtering down.
//
// Known differences with the OpenGL renderer: no edge fringe (AA comes from supersampling), hard scissor edges,
// strokes thinner than 1 px are not alpha-faded the way NanoVG does at 1x. Good enough to check geometry,
// colours, gradients, text placement and clipping; NOT a pixel-exact preview.
#pragma once
extern "C" {
#include "nanovg.h"
}
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace nvgsoft {

struct Tex { int w = 0, h = 0, type = 0, flags = 0; std::vector<uint8_t> d; };

struct Renderer {
    int W = 0, H = 0;           // logical size
    float ssaa = 3.0f;          // supersampling factor (device pixels per logical pixel)
    int PW = 0, PH = 0;
    std::vector<uint8_t> px;    // premultiplied RGBA, device resolution
    std::vector<Tex> tex;

    // ---- paint ---------------------------------------------------------------------------------------------
    struct Frag { float inv[6]; float ext[2], radius, feather; float inner[4], outer[4]; int image; float sc[6]; float scExt[2]; bool hasScissor; };

    static void premult(const NVGcolor& c, float* o) { o[0] = c.r * c.a; o[1] = c.g * c.a; o[2] = c.b * c.a; o[3] = c.a; }
    Frag makeFrag(const NVGpaint* p, const NVGscissor* s) const {
        Frag f; nvgTransformInverse(f.inv, p->xform);
        f.ext[0] = p->extent[0]; f.ext[1] = p->extent[1]; f.radius = p->radius; f.feather = p->feather;
        premult(p->innerColor, f.inner); premult(p->outerColor, f.outer); f.image = p->image;
        f.hasScissor = s->extent[0] >= -0.5f;
        if (f.hasScissor) { nvgTransformInverse(f.sc, s->xform); f.scExt[0] = s->extent[0]; f.scExt[1] = s->extent[1]; }
        return f;
    }
    static bool inScissor(const Frag& f, float x, float y) {
        if (!f.hasScissor) return true;
        const float sx = f.sc[0] * x + f.sc[2] * y + f.sc[4], sy = f.sc[1] * x + f.sc[3] * y + f.sc[5];
        return std::fabs(sx) <= f.scExt[0] && std::fabs(sy) <= f.scExt[1];
    }
    static float sdroundrect(float px, float py, float ex, float ey, float r) {
        const float dx = std::fabs(px) - (ex - r), dy = std::fabs(py) - (ey - r);
        return std::min(std::max(dx, dy), 0.0f) + std::sqrt(std::max(dx, 0.0f) * std::max(dx, 0.0f) + std::max(dy, 0.0f) * std::max(dy, 0.0f)) - r;
    }
    static void shadeGrad(const Frag& f, float x, float y, float* out) {
        const float ptx = f.inv[0] * x + f.inv[2] * y + f.inv[4], pty = f.inv[1] * x + f.inv[3] * y + f.inv[5];
        const float d = std::min(1.0f, std::max(0.0f, (sdroundrect(ptx, pty, f.ext[0], f.ext[1], f.radius) + f.feather * 0.5f) / f.feather));
        for (int i = 0; i < 4; ++i) out[i] = f.inner[i] + (f.outer[i] - f.inner[i]) * d;
    }
    void blend(int ix, int iy, const float* c) {                      // c = premultiplied source colour
        if (ix < 0 || iy < 0 || ix >= PW || iy >= PH) return;
        uint8_t* p = &px[(size_t(iy) * size_t(PW) + size_t(ix)) * 4];
        const float ia = 1.0f - c[3];
        for (int i = 0; i < 4; ++i) { const float v = c[i] * 255.0f + p[i] * ia; p[i] = uint8_t(std::min(255.0f, v + 0.5f)); }
    }

    // ---- scan conversion (non-zero winding, sampled at device pixel centres) -----------------------------------
    struct Poly { std::vector<std::pair<float, float>> v; };
    template <class Span>
    void scan(const std::vector<Poly>& polys, Span span) {
        float y0 = 1e30f, y1 = -1e30f;
        for (auto& p : polys) for (auto& q : p.v) { y0 = std::min(y0, q.second); y1 = std::max(y1, q.second); }
        if (y0 > y1) return;
        const int ya = std::max(0, int(std::floor(y0 - 0.5f))), yb = std::min(PH - 1, int(std::ceil(y1 + 0.5f)));
        if (yb < ya) return;
        std::vector<std::vector<std::pair<float, int>>> rows(size_t(yb - ya + 1));
        for (auto& p : polys) {
            const size_t n = p.v.size(); if (n < 3) continue;
            for (size_t i = 0; i < n; ++i) {
                float ax = p.v[i].first, ay = p.v[i].second, bx = p.v[(i + 1) % n].first, by = p.v[(i + 1) % n].second;
                if (ay == by) continue;
                const int dir = by > ay ? 1 : -1; if (ay > by) { std::swap(ax, bx); std::swap(ay, by); }
                const int ra = std::max(ya, int(std::ceil(ay - 0.5f))), rb = std::min(yb, int(std::ceil(by - 0.5f)) - 1);
                for (int r = ra; r <= rb; ++r) { const float yc = float(r) + 0.5f; rows[size_t(r - ya)].push_back({ax + (yc - ay) * (bx - ax) / (by - ay), dir}); }
            }
        }
        for (int r = ya; r <= yb; ++r) {
            auto& cr = rows[size_t(r - ya)]; if (cr.size() < 2) continue;
            std::sort(cr.begin(), cr.end());
            int wnd = 0;
            for (size_t i = 0; i + 1 < cr.size(); ++i) {
                wnd += cr[i].second;
                if (wnd != 0) { const int xa = std::max(0, int(std::ceil(cr[i].first - 0.5f))), xb = std::min(PW, int(std::ceil(cr[i + 1].first - 0.5f))); for (int x = xa; x < xb; ++x) span(x, r); }
            }
        }
    }
    std::vector<Poly> toDevice(const NVGvertex* v, int n) const { Poly p; for (int i = 0; i < n; ++i) p.v.push_back({v[i].x * ssaa, v[i].y * ssaa}); return {p}; }

    void shadePixel(const Frag& f, int ix, int iy) {
        const float lx = (float(ix) + 0.5f) / ssaa, ly = (float(iy) + 0.5f) / ssaa;
        if (!inScissor(f, lx, ly)) return;
        float c[4]; shadeGrad(f, lx, ly, c); blend(ix, iy, c);
    }

    // ---- NanoVG callbacks ---------------------------------------------------------------------------------------
    void fill(NVGpaint* paint, NVGscissor* sc, const NVGpath* paths, int np) {
        const Frag f = makeFrag(paint, sc); std::vector<Poly> polys;
        for (int i = 0; i < np; ++i) { Poly p; for (int j = 0; j < paths[i].nfill; ++j) p.v.push_back({paths[i].fill[j].x * ssaa, paths[i].fill[j].y * ssaa}); polys.push_back(std::move(p)); }
        scan(polys, [&](int x, int y) { shadePixel(f, x, y); });
    }
    void stroke(NVGpaint* paint, NVGscissor* sc, const NVGpath* paths, int np) {
        const Frag f = makeFrag(paint, sc);
        std::vector<uint8_t> mask(size_t(PW) * size_t(PH), 0); bool any = false;
        for (int i = 0; i < np; ++i) {
            const NVGvertex* v = paths[i].stroke; const int n = paths[i].nstroke;
            for (int j = 0; j + 2 < n; ++j) {
                Poly t; const int a = j, b = (j & 1) ? j + 2 : j + 1, c = (j & 1) ? j + 1 : j + 2;
                t.v = {{v[a].x * ssaa, v[a].y * ssaa}, {v[b].x * ssaa, v[b].y * ssaa}, {v[c].x * ssaa, v[c].y * ssaa}};
                scan({t}, [&](int x, int y) { mask[size_t(y) * size_t(PW) + size_t(x)] = 1; any = true; });
            }
        }
        if (!any) return;
        for (int y = 0; y < PH; ++y) for (int x = 0; x < PW; ++x) if (mask[size_t(y) * size_t(PW) + size_t(x)]) shadePixel(f, x, y);
    }
    float sampleAlpha(const Tex& t, float u, float v) const {
        const float fx = u * float(t.w) - 0.5f, fy = v * float(t.h) - 0.5f;
        const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy)); const float ax = fx - float(x0), ay = fy - float(y0);
        auto at = [&](int x, int y) { x = std::min(t.w - 1, std::max(0, x)); y = std::min(t.h - 1, std::max(0, y)); return t.d[size_t(y) * size_t(t.w) + size_t(x)] / 255.0f; };
        return (at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax) * (1 - ay) + (at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax) * ay;
    }
    void triangles(NVGpaint* paint, NVGscissor* sc, const NVGvertex* v, int n) {
        const Frag f = makeFrag(paint, sc);
        if (f.image <= 0 || size_t(f.image) > tex.size()) return;
        const Tex& t = tex[size_t(f.image - 1)];
        for (int i = 0; i + 2 < n; i += 3) {
            float X[3], Y[3]; for (int k = 0; k < 3; ++k) { X[k] = v[i + k].x * ssaa; Y[k] = v[i + k].y * ssaa; }
            const float den = (Y[1] - Y[2]) * (X[0] - X[2]) + (X[2] - X[1]) * (Y[0] - Y[2]); if (std::fabs(den) < 1e-9f) continue;
            const int xa = std::max(0, int(std::floor(std::min({X[0], X[1], X[2]})))), xb = std::min(PW - 1, int(std::ceil(std::max({X[0], X[1], X[2]}))));
            const int ya = std::max(0, int(std::floor(std::min({Y[0], Y[1], Y[2]})))), yb = std::min(PH - 1, int(std::ceil(std::max({Y[0], Y[1], Y[2]}))));
            for (int y = ya; y <= yb; ++y) for (int x = xa; x <= xb; ++x) {
                const float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
                const float l0 = ((Y[1] - Y[2]) * (cx - X[2]) + (X[2] - X[1]) * (cy - Y[2])) / den, l1 = ((Y[2] - Y[0]) * (cx - X[2]) + (X[0] - X[2]) * (cy - Y[2])) / den, l2 = 1 - l0 - l1;
                if (l0 < 0 || l1 < 0 || l2 < 0) continue;
                if (!inScissor(f, cx / ssaa, cy / ssaa)) continue;
                const float u = l0 * v[i].u + l1 * v[i + 1].u + l2 * v[i + 2].u, w = l0 * v[i].v + l1 * v[i + 1].v + l2 * v[i + 2].v;
                const float a = t.type == 0 ? sampleAlpha(t, u, w) : 1.0f;
                const float c[4] = {f.inner[0] * a, f.inner[1] * a, f.inner[2] * a, f.inner[3] * a}; blend(x, y, c);
            }
        }
    }

    // ---- output -------------------------------------------------------------------------------------------------
    std::vector<uint8_t> downsample() const {           // -> RGB over black (the scene paints its own opaque background)
        const int s = int(ssaa + 0.5f); std::vector<uint8_t> out(size_t(W) * size_t(H) * 3);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            float acc[3] = {0, 0, 0};
            for (int j = 0; j < s; ++j) for (int i = 0; i < s; ++i) { const uint8_t* p = &px[(size_t(y * s + j) * size_t(PW) + size_t(x * s + i)) * 4]; for (int k = 0; k < 3; ++k) acc[k] += p[k]; }
            for (int k = 0; k < 3; ++k) out[(size_t(y) * size_t(W) + size_t(x)) * 3 + size_t(k)] = uint8_t(std::min(255.0f, acc[k] / float(s * s) + 0.5f));
        }
        return out;
    }
};

// ---- C callbacks ---------------------------------------------------------------------------------------------------
inline Renderer& R(void* u) { return *static_cast<Renderer*>(u); }
inline int cbCreate(void*, void*) { return 1; }
inline int cbCreateTexture(void* u, int type, int w, int h, int flags, const unsigned char* data) {
    Tex t; t.w = w; t.h = h; t.type = type == NVG_TEXTURE_ALPHA ? 0 : 1; t.flags = flags;
    t.d.assign(size_t(w) * size_t(h) * (t.type == 0 ? 1 : 4), 0); if (data) std::memcpy(t.d.data(), data, t.d.size());
    R(u).tex.push_back(std::move(t)); return int(R(u).tex.size());
}
inline int cbDeleteTexture(void*, int) { return 1; }
inline int cbUpdateTexture(void* u, int id, int x, int y, int w, int h, const unsigned char* data) {
    Tex& t = R(u).tex[size_t(id - 1)]; const int bpp = t.type == 0 ? 1 : 4;
    for (int r = 0; r < h; ++r) std::memcpy(&t.d[(size_t(y + r) * size_t(t.w) + size_t(x)) * size_t(bpp)], data + (size_t(y + r) * size_t(t.w) + size_t(x)) * size_t(bpp), size_t(w) * size_t(bpp));
    return 1;
}
inline int cbGetSize(void* u, int id, int* w, int* h) { const Tex& t = R(u).tex[size_t(id - 1)]; *w = t.w; *h = t.h; return 1; }
inline void cbViewport(void* u, float w, float h, float) { Renderer& r = R(u); r.W = int(w); r.H = int(h); r.PW = int(w * r.ssaa); r.PH = int(h * r.ssaa); if (r.px.size() != size_t(r.PW) * size_t(r.PH) * 4) r.px.assign(size_t(r.PW) * size_t(r.PH) * 4, 0); }
inline void cbNoop(void*) {}
inline void cbFill(void* u, NVGpaint* p, NVGcompositeOperationState, NVGscissor* s, float, const float*, const NVGpath* paths, int n) { R(u).fill(p, s, paths, n); }
inline void cbStroke(void* u, NVGpaint* p, NVGcompositeOperationState, NVGscissor* s, float, float, const NVGpath* paths, int n) { R(u).stroke(p, s, paths, n); }
inline void cbTriangles(void* u, NVGpaint* p, NVGcompositeOperationState, NVGscissor* s, const NVGvertex* v, int n, float) { R(u).triangles(p, s, v, n); }

// Creates a NanoVG context drawing into `r` (call nvgBeginFrame(ctx, w, h, ssaa) afterwards).
inline NVGcontext* createContext(Renderer& r) {
    static NVGparams params; std::memset(&params, 0, sizeof params);
    params.userPtr = &r; params.edgeAntiAlias = 0;
    params.renderCreate = cbCreate; params.renderCreateTexture = cbCreateTexture; params.renderDeleteTexture = cbDeleteTexture;
    params.renderUpdateTexture = cbUpdateTexture; params.renderGetTextureSize = cbGetSize; params.renderViewport = cbViewport;
    params.renderCancel = cbNoop; params.renderFlush = cbNoop; params.renderFill = cbFill; params.renderStroke = cbStroke;
    params.renderTriangles = cbTriangles; params.renderDelete = cbNoop;
    return nvgCreateInternal(&params, nullptr);
}

// ---- minimal PNG writer (stored deflate blocks) -------------------------------------------------------------------
inline void writePNG(const char* path, const std::vector<uint8_t>& rgb, int w, int h) {
    auto crcTab = [] { static uint32_t t[256]; for (uint32_t n = 0; n < 256; ++n) { uint32_t c = n; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; t[n] = c; } return t; }();
    auto crc = [&](const uint8_t* d, size_t n, uint32_t c = 0xffffffffu) { for (size_t i = 0; i < n; ++i) c = crcTab[(c ^ d[i]) & 255] ^ (c >> 8); return c; };
    std::vector<uint8_t> raw; raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
    for (int y = 0; y < h; ++y) { raw.push_back(0); raw.insert(raw.end(), rgb.begin() + long(y) * w * 3, rgb.begin() + long(y + 1) * w * 3); }
    std::vector<uint8_t> z = {0x78, 0x01}; uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw.size();) { const size_t n = std::min<size_t>(65535, raw.size() - i); z.push_back(i + n == raw.size()); z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8)); z.push_back(uint8_t(~n)); z.push_back(uint8_t((~n) >> 8)); z.insert(z.end(), raw.begin() + long(i), raw.begin() + long(i + n)); i += n; }
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; } const uint32_t ad = (b << 16) | a; for (int s = 24; s >= 0; s -= 8) z.push_back(uint8_t(ad >> s));
    FILE* f = std::fopen(path, "wb"); if (!f) return; const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10}; std::fwrite(sig, 1, 8, f);
    auto chunk = [&](const char* type, const std::vector<uint8_t>& d) { uint8_t len[4] = {uint8_t(d.size() >> 24), uint8_t(d.size() >> 16), uint8_t(d.size() >> 8), uint8_t(d.size())}; std::fwrite(len, 1, 4, f);
        std::vector<uint8_t> td(type, type + 4); td.insert(td.end(), d.begin(), d.end()); std::fwrite(td.data(), 1, td.size(), f); const uint32_t c = ~crc(td.data(), td.size()); uint8_t cc[4] = {uint8_t(c >> 24), uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)}; std::fwrite(cc, 1, 4, f); };
    chunk("IHDR", {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w), uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0});
    chunk("IDAT", z); chunk("IEND", {}); std::fclose(f);
}

} // namespace nvgsoft
