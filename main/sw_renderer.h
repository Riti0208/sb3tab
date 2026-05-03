#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <list>
#include <functional>
#include <unordered_map>
#include "esp_http_client.h"

// Framebuffer at 4/5 of Scratch's 480x360 logical stage.
// PPA SRM upscales 2.5x to the DSI scratch region (960x720). Non-integer
// scaling means pixels expand unevenly (alternating 2/3-pixel groups) but
// the detail is visibly better than 320x240, and blit work is ~64% of full.
#define STAGE_W 384
#define STAGE_H 288
#define LOGICAL_TO_FB 0.8f

struct CostumePixels {
    uint8_t *rgba = nullptr;  // RGBA8888 — may be nullptr when evicted from LRU.
    // Raster dimensions: the size of `rgba` in actual pixels. SVGs whose full
    // rasterisation would exceed MAX_SVG_RASTER_BYTES are downscaled at decode
    // time to fit the LRU budget; for those, `decodeScale` < 1.0 records the
    // scale factor so blit can compensate (each raster pixel represents
    // 1/decodeScale logical pixels of visual size).
    int w = 0;
    int h = 0;
    double rotCenterX = 0;  // already multiplied by decodeScale (raster coords)
    double rotCenterY = 0;
    // Trimmed (visible) bounds in raster pixel coords (so they line up with
    // rgba for collision); convert back to logical via decodeScale on read.
    int trimX = 0, trimY = 0, trimW = 0, trimH = 0;
    // Per-row visibility: 1 if that costume row has any non-transparent pixel,
    // 0 if fully empty. Lets the blit loop skip whole-row no-ops, big win for
    // SVGs with sparse content (e.g. ground sprite with transparent middle).
    uint8_t *rowSet = nullptr;
    // True once dimensions/trim/rowSet have been computed at least once.
    bool metaReady = false;
    // Suppresses repeat decode-failure logs for this costume so a missing
    // sprite that's drawn every frame doesn't flood the console. Cleared on
    // a successful decode, so a later transient failure logs again.
    bool loggedFailure = false;
    // Once a decode definitively fails (parse error, alloc-NULL even after
    // size cap), skip further attempts. Stops the runtime from re-decoding
    // the same broken costume every frame and burning PSRAM bandwidth.
    bool decodeFailed = false;
    // raster_size / logical_size, ≤1.0. SVGs are downscaled on decode if a
    // full-resolution rasterisation would exceed MAX_SVG_RASTER_BYTES. PNG
    // costumes are always 1.0 (we have to use whatever size the file is).
    float decodeScale = 1.0f;
    // Scratch's "bitmap resolution" attribute (1 or 2). Resolution=2 means
    // the bitmap is 2× the logical size — a high-DPI/Retina costume that
    // should display at half its raw pixel dimensions. Used to scale the
    // visual output and the values returned by getCostumeSize.
    int bitmapResolution = 1;
};

class SWRenderer {
public:
    SWRenderer();
    ~SWRenderer();

    // Download and decode a costume PNG from Scratch CDN
    // storeAs: optional key name (if different from md5ext, e.g. original SVG name)
    bool loadCostume(const std::string &md5ext, double rotCenterX, double rotCenterY,
                     const std::string &storeAs = "");

    // Load costume from memory buffer (for .sb3 ZIP extraction).
    // bitmapResolution comes from the Scratch costume metadata (1 or 2);
    // 2 indicates a high-DPI bitmap whose visual size is half its raw pixels.
    bool loadCostumeFromMemory(const std::string &name, const uint8_t *data, size_t len,
                                double rotCenterX, double rotCenterY,
                                int bitmapResolution = 1);

    // Clear framebuffer to white (full clear)
    void clear();

    // Clear only dirty rects from previous frame (for white backdrop)
    // Returns false if full clear is needed (first frame or too many dirty rects)
    bool clearDirty();

    // Draw a sprite onto the framebuffer
    void drawSprite(const std::string &costumeMd5ext, float x, float y,
                    float direction, float size, bool visible,
                    float ghostEffect, float brightnessEffect = 0.0f,
                    bool flipH = false);

    // Draw stage backdrop (no rotation, fills entire stage)
    void drawBackdrop(const std::string &costumeMd5ext);

    // Draw backdrop filling every pixel (replaces clear+drawBackdrop)
    // Returns false if costume not found (caller should clear instead)
    bool drawBackdropFull(const std::string &costumeMd5ext);

    // Pen layer
    void initPenLayer();
    void clearPenLayer();
    uint8_t *getPenLayer() { return penFb; }
    void compositePenLayer();
    void penLine(float x1, float y1, float x2, float y2,
                 uint8_t r, uint8_t g, uint8_t b, uint8_t a, float thickness);
    void penDot(float x, float y,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a, float thickness);
    void penStampSprite(const std::string &costumeMd5ext, float x, float y,
                        float direction, float size, float ghostEffect);

    // Get costume dimensions (returns false if not found)
    bool getCostumeSize(const std::string &name, int &w, int &h) const;

    // Get trimmed (visible-pixel) bounds in costume pixel coords
    bool getCostumeTrimBounds(const std::string &name, int &trimX, int &trimY, int &trimW, int &trimH) const;

    // Get raw RGBA pixel buffer for a costume (used by pixel-perfect collision).
    // Returns nullptr if not loaded.
    const uint8_t *getCostumeRGBA(const std::string &name, int &w, int &h) const;

    // Encode framebuffer as JPEG, returns malloc'd buffer (caller must free)
    uint8_t *encodeJpeg(int quality, int *outSize);

    // Get raw framebuffer (current draw target)
    uint8_t *getFramebuffer() { return fb[currentFb]; }

    // Swap framebuffers: returns the completed frame's buffer for display
    uint8_t *swapFramebuffer() {
        uint8_t *completed = fb[currentFb];
        currentFb ^= 1;
        return completed;
    }

    // Notify a dirty region was drawn (called internally and by external renderers)
    void addDirtyRect(int x, int y, int w, int h);

    // Free all rasterized costume RGBAs and the row-set masks. Called between
    // games so the freshly-decoded ~10 MB of SVG bitmaps don't fragment PSRAM
    // and starve the next project's large WAV/asset allocations.
    void clearCostumes();

    // Lazy-load support: caller installs a reader that returns the raw
    // encoded bytes (PNG/SVG) for a given costume name, fetched from
    // wherever the project lives (SD card, in-memory g_assets, ...). When
    // a costume's RGBA has been evicted to stay under the budget, the
    // renderer calls this to decode it again on demand.
    using AssetReader = std::function<std::vector<uint8_t>(const std::string &)>;
    void setAssetReader(AssetReader r) { assetReader = std::move(r); }

    // Cap on RGBA bytes held resident across all costumes. When exceeded
    // after a decode, the LRU costume is evicted (rgba freed; metadata kept
    // so AABB sizing/trim still works without re-decoding).
    void setCostumeBudgetBytes(size_t bytes) { costumeBudgetBytes = bytes; }

private:
    uint8_t *fb[2];   // Double-buffered STAGE_W * STAGE_H * 3 (RGB888)
    int currentFb = 0;
    uint8_t *penFb;   // STAGE_W * STAGE_H * 4 (RGBA8888) pen layer
    std::unordered_map<std::string, CostumePixels> costumes;

    // LRU bookkeeping for decoded RGBA buffers. lruList is most-recent-first;
    // lruIter[name] points into it for O(1) move-to-front and erase.
    std::list<std::string> lruList;
    std::unordered_map<std::string, std::list<std::string>::iterator> lruIter;
    size_t totalRgbaBytes = 0;
    size_t costumeBudgetBytes = 8 * 1024 * 1024;  // 8 MB default
    AssetReader assetReader;
    // Serialises the lazy-load / LRU bookkeeping path so two cores rendering
    // different sprites can't trample lruList and lruIter at the same time.
    // The actual blit reads cp.rgba *outside* the lock, which is safe because
    // touchLru pins the just-touched costume at the front of the LRU and
    // evictUntilUnderBudget only removes from the back.
    void *costumeLock = nullptr;  // SemaphoreHandle_t (mutex), opaque here

    // Decode the encoded bytes into cp.rgba (and on first call also fill
    // w/h/trim/rowSet). Returns true on success.
    bool decodeInto(CostumePixels &cp, const std::string &name,
                    const uint8_t *data, size_t len);

    // Make sure cp.rgba is loaded. Re-fetches via assetReader if needed.
    // Returns nullptr-equivalent failure as false; on success the costume
    // is moved to the front of the LRU.
    bool ensureDecoded(const std::string &name, CostumePixels &cp);

    // Drop cp.rgba (and rowSet, since it's tied to the same decode). Keeps
    // metadata. totalRgbaBytes is updated.
    void evictRgba(CostumePixels &cp, const std::string &name);

    // Mark costume as most-recently-used.
    void touchLru(const std::string &name);

    // Evict from the back of the LRU until totalRgbaBytes < costumeBudgetBytes.
    void evictUntilUnderBudget();

    struct DirtyRect { int16_t x, y, w, h; };
    std::vector<DirtyRect> dirtyRects[2];  // per framebuffer
    bool needFullClear[2] = {true, true};

    void blitRGBA(const CostumePixels &cos, int dstX, int dstY,
                  float scale, float angleDeg, float alpha,
                  float brightness = 0.0f, bool flipH = false);
};
