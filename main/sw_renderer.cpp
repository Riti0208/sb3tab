#include "sw_renderer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include <cmath>
#include <cstring>
#include <cstdlib>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_MALLOC(sz)       heap_caps_malloc(sz, MALLOC_CAP_SPIRAM)
#define STBI_REALLOC(p,newsz) heap_caps_realloc(p, newsz, MALLOC_CAP_SPIRAM)
#define STBI_FREE(p)          heap_caps_free(p)
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

static const char *TAG = "sw_render";

// HTTP download helper
struct DLBuf {
    uint8_t *data;
    size_t len, cap;
};

static esp_err_t dl_handler(esp_http_client_event_t *evt) {
    DLBuf *b = (DLBuf *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (b->len + evt->data_len > b->cap) {
        size_t nc = b->cap * 2;
        while (nc < b->len + evt->data_len) nc *= 2;
        uint8_t *nd = (uint8_t *)heap_caps_realloc(b->data, nc, MALLOC_CAP_SPIRAM);
        if (!nd) return ESP_FAIL;
        b->data = nd;
        b->cap = nc;
    }
    memcpy(b->data + b->len, evt->data, evt->data_len);
    b->len += evt->data_len;
    return ESP_OK;
}

static DLBuf download_asset(const char *md5ext) {
    char url[256];
    snprintf(url, sizeof(url),
             "https://assets.scratch.mit.edu/internalapi/asset/%s/get/", md5ext);

    DLBuf buf = {};
    buf.cap = 32 * 1024;
    buf.data = (uint8_t *)heap_caps_malloc(buf.cap, MALLOC_CAP_SPIRAM);
    if (!buf.data) return buf;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = dl_handler;
    cfg.user_data = &buf;
    cfg.buffer_size = 4096;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || buf.len == 0) {
        ESP_LOGE(TAG, "Download failed: %s (status=%d)", md5ext, status);
        heap_caps_free(buf.data);
        buf.data = nullptr;
        buf.len = 0;
    } else {
        ESP_LOGI(TAG, "Downloaded %s: %zu bytes", md5ext, buf.len);
    }
    return buf;
}

SWRenderer::SWRenderer() {
    fb = (uint8_t *)heap_caps_calloc(STAGE_W * STAGE_H * 3, 1, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    penFb = nullptr;
    clear();
}

SWRenderer::~SWRenderer() {
    if (fb) heap_caps_free(fb);
    if (penFb) heap_caps_free(penFb);
    for (auto &[k, c] : costumes) {
        if (c.rgba) heap_caps_free(c.rgba);
    }
}

void SWRenderer::clear() {
    memset(fb, 0xFF, STAGE_W * STAGE_H * 3);  // white
}

bool SWRenderer::loadCostume(const std::string &md5ext, double rotCenterX, double rotCenterY,
                             const std::string &storeAs) {
    const std::string &key = storeAs.empty() ? md5ext : storeAs;
    if (costumes.count(key)) return true;

    DLBuf buf = download_asset(md5ext.c_str());
    if (!buf.data) return false;

    int w = 0, h = 0;
    uint8_t *pixels = nullptr;

    // Check if SVG by extension
    bool isSvg = (md5ext.size() > 4 && md5ext.substr(md5ext.size() - 4) == ".svg");

    if (isSvg) {
        // Null-terminate for nanosvg parser (it modifies the string in-place)
        buf.data = (uint8_t *)heap_caps_realloc(buf.data, buf.len + 1, MALLOC_CAP_SPIRAM);
        buf.data[buf.len] = '\0';

        NSVGimage *svg = nsvgParse((char *)buf.data, "px", 96.0f);
        heap_caps_free(buf.data);

        if (!svg) {
            ESP_LOGE(TAG, "SVG parse failed: %s", md5ext.c_str());
            return false;
        }

        w = (int)svg->width;
        h = (int)svg->height;
        if (w <= 0 || h <= 0) { nsvgDelete(svg); return false; }

        pixels = (uint8_t *)heap_caps_malloc(w * h * 4, MALLOC_CAP_SPIRAM);
        if (!pixels) { nsvgDelete(svg); return false; }

        NSVGrasterizer *rast = nsvgCreateRasterizer();
        nsvgRasterize(rast, svg, 0, 0, 1.0f, pixels, w, h, w * 4);
        nsvgDeleteRasterizer(rast);
        nsvgDelete(svg);

        ESP_LOGI(TAG, "Rasterized SVG %s: %dx%d", md5ext.c_str(), w, h);
    } else {
        // PNG: decode with stb_image
        int channels;
        pixels = stbi_load_from_memory(buf.data, buf.len, &w, &h, &channels, 4);
        heap_caps_free(buf.data);

        if (!pixels) {
            ESP_LOGE(TAG, "PNG decode failed: %s", md5ext.c_str());
            return false;
        }
        ESP_LOGI(TAG, "Decoded PNG %s: %dx%d", md5ext.c_str(), w, h);
    }

    CostumePixels cp;
    cp.rgba = pixels;
    cp.w = w;
    cp.h = h;
    cp.rotCenterX = rotCenterX;
    cp.rotCenterY = rotCenterY;
    costumes[key] = cp;
    return true;
}

bool SWRenderer::loadCostumeFromMemory(const std::string &name, const uint8_t *data, size_t len,
                                        double rotCenterX, double rotCenterY) {
    if (costumes.count(name)) return true;

    int w = 0, h = 0;
    uint8_t *pixels = nullptr;
    bool isSvg = (name.size() > 4 && name.substr(name.size() - 4) == ".svg");

    if (isSvg) {
        // Copy and null-terminate for nanosvg (it modifies in-place)
        char *svgCopy = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
        if (!svgCopy) return false;
        memcpy(svgCopy, data, len);
        svgCopy[len] = '\0';

        NSVGimage *svg = nsvgParse(svgCopy, "px", 96.0f);
        heap_caps_free(svgCopy);

        if (!svg) { ESP_LOGE(TAG, "SVG parse failed: %s", name.c_str()); return false; }
        w = (int)svg->width;
        h = (int)svg->height;
        if (w <= 0 || h <= 0) { nsvgDelete(svg); return false; }

        pixels = (uint8_t *)heap_caps_malloc(w * h * 4, MALLOC_CAP_SPIRAM);
        if (!pixels) { nsvgDelete(svg); return false; }

        NSVGrasterizer *rast = nsvgCreateRasterizer();
        nsvgRasterize(rast, svg, 0, 0, 1.0f, pixels, w, h, w * 4);
        nsvgDeleteRasterizer(rast);
        nsvgDelete(svg);
        ESP_LOGI(TAG, "Rasterized SVG %s: %dx%d", name.c_str(), w, h);
    } else {
        int channels;
        pixels = stbi_load_from_memory(data, len, &w, &h, &channels, 4);
        if (!pixels) { ESP_LOGE(TAG, "PNG decode failed: %s", name.c_str()); return false; }
        ESP_LOGI(TAG, "Decoded PNG %s: %dx%d", name.c_str(), w, h);
    }

    CostumePixels cp;
    cp.rgba = pixels;
    cp.w = w;
    cp.h = h;
    cp.rotCenterX = rotCenterX;
    cp.rotCenterY = rotCenterY;
    costumes[name] = cp;
    return true;
}

void SWRenderer::drawSprite(const std::string &costumeMd5ext, float x, float y,
                            float direction, float size, bool visible,
                            float ghostEffect, float brightnessEffect) {
    if (!visible) return;
    auto it = costumes.find(costumeMd5ext);
    if (it == costumes.end()) return;

    float alpha = 1.0f - (ghostEffect / 100.0f);
    if (alpha <= 0.0f) return;
    float scale = size / 100.0f;

    blitRGBA(it->second, (int)(STAGE_W / 2.0f + x), (int)(STAGE_H / 2.0f - y),
             scale, direction - 90.0f, alpha, brightnessEffect);
}

void SWRenderer::drawBackdrop(const std::string &costumeMd5ext) {
    auto it = costumes.find(costumeMd5ext);
    if (it == costumes.end()) return;

    const CostumePixels &cos = it->second;
    if (!cos.rgba) return;

    float scaleX = (float)STAGE_W / cos.w;
    float scaleY = (float)STAGE_H / cos.h;

    for (int dy = 0; dy < STAGE_H; dy++) {
        int srcY = (int)(dy / scaleY);
        if (srcY >= cos.h) srcY = cos.h - 1;
        for (int dx = 0; dx < STAGE_W; dx++) {
            int srcX = (int)(dx / scaleX);
            if (srcX >= cos.w) srcX = cos.w - 1;
            const uint8_t *src = cos.rgba + (srcY * cos.w + srcX) * 4;
            if (src[3] == 0) continue;
            uint8_t *dst = fb + (dy * STAGE_W + dx) * 3;
            if (src[3] >= 254) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
            } else {
                int a = src[3];
                dst[0] = (src[0] * a + dst[0] * (255 - a)) >> 8;
                dst[1] = (src[1] * a + dst[1] * (255 - a)) >> 8;
                dst[2] = (src[2] * a + dst[2] * (255 - a)) >> 8;
            }
        }
    }
}

// ---- Pen layer ----

void SWRenderer::initPenLayer() {
    if (penFb) return;
    penFb = (uint8_t *)heap_caps_calloc(STAGE_W * STAGE_H * 4, 1, MALLOC_CAP_SPIRAM);
}

void SWRenderer::clearPenLayer() {
    if (penFb) memset(penFb, 0, STAGE_W * STAGE_H * 4);
}

void SWRenderer::compositePenLayer() {
    if (!penFb) return;
    for (int i = 0; i < STAGE_W * STAGE_H; i++) {
        uint8_t *src = penFb + i * 4;
        if (src[3] == 0) continue;
        uint8_t *dst = fb + i * 3;
        int a = src[3];
        if (a >= 254) {
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        } else {
            dst[0] = (src[0] * a + dst[0] * (255 - a)) >> 8;
            dst[1] = (src[1] * a + dst[1] * (255 - a)) >> 8;
            dst[2] = (src[2] * a + dst[2] * (255 - a)) >> 8;
        }
    }
}

static void penPixel(uint8_t *penFb, int x, int y,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= STAGE_W || y < 0 || y >= STAGE_H) return;
    uint8_t *dst = penFb + (y * STAGE_W + x) * 4;
    if (a >= 254) {
        dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = 255;
    } else {
        int oldA = dst[3];
        int newA = a + oldA * (255 - a) / 255;
        if (newA > 0) {
            dst[0] = (r * a + dst[0] * oldA * (255 - a) / 255) / newA;
            dst[1] = (g * a + dst[1] * oldA * (255 - a) / 255) / newA;
            dst[2] = (b * a + dst[2] * oldA * (255 - a) / 255) / newA;
            dst[3] = newA > 255 ? 255 : newA;
        }
    }
}

void SWRenderer::penDot(float x, float y,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a, float thickness) {
    if (!penFb) return;
    int cx = (int)(STAGE_W / 2.0f + x);
    int cy = (int)(STAGE_H / 2.0f - y);
    int rad = (int)(thickness / 2.0f + 0.5f);
    if (rad < 1) rad = 1;
    for (int dy = -rad; dy <= rad; dy++) {
        for (int dx = -rad; dx <= rad; dx++) {
            if (dx * dx + dy * dy <= rad * rad) {
                penPixel(penFb, cx + dx, cy + dy, r, g, b, a);
            }
        }
    }
}

void SWRenderer::penLine(float x1, float y1, float x2, float y2,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a, float thickness) {
    if (!penFb) return;
    // Bresenham with thickness via penDot at each step
    int sx = (int)(STAGE_W / 2.0f + x1);
    int sy = (int)(STAGE_H / 2.0f - y1);
    int ex = (int)(STAGE_W / 2.0f + x2);
    int ey = (int)(STAGE_H / 2.0f - y2);

    int dx = abs(ex - sx), dy = -abs(ey - sy);
    int stepX = sx < ex ? 1 : -1;
    int stepY = sy < ey ? 1 : -1;
    int err = dx + dy;
    int rad = (int)(thickness / 2.0f + 0.5f);
    if (rad < 1) rad = 1;

    while (true) {
        // Draw dot at current pos
        for (int py = -rad; py <= rad; py++)
            for (int px = -rad; px <= rad; px++)
                if (px * px + py * py <= rad * rad)
                    penPixel(penFb, sx + px, sy + py, r, g, b, a);

        if (sx == ex && sy == ey) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; sx += stepX; }
        if (e2 <= dx) { err += dx; sy += stepY; }
    }
}

void SWRenderer::penStampSprite(const std::string &costumeMd5ext, float x, float y,
                                float direction, float size, float ghostEffect) {
    // Draw sprite onto pen layer instead of main fb (swap fb temporarily)
    if (!penFb) return;
    auto it = costumes.find(costumeMd5ext);
    if (it == costumes.end()) return;

    float alpha = 1.0f - (ghostEffect / 100.0f);
    if (alpha <= 0.0f) return;
    float scale = size / 100.0f;

    const CostumePixels &cos = it->second;
    if (!cos.rgba || scale <= 0.0f) return;

    float rad = (direction - 90.0f) * M_PI / 180.0f;
    float cosA = cosf(rad), sinA = sinf(rad);
    int cx = (int)(STAGE_W / 2.0f + x);
    int cy = (int)(STAGE_H / 2.0f - y);

    float corners[4][2] = {
        { (float)-cos.rotCenterX * scale, (float)-cos.rotCenterY * scale },
        { (float)(cos.w - cos.rotCenterX) * scale, (float)-cos.rotCenterY * scale },
        { (float)-cos.rotCenterX * scale, (float)(cos.h - cos.rotCenterY) * scale },
        { (float)(cos.w - cos.rotCenterX) * scale, (float)(cos.h - cos.rotCenterY) * scale },
    };
    float bminX = 1e9, bmaxX = -1e9, bminY = 1e9, bmaxY = -1e9;
    for (auto &c : corners) {
        float rx = cosA * c[0] - sinA * c[1];
        float ry = sinA * c[0] + cosA * c[1];
        if (rx < bminX) bminX = rx;
        if (rx > bmaxX) bmaxX = rx;
        if (ry < bminY) bminY = ry;
        if (ry > bmaxY) bmaxY = ry;
    }
    int minX = std::max(0, cx + (int)floorf(bminX) - 1);
    int maxX = std::min(STAGE_W - 1, cx + (int)ceilf(bmaxX) + 1);
    int minY = std::max(0, cy + (int)floorf(bminY) - 1);
    int maxY = std::min(STAGE_H - 1, cy + (int)ceilf(bmaxY) + 1);

    int alphaI = (int)(alpha * 255);
    float invScale = 1.0f / scale;

    for (int dy = minY; dy <= maxY; dy++) {
        for (int dx = minX; dx <= maxX; dx++) {
            float u = (float)(dx - cx) * invScale;
            float v = (float)(dy - cy) * invScale;
            int srcX = (int)(cosA * u + sinA * v + cos.rotCenterX);
            int srcY = (int)(-sinA * u + cosA * v + cos.rotCenterY);
            if (srcX < 0 || srcX >= cos.w || srcY < 0 || srcY >= cos.h) continue;
            const uint8_t *src = cos.rgba + (srcY * cos.w + srcX) * 4;
            int srcA = (src[3] * alphaI) >> 8;
            if (srcA == 0) continue;
            penPixel(penFb, dx, dy, src[0], src[1], src[2], srcA);
        }
    }
}

void SWRenderer::blitRGBA(const CostumePixels &cos, int cx, int cy,
                          float scale, float angleDeg, float alpha,
                          float brightness) {
    if (!cos.rgba || scale <= 0.0f) return;

    float rad = angleDeg * M_PI / 180.0f;
    float cosA = cosf(rad);
    float sinA = sinf(rad);

    // Compute bounding box from all 4 corners of the scaled sprite
    // Sprite extends from (-rotCenterX, -rotCenterY) to (w-rotCenterX, h-rotCenterY)
    float corners[4][2] = {
        { (float)-cos.rotCenterX * scale, (float)-cos.rotCenterY * scale },
        { (float)(cos.w - cos.rotCenterX) * scale, (float)-cos.rotCenterY * scale },
        { (float)-cos.rotCenterX * scale, (float)(cos.h - cos.rotCenterY) * scale },
        { (float)(cos.w - cos.rotCenterX) * scale, (float)(cos.h - cos.rotCenterY) * scale },
    };

    float bminX = 1e9, bmaxX = -1e9, bminY = 1e9, bmaxY = -1e9;
    for (auto &c : corners) {
        float rx = cosA * c[0] - sinA * c[1];
        float ry = sinA * c[0] + cosA * c[1];
        if (rx < bminX) bminX = rx;
        if (rx > bmaxX) bmaxX = rx;
        if (ry < bminY) bminY = ry;
        if (ry > bmaxY) bmaxY = ry;
    }

    int minX = cx + (int)floorf(bminX) - 1;
    int maxX = cx + (int)ceilf(bmaxX) + 1;
    int minY = cy + (int)floorf(bminY) - 1;
    int maxY = cy + (int)ceilf(bmaxY) + 1;

    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX >= STAGE_W) maxX = STAGE_W - 1;
    if (maxY >= STAGE_H) maxY = STAGE_H - 1;

    int alphaI = (int)(alpha * 255);
    float invScale = 1.0f / scale;

    for (int dy = minY; dy <= maxY; dy++) {
        for (int dx = minX; dx <= maxX; dx++) {
            // Inverse transform: screen → sprite local coords
            float u = (float)(dx - cx) * invScale;
            float v = (float)(dy - cy) * invScale;

            // Inverse rotation + translate to sprite pixel coords
            int srcX = (int)(cosA * u + sinA * v + cos.rotCenterX);
            int srcY = (int)(-sinA * u + cosA * v + cos.rotCenterY);

            if (srcX < 0 || srcX >= cos.w || srcY < 0 || srcY >= cos.h) continue;

            const uint8_t *src = cos.rgba + (srcY * cos.w + srcX) * 4;
            int srcA = (src[3] * alphaI) >> 8;
            if (srcA == 0) continue;

            int pr = src[0], pg = src[1], pb = src[2];
            // Apply brightness effect (-100 to 100)
            if (brightness > 0.5f) {
                float t = brightness / 100.0f;
                pr = pr + (int)((255 - pr) * t);
                pg = pg + (int)((255 - pg) * t);
                pb = pb + (int)((255 - pb) * t);
            } else if (brightness < -0.5f) {
                float t = 1.0f + brightness / 100.0f;
                pr = (int)(pr * t);
                pg = (int)(pg * t);
                pb = (int)(pb * t);
            }

            uint8_t *dst = fb + (dy * STAGE_W + dx) * 3;
            if (srcA >= 254) {
                dst[0] = pr;
                dst[1] = pg;
                dst[2] = pb;
            } else {
                dst[0] = (pr * srcA + dst[0] * (255 - srcA)) >> 8;
                dst[1] = (pg * srcA + dst[1] * (255 - srcA)) >> 8;
                dst[2] = (pb * srcA + dst[2] * (255 - srcA)) >> 8;
            }
        }
    }
}

// JPEG encoding callback
struct JpegWriteCtx {
    uint8_t *data;
    int len, cap;
};

static void jpeg_write_func(void *context, void *data, int size) {
    JpegWriteCtx *ctx = (JpegWriteCtx *)context;
    if (ctx->len + size > ctx->cap) {
        int nc = ctx->cap * 2;
        while (nc < ctx->len + size) nc *= 2;
        ctx->data = (uint8_t *)heap_caps_realloc(ctx->data, nc, MALLOC_CAP_SPIRAM);
        ctx->cap = nc;
    }
    memcpy(ctx->data + ctx->len, data, size);
    ctx->len += size;
}

uint8_t *SWRenderer::encodeJpeg(int quality, int *outSize) {
    JpegWriteCtx ctx = {};
    ctx.cap = 32 * 1024;
    ctx.data = (uint8_t *)heap_caps_malloc(ctx.cap, MALLOC_CAP_SPIRAM);
    ctx.len = 0;

    stbi_write_jpg_to_func(jpeg_write_func, &ctx, STAGE_W, STAGE_H, 3, fb, quality);

    *outSize = ctx.len;
    return ctx.data;
}
