#include "sw_renderer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "driver/ppa.h"
#include <cmath>
#include <cstring>
#include <cstdlib>

static ppa_client_handle_t s_ppa_fill = NULL;

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
    fb[0] = (uint8_t *)heap_caps_calloc(STAGE_W * STAGE_H * 3, 1, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    fb[1] = (uint8_t *)heap_caps_calloc(STAGE_W * STAGE_H * 3, 1, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    currentFb = 0;
    penFb = nullptr;

    // Init PPA fill client for fast clear
    if (!s_ppa_fill) {
        ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_FILL, .max_pending_trans_num = 1 };
        if (ppa_register_client(&cfg, &s_ppa_fill) != ESP_OK) {
            ESP_LOGW(TAG, "PPA fill init failed, using memset fallback");
            s_ppa_fill = NULL;
        }
    }

    clear();
}

SWRenderer::~SWRenderer() {
    if (fb[0]) heap_caps_free(fb[0]);
    if (fb[1]) heap_caps_free(fb[1]);
    if (penFb) heap_caps_free(penFb);
    for (auto &[k, c] : costumes) {
        if (c.rgba) heap_caps_free(c.rgba);
    }
}

void SWRenderer::addDirtyRect(int x, int y, int w, int h) {
    // Disabled: not thread-safe for dual-core rendering,
    // and not effective for projects with many sprites covering >50% of screen
    (void)x; (void)y; (void)w; (void)h;
}

bool SWRenderer::clearDirty() {
    auto &rects = dirtyRects[currentFb];
    if (needFullClear[currentFb] || rects.empty()) {
        needFullClear[currentFb] = false;
        rects.clear();
        return false;  // caller should do full clear
    }

    // Calculate total dirty area to decide if full clear is cheaper
    int totalPixels = 0;
    for (auto &r : rects) totalPixels += r.w * r.h;
    if (totalPixels > STAGE_W * STAGE_H / 2) {
        rects.clear();
        return false;  // too much dirty area, full clear is better
    }

    uint8_t *cur = fb[currentFb];
    for (auto &r : rects) {
        for (int dy = r.y; dy < r.y + r.h; dy++) {
            memset(cur + (dy * STAGE_W + r.x) * 3, 0xFF, r.w * 3);
        }
    }
    rects.clear();
    return true;
}

void SWRenderer::clear() {
    dirtyRects[currentFb].clear();
    uint8_t *cur = fb[currentFb];
    memset(cur, 0xFF, STAGE_W * STAGE_H * 3);
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
                            float ghostEffect, float brightnessEffect,
                            bool flipH) {
    if (!visible) return;
    auto it = costumes.find(costumeMd5ext);
    if (it == costumes.end()) return;

    float alpha = 1.0f - (ghostEffect / 100.0f);
    if (alpha <= 0.0f) return;
    float scale = size / 100.0f;

    blitRGBA(it->second, (int)(STAGE_W / 2.0f + x), (int)(STAGE_H / 2.0f - y),
             scale, direction - 90.0f, alpha, brightnessEffect, flipH);
}

void SWRenderer::drawBackdrop(const std::string &costumeMd5ext) {
    auto it = costumes.find(costumeMd5ext);
    if (it == costumes.end()) return;

    const CostumePixels &cos = it->second;
    if (!cos.rgba) return;

    float scaleX = (float)STAGE_W / cos.w;
    float scaleY = (float)STAGE_H / cos.h;
    uint8_t *cur = fb[currentFb];

    for (int dy = 0; dy < STAGE_H; dy++) {
        int srcY = (int)(dy / scaleY);
        if (srcY >= cos.h) srcY = cos.h - 1;
        for (int dx = 0; dx < STAGE_W; dx++) {
            int srcX = (int)(dx / scaleX);
            if (srcX >= cos.w) srcX = cos.w - 1;
            const uint8_t *src = cos.rgba + (srcY * cos.w + srcX) * 4;
            if (src[3] == 0) continue;
            uint8_t *dst = cur + (dy * STAGE_W + dx) * 3;
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

bool SWRenderer::drawBackdropFull(const std::string &costumeMd5ext) {
    auto it = costumes.find(costumeMd5ext);
    if (it == costumes.end()) return false;

    const CostumePixels &cos = it->second;
    if (!cos.rgba) return false;

    // Check if backdrop is trivially white/empty (cache result per costume)
    static std::string lastCheckedCostume;
    static bool lastWasTrivialWhite = false;
    if (costumeMd5ext != lastCheckedCostume) {
        lastCheckedCostume = costumeMd5ext;
        lastWasTrivialWhite = true;
        for (int i = 0; i < cos.w * cos.h && lastWasTrivialWhite; i++) {
            const uint8_t *p = cos.rgba + i * 4;
            // Check if pixel is either transparent or white
            if (p[3] == 0) continue;
            if (p[3] >= 254 && p[0] >= 254 && p[1] >= 254 && p[2] >= 254) continue;
            lastWasTrivialWhite = false;
        }
    }

    if (lastWasTrivialWhite) {
        clear();  // Use non-cacheable clear
        return true;
    }

    // Non-trivial backdrop: render with alpha over white
    float scaleX = (float)STAGE_W / cos.w;
    float scaleY = (float)STAGE_H / cos.h;
    uint8_t *cur = fb[currentFb];

    for (int dy = 0; dy < STAGE_H; dy++) {
        int srcY = (int)(dy / scaleY);
        if (srcY >= cos.h) srcY = cos.h - 1;
        uint8_t *dstRow = cur + dy * STAGE_W * 3;
        const uint8_t *srcRow = cos.rgba + srcY * cos.w * 4;
        for (int dx = 0; dx < STAGE_W; dx++) {
            int srcX = (int)(dx / scaleX);
            if (srcX >= cos.w) srcX = cos.w - 1;
            const uint8_t *src = srcRow + srcX * 4;
            uint8_t *dst = dstRow + dx * 3;
            if (src[3] >= 254) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
            } else if (src[3] == 0) {
                dst[0] = 255; dst[1] = 255; dst[2] = 255;
            } else {
                int a = src[3];
                dst[0] = src[0] + (((255 - src[0]) * (255 - a)) >> 8);
                dst[1] = src[1] + (((255 - src[1]) * (255 - a)) >> 8);
                dst[2] = src[2] + (((255 - src[2]) * (255 - a)) >> 8);
            }
        }
    }
    return true;
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
    uint8_t *cur = fb[currentFb];
    for (int i = 0; i < STAGE_W * STAGE_H; i++) {
        uint8_t *src = penFb + i * 4;
        if (src[3] == 0) continue;
        uint8_t *dst = cur + i * 3;
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

// Internal SRAM cache for fast costume pixel access (avoids slow PSRAM random reads)
// Per-core to avoid race conditions in dual-core rendering
static uint8_t *s_iram_cache[2] = {nullptr, nullptr};
static int s_iram_cache_cap[2] = {0, 0};
static const uint8_t *s_iram_cached_src[2] = {nullptr, nullptr};

void SWRenderer::blitRGBA(const CostumePixels &cos, int cx, int cy,
                          float scale, float angleDeg, float alpha,
                          float brightness, bool flipH) {
    if (!cos.rgba || scale <= 0.0f) return;

    float rad = angleDeg * M_PI / 180.0f;
    float cosA = cosf(rad);
    float sinA = sinf(rad);

    // Compute bounding box from all 4 corners of the scaled sprite
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
    if (minX > maxX || minY > maxY) return;

    // Track dirty rect for next frame's partial clear
    addDirtyRect(minX, minY, maxX - minX + 1, maxY - minY + 1);

    int alphaI = (int)(alpha * 255);
    float invScale = 1.0f / scale;

    // Cache small costumes in internal SRAM (clones share same rgba pointer → cache hit)
    // Per-core cache to avoid race conditions in dual-core rendering
    const uint8_t *srcPixels = cos.rgba;
    int costumeBytes = cos.w * cos.h * 4;
    int coreId = xPortGetCoreID();
    if (costumeBytes <= 64 * 1024) {
        if (cos.rgba != s_iram_cached_src[coreId] || !s_iram_cache[coreId]) {
            if (!s_iram_cache[coreId] || s_iram_cache_cap[coreId] < costumeBytes) {
                if (s_iram_cache[coreId]) heap_caps_free(s_iram_cache[coreId]);
                s_iram_cache_cap[coreId] = costumeBytes;
                s_iram_cache[coreId] = (uint8_t *)heap_caps_malloc(s_iram_cache_cap[coreId], MALLOC_CAP_INTERNAL);
            }
            if (s_iram_cache[coreId]) {
                memcpy(s_iram_cache[coreId], cos.rgba, costumeBytes);
                s_iram_cached_src[coreId] = cos.rgba;
            }
        }
        if (s_iram_cache[coreId]) srcPixels = s_iram_cache[coreId];
    }

    // Fixed-point 16.16 incremental computation (eliminates multiply per pixel)
    int32_t dSrcX_fp = (int32_t)(cosA * invScale * 65536.0f);
    int32_t dSrcY_fp = (int32_t)(-sinA * invScale * 65536.0f);

    // Pre-compute brightness as fixed-point
    int32_t brightFp = 0;
    bool brightPos = brightness > 0.5f;
    bool brightNeg = brightness < -0.5f;
    if (brightPos) brightFp = (int32_t)(brightness * 256.0f / 100.0f);
    else if (brightNeg) brightFp = (int32_t)((1.0f + brightness / 100.0f) * 256.0f);

    const unsigned uw = (unsigned)cos.w;
    const unsigned uh = (unsigned)cos.h;
    const int stride = cos.w * 4;

    for (int dy = minY; dy <= maxY; dy++) {
        float u0 = (float)(minX - cx) * invScale;
        float v0 = (float)(dy - cy) * invScale;
        int32_t srcX_fp = (int32_t)((cosA * u0 + sinA * v0 + cos.rotCenterX) * 65536.0f);
        int32_t srcY_fp = (int32_t)((-sinA * u0 + cosA * v0 + cos.rotCenterY) * 65536.0f);

        uint8_t *dstRow = fb[currentFb] + (dy * STAGE_W + minX) * 3;

        for (int dx = minX; dx <= maxX; dx++) {
            int srcX = srcX_fp >> 16;
            int srcY = srcY_fp >> 16;
            srcX_fp += dSrcX_fp;
            srcY_fp += dSrcY_fp;

            if (flipH) srcX = (int)uw - 1 - srcX;

            if ((unsigned)srcX >= uw || (unsigned)srcY >= uh) {
                dstRow += 3;
                continue;
            }

            const uint8_t *src = srcPixels + srcY * stride + srcX * 4;
            int srcA = (src[3] * alphaI) >> 8;
            if (srcA == 0) { dstRow += 3; continue; }

            int pr = src[0], pg = src[1], pb = src[2];
            if (brightPos) {
                pr += ((255 - pr) * brightFp) >> 8;
                pg += ((255 - pg) * brightFp) >> 8;
                pb += ((255 - pb) * brightFp) >> 8;
            } else if (brightNeg) {
                pr = (pr * brightFp) >> 8;
                pg = (pg * brightFp) >> 8;
                pb = (pb * brightFp) >> 8;
            }

            if (srcA >= 254) {
                dstRow[0] = pr;
                dstRow[1] = pg;
                dstRow[2] = pb;
            } else {
                int invA = 255 - srcA;
                dstRow[0] = (pr * srcA + dstRow[0] * invA) >> 8;
                dstRow[1] = (pg * srcA + dstRow[1] * invA) >> 8;
                dstRow[2] = (pb * srcA + dstRow[2] * invA) >> 8;
            }
            dstRow += 3;
        }
    }
}

bool SWRenderer::getCostumeSize(const std::string &name, int &w, int &h) const {
    auto it = costumes.find(name);
    if (it == costumes.end()) return false;
    w = it->second.w;
    h = it->second.h;
    return true;
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

    stbi_write_jpg_to_func(jpeg_write_func, &ctx, STAGE_W, STAGE_H, 3, fb[currentFb], quality);

    *outSize = ctx.len;
    return ctx.data;
}
