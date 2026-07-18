#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <sys/stat.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include "apk.h"
#include "compat/loader.h"
#include "build_number.h"
#include "avatar.h"

extern void compatLogFmt(const char* fmt, ...);

static constexpr float PI_F = 3.14159265358979323846f;

static const char* APK_DIR  = "sdmc:/Viridite/apks";
static const char* LOG_FILE = "sdmc:/Viridite/log.txt";

// ---------------------------------------------------------------------------
// Layout (1280×720)
// ---------------------------------------------------------------------------
static const int SW       = 1280;
static const int SH       = 720;
static const int HEADER_H = 72;
static const int FOOTER_H = 48;
static const int LIST_Y   = HEADER_H;
static const int LIST_H   = SH - HEADER_H - FOOTER_H;
static const int ITEM_H   = 108;
static const int ICON_SZ  = 84;
static const int VISIBLE  = LIST_H / ITEM_H;

// Viridite light theme — white base with the logo's vivid green as the accent.
// C_WHITE is the primary TEXT colour (dark on white); matches the launcher.
static const SDL_Color C_BG     = {248, 251, 249, 255};  // near-white background
static const SDL_Color C_HEADER = {255, 255, 255, 255};  // white surface
static const SDL_Color C_FOOTER = {242, 248, 245, 255};  // light footer
static const SDL_Color C_SEL    = {205, 244, 224, 255};  // light mint selection
static const SDL_Color C_DIV    = {224, 234, 228, 255};  // light divider/border
static const SDL_Color C_WHITE  = {17,  32,  24,  255};  // primary text (dark)
static const SDL_Color C_GRAY   = {92,  112, 102, 255};  // secondary text
static const SDL_Color C_DIM    = {142, 160, 150, 255};  // tertiary text
static const SDL_Color C_OK     = {0,   170, 80,  255};  // accent (vivid green)
static const SDL_Color C_ERR    = {214, 48,  79,  255};
static const SDL_Color C_WARN   = {176, 120, 0,   255};
static const SDL_Color C_INST   = {0,   170, 80,  255};
static const SDL_Color C_RIM    = {0,   190, 90,  255};  // accent rim

// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
static void logOpen()  { g_log = fopen(LOG_FILE, "w"); }
static void logClose() { if (g_log) { fclose(g_log); g_log = nullptr; } }
static void logMsg(const char* msg) {
    if (g_log) { fputs(msg, g_log); fputc('\n', g_log); fflush(g_log); }
}
static void logSDL(const char* prefix) {
    if (!g_log) return;
    fputs(prefix, g_log); fputs(": ", g_log);
    fputs(SDL_GetError(), g_log); fputc('\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------------------
static const int BTN_A     = 0;
static const int BTN_B     = 1;
static const int BTN_X     = 2;
static const int BTN_Y     = 3;
static const int BTN_PLUS  = 10;
static const int BTN_MINUS = 11;

// ---------------------------------------------------------------------------
// Shared loader state — written by loader thread, read by main thread.
// ---------------------------------------------------------------------------
// Ring buffers defined in loader.cpp
extern char g_ui_log[20][128];   // throttled UI messages (every 512 entries etc.)
extern int  g_ui_head;
extern int  g_ui_pct;
// Full-detail log: every compatLog line written here — read by render thread without file I/O
extern char g_detail_log[28][164];
extern int  g_detail_head;

// Current high-level stage string, set by progressCallback on the loader thread.
static char g_ui_stage[80] = "Working...";

// ---------------------------------------------------------------------------
// Loader thread plumbing
// ---------------------------------------------------------------------------
struct LoaderCtx {
    std::string       apk_path;
    std::string       pkg_name;
    bool              skip_install = false;
    LaunchResult      result;
    std::atomic<bool> done{false};
};

static LoaderCtx* g_loader_ctx = nullptr;

// Progress callback — called from loader thread.
// Updates shared state only; never touches SDL (wrong thread).
static void progressCallback(const char* stage, const char* /*detail*/) {
    if (stage) {
        strncpy(g_ui_stage, stage, sizeof(g_ui_stage) - 1);
        g_ui_stage[sizeof(g_ui_stage) - 1] = '\0';
    }
}

static void loaderThreadFn(void*) {
    g_loader_ctx->result = launchApk(
        g_loader_ctx->apk_path,
        g_loader_ctx->pkg_name,
        progressCallback,
        g_loader_ctx->skip_install
    );
    g_loader_ctx->done.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
struct App {
    SDL_Window*    win  = nullptr;
    SDL_Renderer*  rdr  = nullptr;
    TTF_Font*      fLg  = nullptr;
    TTF_Font*      fMd  = nullptr;  // 20px — monospace-ish for log lines
    TTF_Font*      fSm  = nullptr;
    SDL_Joystick*  joy  = nullptr;

    std::vector<ApkInfo>      apks;
    std::vector<SDL_Texture*> icons;
    int selected = 0;
    int scroll   = 0;

    SDL_Texture* avatarTex = nullptr;

    // ── Scenery: cached sky/planet texture + animated starfield ─────────
    SDL_Texture* bgTex = nullptr;
    TTF_Font*    fBtn  = nullptr;  // NintendoExt shared font: HOS button glyphs
    struct Star { float x, y; int sz; float phase, speed; };
    std::vector<Star> stars;
    float selAnimY = -1.0f;        // eased focus-card position (borealis-style)

    // ── Boot animation (shown while a game loads) ───────────────────────
    // Native port of animation/"Viridite Boot Animation.dc.html": the gem from
    // viridite1.svg floating inside two counter-rotating arc rings, with a
    // white sweep clipped to the gem, sparkles, and a tracked-caps caption.
    SDL_Texture* gemTex    = nullptr;  // romfs:/viridite_gem.png (viridite1.svg)
    SDL_Texture* bootBgTex = nullptr;  // radial-gradient backdrop
    SDL_Texture* ringOutTex = nullptr; // outer arcs — spins forward
    SDL_Texture* ringInTex  = nullptr; // inner arc  — spins reverse
    SDL_Texture* gemFrame   = nullptr; // render target: gem + sweep, alpha-clipped
    SDL_Texture* sweepTex   = nullptr; // white gradient band
    TTF_Font*    fBootT = nullptr;     // 15px caps title
    TTF_Font*    fBootS = nullptr;     // 13px caps status
    TTF_Font*    fBootF = nullptr;     // 11px caps footer
    bool         bootReady   = false;
    std::string  launchTitle;          // game name shown under the gem
    bool         showLogPanel = false; // Y toggles the compat_log feed back on

    // One-shot README screenshot flags (each screen captured once per run)
    bool shotMenu = false, shotLoading = false, shotResult = false, shotAbout = false;

    // A game session leaves JIT regions and worker threads behind that we
    // can't fully unload yet — a second launch reads garbage ("not an ARM
    // binary"). Block relaunch until the app is restarted.
    bool   gameRanOnce = false;
    Uint32 noticeUntil = 0;
    std::string noticeText = "One game session per launch for now — restart Viridite to play again";

    // Save the composed frame (call just before SDL_RenderPresent) as a PNG in
    // sdmc:/Viridite/screenshots/ — showcase material for the README.
    void saveScreenshot(const char* name) {
        mkdir("sdmc:/Viridite/screenshots", 0777);
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(
            0, SW, SH, 32, SDL_PIXELFORMAT_ABGR8888);
        if (!s) return;
        if (SDL_RenderReadPixels(rdr, nullptr, s->format->format,
                                 s->pixels, s->pitch) == 0) {
            char path[128];
            snprintf(path, sizeof(path), "sdmc:/Viridite/screenshots/%s", name);
            if (IMG_SavePNG(s, path) == 0) logMsg(path);
        }
        SDL_FreeSurface(s);
    }

    // ------------------------------------------------------------------
    TTF_Font* openFont(int ptsize) {
        plInitialize(PlServiceType_User);
        PlFontData fd = {};
        if (plGetSharedFontByType(&fd, PlSharedFontType_Standard) == 0 && fd.size > 8) {
            SDL_RWops* rw = SDL_RWFromConstMem(
                (const uint8_t*)fd.address + 8, (int)fd.size - 8);
            TTF_Font* f = TTF_OpenFontRW(rw, 1, ptsize);
            if (f) { logMsg("  font: system BFTTF"); return f; }
            logSDL("  BFTTF open failed");
        }
        romfsInit();
        TTF_Font* f = TTF_OpenFont("romfs:/fonts/DejaVuSans.ttf", ptsize);
        if (f) { logMsg("  font: romfs DejaVuSans"); return f; }
        logSDL("  romfs font open failed");
        return nullptr;
    }

    // NintendoExt shared font: circled A/B/X/Y/+/- button glyphs (U+E0E0…).
    // Returns nullptr on failure — callers fall back to plain-text hints.
    TTF_Font* openExtFont(int ptsize) {
        PlFontData fd = {};
        if (plGetSharedFontByType(&fd, PlSharedFontType_NintendoExt) == 0 && fd.size > 8) {
            SDL_RWops* rw = SDL_RWFromConstMem(
                (const uint8_t*)fd.address + 8, (int)fd.size - 8);
            TTF_Font* f = TTF_OpenFontRW(rw, 1, ptsize);
            if (f) { logMsg("  font: NintendoExt glyphs"); return f; }
        }
        logMsg("  NintendoExt font unavailable — text hints");
        return nullptr;
    }

    // ------------------------------------------------------------------
    bool init() {
        mkdir("sdmc:/Viridite", 0777);
        logOpen();
        logMsg("Viridite starting");
        socketInitializeDefault();

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
            logSDL("SDL_Init failed"); logClose(); return false;
        }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP) == 0)
            logSDL("IMG_Init warning");
        if (TTF_Init() != 0) {
            logSDL("TTF_Init failed"); logClose(); return false;
        }

        win = SDL_CreateWindow("Viridite",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            SW, SH, SDL_WINDOW_SHOWN);
        if (!win) { logSDL("CreateWindow failed"); logClose(); return false; }

        rdr = SDL_CreateRenderer(win, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!rdr) {
            logSDL("Accelerated renderer failed, trying software");
            rdr = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!rdr) { logSDL("CreateRenderer failed"); logClose(); return false; }

        SDL_SetRenderDrawBlendMode(rdr, SDL_BLENDMODE_BLEND);

        fLg = openFont(28);
        fMd = openFont(20);
        fSm = openFont(17);
        if (!fLg || !fSm) { logMsg("Font load failed"); logClose(); return false; }
        if (!fMd) fMd = fSm;
        fBtn = openExtFont(22);

        buildBackground();

        if (SDL_NumJoysticks() > 0) {
            joy = SDL_JoystickOpen(0);
            if (!joy) logSDL("JoystickOpen warning");
        }
        logMsg("init complete");
        return true;
    }

    // ------------------------------------------------------------------
    void cleanup() {
        avatarStop();
        if (avatarTex) SDL_DestroyTexture(avatarTex);
        if (bgTex) SDL_DestroyTexture(bgTex);
        for (auto* t : icons) if (t) SDL_DestroyTexture(t);
        // Boot-animation assets
        if (gemTex)     SDL_DestroyTexture(gemTex);
        if (bootBgTex)  SDL_DestroyTexture(bootBgTex);
        if (ringOutTex) SDL_DestroyTexture(ringOutTex);
        if (ringInTex)  SDL_DestroyTexture(ringInTex);
        if (gemFrame)   SDL_DestroyTexture(gemFrame);
        if (sweepTex)   SDL_DestroyTexture(sweepTex);
        if (fBootT && fBootT != fSm) TTF_CloseFont(fBootT);
        if (fBootS && fBootS != fSm) TTF_CloseFont(fBootS);
        if (fBootF && fBootF != fSm) TTF_CloseFont(fBootF);
        if (fBtn) TTF_CloseFont(fBtn);
        if (fLg)  TTF_CloseFont(fLg);
        if (fMd && fMd != fSm) TTF_CloseFont(fMd);
        if (fSm)  TTF_CloseFont(fSm);
        if (joy)  SDL_JoystickClose(joy);
        if (rdr)  SDL_DestroyRenderer(rdr);
        if (win)  SDL_DestroyWindow(win);
        socketExit();
        romfsExit(); plExit();
        TTF_Quit(); IMG_Quit(); SDL_Quit();
        logMsg("cleanup done");
        logClose();
    }

    // ------------------------------------------------------------------
    void fill(int x, int y, int w, int h, SDL_Color c) {
        SDL_SetRenderDrawColor(rdr, c.r, c.g, c.b, c.a);
        SDL_Rect r = {x, y, w, h};
        SDL_RenderFillRect(rdr, &r);
    }

    int drawText(TTF_Font* f, const std::string& s, SDL_Color col, int x, int y) {
        if (s.empty() || !f) return 0;
        SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), col);
        if (!surf) return 0;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(rdr, surf);
        int w = surf->w;
        SDL_FreeSurface(surf);
        if (!tex) return 0;
        int tw, th;
        SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
        SDL_Rect dst = {x, y, tw, th};
        SDL_RenderCopy(rdr, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
        return w;
    }

    // ── Scenery ─────────────────────────────────────────────────────────
    // The icon look: deep-space gradient sky, twinkling stars, and a green
    // planet horizon rising from the bottom edge with a glowing rim.
    // Sky + planet are static → rendered once into bgTex; stars animate live.

    static SDL_Color lerpCol(SDL_Color a, SDL_Color b, float t) {
        return { (Uint8)(a.r + (b.r - a.r) * t), (Uint8)(a.g + (b.g - a.g) * t),
                 (Uint8)(a.b + (b.b - a.b) * t), 255 };
    }

    void fillCircle(int cx, int cy, int r, SDL_Color c) {
        SDL_SetRenderDrawColor(rdr, c.r, c.g, c.b, c.a);
        for (int dy = -r; dy <= r; dy++) {
            int hw = (int)sqrtf((float)(r * r - dy * dy));
            SDL_Rect row = {cx - hw, cy + dy, hw * 2, 1};
            SDL_RenderFillRect(rdr, &row);
        }
    }

    static constexpr float PLANET_R    = 2200.0f;  // huge circle → gentle curve
    static constexpr int   PLANET_BUMP = 130;      // rim height above bottom edge

    void buildBackground() {
        // Light theme: a scatter of faint green motes on the near-white ground
        // (no dark sky/planet SVG). bgTex stays null so drawBackground fills C_BG.
        stars.clear();
        uint32_t rng = 0x5EED5EED;
        auto rnd = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        for (int i = 0; i < 60; i++) {
            Star s;
            s.x     = (float)(rnd() % SW);
            s.y     = (float)(rnd() % SH);
            s.sz    = (rnd() % 100 < 22) ? 3 : 2;
            s.phase = (rnd() % 628) / 100.0f;
            s.speed = 0.25f + (rnd() % 100) / 120.0f;
            stars.push_back(s);
        }
    }

    void drawBackground() {
        Uint32 now = SDL_GetTicks();
        fill(0, 0, SW, SH, C_BG);
        for (auto& s : stars) {
            s.x -= 0.02f * s.speed;
            if (s.x < 0) s.x += SW;
            float tw = 0.5f + 0.5f * sinf(now / 1000.0f * s.speed * 6.2832f + s.phase);
            Uint8 a  = (Uint8)(12 + 24 * tw);   // faint green motes on white
            fill((int)s.x, (int)s.y, s.sz, s.sz, {0, 190, 110, a});
        }
    }

    // Shared translucent header: "Viridite" with green accent + rim line
    void drawHeaderBar(const std::string& rightText = "") {
        fill(0, 0, SW, HEADER_H, {255, 255, 255, 235});
        fill(0, HEADER_H - 3, SW, 3, C_RIM);
        int w = drawText(fLg, "Virid", C_WHITE, 30, (HEADER_H - 28) / 2);
        w += drawText(fLg, "ite", C_OK, 30 + w, (HEADER_H - 28) / 2);
        drawText(fSm, BUILD_VERSION, C_DIM, 30 + w + 14, (HEADER_H + 4) / 2);
        if (!rightText.empty()) {
            int tw = 0, th = 0;
            TTF_SizeUTF8(fSm, rightText.c_str(), &tw, &th);
            drawText(fSm, rightText, C_DIM, SW - tw - 30, (HEADER_H - 18) / 2);
        }
    }

    // HOS-style footer hints, right-aligned: {glyph-utf8-or-letter, label}.
    // With the NintendoExt font the glyph IS the circled button; otherwise we
    // draw our own chip with the letter.
    void drawFooterBar(const std::vector<std::pair<std::string, std::string>>& hints,
                       const std::string& leftText = "") {
        fill(0, SH - FOOTER_H, SW, FOOTER_H, {242, 248, 245, 235});
        fill(0, SH - FOOTER_H, SW, 2, C_RIM);
        int cy = SH - FOOTER_H / 2;
        if (!leftText.empty())
            drawText(fSm, leftText, C_WARN, 30, cy - 9);
        int x = SW - 30;
        for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
            int lw = 0, lh = 0;
            TTF_SizeUTF8(fSm, it->second.c_str(), &lw, &lh);
            x -= lw;
            drawText(fSm, it->second, C_GRAY, x, cy - lh / 2);
            x -= 8;
            if (fBtn && it->first.size() > 1) {   // real HOS glyph
                int gw = 0, gh = 0;
                TTF_SizeUTF8(fBtn, it->first.c_str(), &gw, &gh);
                x -= gw;
                drawText(fBtn, it->first, C_WHITE, x, cy - gh / 2);
            } else {                              // fallback chip
                x -= 26;
                fillCircle(x + 13, cy, 13, {205, 244, 224, 255});
                std::string letter = it->first.size() > 1 ? "?" : it->first;
                int gw = 0, gh = 0;
                TTF_SizeUTF8(fSm, letter.c_str(), &gw, &gh);
                drawText(fSm, letter, C_WHITE, x + 13 - gw / 2, cy - gh / 2);
            }
            x -= 34;
        }
    }

    // HOS button glyphs in the NintendoExt shared font
    static constexpr const char* GLYPH_A     = "\xEE\x83\xA0";  // U+E0E0
    static constexpr const char* GLYPH_B     = "\xEE\x83\xA1";  // U+E0E1
    static constexpr const char* GLYPH_X     = "\xEE\x83\xA2";  // U+E0E2
    static constexpr const char* GLYPH_Y     = "\xEE\x83\xA3";  // U+E0E3
    static constexpr const char* GLYPH_PLUS  = "\xEE\x83\xAF";  // U+E0EF (+)
    static constexpr const char* GLYPH_MINUS = "\xEE\x83\xB0";  // U+E0F0 (-)

    // Pick the HOS glyph when the ext font loaded, else the plain letter
    // (drawFooterBar renders single-char hints as its own chip).
    std::string BG(const char* glyph, const char* letter) const {
        return fBtn ? glyph : letter;
    }

    static std::string formatSize(uint64_t bytes) {
        char buf[32];
        if (bytes >= 1024ull * 1024 * 1024)
            snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
        else if (bytes >= 1024ull * 1024)
            snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
        else if (bytes >= 1024ull)
            snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
        else
            snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
        return buf;
    }

    void drawMonogram(const std::string& name, int x, int y, int sz) {
        static const SDL_Color PALETTE[] = {
            {239, 83,  80,  255}, {171, 71,  188, 255}, {66,  165, 245, 255},
            {38,  166, 154, 255}, {255, 167, 38,  255}, {126, 87,  194, 255},
            {92,  107, 192, 255}, {255, 112, 67,  255},
        };
        uint32_t h = 2166136261u;
        for (char c : name) h = (h ^ (uint8_t)c) * 16777619u;
        SDL_Color bg = PALETTE[h % (sizeof(PALETTE) / sizeof(PALETTE[0]))];
        fill(x, y, sz, sz, bg);
        char letter = name.empty() ? '?' : (char)toupper((unsigned char)name[0]);
        std::string s(1, letter);
        int w = 0, h2 = 0;
        TTF_SizeUTF8(fLg, s.c_str(), &w, &h2);
        drawText(fLg, s, C_WHITE, x + (sz - w) / 2, y + (sz - h2) / 2);
    }

    std::string clamp(TTF_Font* f, const std::string& s, int maxW) {
        int w = 0, h = 0;
        TTF_SizeUTF8(f, s.c_str(), &w, &h);
        if (w <= maxW) return s;
        std::string t = s;
        while (!t.empty()) {
            t.pop_back();
            std::string try_ = t + "...";
            TTF_SizeUTF8(f, try_.c_str(), &w, &h);
            if (w <= maxW) return try_;
        }
        return "...";
    }

    // ------------------------------------------------------------------
    void loadIcons() {
        icons.assign(apks.size(), nullptr);
        for (size_t i = 0; i < apks.size(); i++) {
            if (apks[i].iconPng.empty()) continue;
            SDL_RWops* rw = SDL_RWFromConstMem(
                apks[i].iconPng.data(), (int)apks[i].iconPng.size());
            SDL_Surface* surf = IMG_Load_RW(rw, 1);
            if (!surf) continue;
            icons[i] = SDL_CreateTextureFromSurface(rdr, surf);
            SDL_FreeSurface(surf);
            apks[i].iconPng.clear();
        }
    }

    void rescan() {
        for (auto* t : icons) if (t) SDL_DestroyTexture(t);
        icons.clear();
        apks = ::scanApks(APK_DIR);
        loadIcons();
        selected = 0; scroll = 0;
    }

    // ------------------------------------------------------------------
    void render() {
        Uint32 now = SDL_GetTicks();
        drawBackground();

        if (apks.empty()) {
            drawText(fSm,
                "No APKs found — place .apk files in sdmc:/Viridite/apks/",
                C_GRAY, 30, LIST_Y + 30);
        } else {
            // Focus card (borealis-style): eased position + pulsing green glow,
            // drawn under the row content.
            int targetY = LIST_Y + (selected - scroll) * ITEM_H;
            if (selAnimY < 0) selAnimY = (float)targetY;
            selAnimY += (targetY - selAnimY) * 0.35f;
            if (fabsf(selAnimY - targetY) < 0.5f) selAnimY = (float)targetY;
            {
                int cy2 = (int)selAnimY;
                float pulse = 0.5f + 0.5f * sinf(now / 1000.0f * 2.6f);
                SDL_Rect card = {12, cy2 + 4, SW - 24, ITEM_H - 8};
                fill(card.x, card.y, card.w, card.h, {205, 244, 224, 235});
                for (int g = 1; g <= 5; g++) {       // soft outer glow
                    Uint8 a = (Uint8)((60 - g * 10) * (0.55f + 0.45f * pulse));
                    SDL_SetRenderDrawColor(rdr, 0, 200, 100, a);
                    SDL_Rect gr = {card.x - g, card.y - g,
                                   card.w + 2 * g, card.h + 2 * g};
                    SDL_RenderDrawRect(rdr, &gr);
                }
                SDL_SetRenderDrawColor(rdr, 0, 200, 100,
                                       (Uint8)(160 + 95 * pulse));
                SDL_RenderDrawRect(rdr, &card);      // crisp focus border
                fill(card.x, card.y, 5, card.h, C_RIM);
            }

            int end = std::min((int)apks.size(), scroll + VISIBLE);
            for (int i = scroll; i < end; i++) {
                int iy = LIST_Y + (i - scroll) * ITEM_H;
                SDL_SetRenderDrawColor(rdr, C_DIV.r, C_DIV.g, C_DIV.b, 130);
                SDL_RenderDrawLine(rdr, 24, iy + ITEM_H - 1, SW - 24, iy + ITEM_H - 1);

                int iconY = iy + (ITEM_H - ICON_SZ) / 2;
                if (i < (int)icons.size() && icons[i]) {
                    SDL_Rect dst = {28, iconY, ICON_SZ, ICON_SZ};
                    SDL_RenderCopy(rdr, icons[i], nullptr, &dst);
                } else {
                    drawMonogram(apks[i].appName, 28, iconY, ICON_SZ);
                }

                int tx   = 28 + ICON_SZ + 16;
                int maxW = SW - tx - 40;
                drawText(fLg, clamp(fLg, apks[i].appName, maxW), C_WHITE, tx, iy + 14);

                if (apks[i].installed) {
                    static const std::string INST = "INSTALLED";
                    int bw = 0, bh = 0;
                    TTF_SizeUTF8(fSm, INST.c_str(), &bw, &bh);
                    int bx = SW - bw - 40;
                    fill(bx - 6, iy + 14, bw + 12, bh, {205, 244, 224, 220});
                    drawText(fSm, INST, C_INST, bx, iy + 14);
                }

                std::string pkgLine =
                    (apks[i].packageName.empty() ? apks[i].filename : apks[i].packageName);
                if (!apks[i].versionName.empty())
                    pkgLine += "  v" + apks[i].versionName;
                if (apks[i].fileSizeBytes > 0)
                    pkgLine += "  ·  " + formatSize(apks[i].fileSizeBytes);
                drawText(fSm, clamp(fSm, pkgLine, maxW), C_GRAY, tx, iy + 58);
            }
            if ((int)apks.size() > VISIBLE) {
                int barH = LIST_H * VISIBLE / (int)apks.size();
                int barY = LIST_Y + LIST_H * scroll / (int)apks.size();
                fill(SW - 6, barY, 6, barH, {150, 195, 172, 200});
            }
        }

        std::string cnt;
        if (!apks.empty())
            cnt = std::to_string(apks.size()) + (apks.size() == 1 ? " APK" : " APKs");
        drawHeaderBar(cnt);

        if (noticeUntil && now < noticeUntil) {
            const char* msg = noticeText.c_str();
            int w = 0, h = 0;
            TTF_SizeUTF8(fSm, msg, &w, &h);
            fill((SW - w) / 2 - 16, SH - FOOTER_H - 44, w + 32, 34, {253, 235, 238, 240});
            drawText(fSm, msg, C_WARN, (SW - w) / 2, SH - FOOTER_H - 36);
        }

        bool docked = appletGetOperationMode() == AppletOperationMode_Console;
        drawFooterBar({{BG(GLYPH_A, "A"), "Launch"}, {BG(GLYPH_X, "X"), "Reinstall"},
                       {BG(GLYPH_Y, "Y"), "Rescan"}, {BG(GLYPH_MINUS, "-"), "About"},
                       {BG(GLYPH_PLUS, "+"), "Quit"}},
                      docked ? "Docked — games need handheld (touch screen)" : "");

        if (!shotMenu && !apks.empty() && now > 3000) {  // icons + glow settled
            shotMenu = true;
            saveScreenshot("ui_menu.png");
        }
        SDL_RenderPresent(rdr);
    }

    // ------------------------------------------------------------------
    // Snapshot the last N lines from the in-memory detail ring buffer.
    // The detail buffer is written by every compatLog() call on the loader
    // thread — no file I/O, always fresh, works during silent phases too.
    // ------------------------------------------------------------------
    void snapDetailLog(std::vector<std::string>& out, int maxLines) {
        int head = g_detail_head;  // sample once
        int total = head < DETLOG_N ? head : DETLOG_N;
        int show  = total < maxLines ? total : maxLines;
        out.clear();
        out.reserve(show);
        for (int i = show - 1; i >= 0; i--) {
            int slot = ((head - 1 - i) % DETLOG_N + DETLOG_N) % DETLOG_N;
            if (g_detail_log[slot][0])
                out.push_back(std::string(g_detail_log[slot]));
        }
    }
    static const int DETLOG_N = 28;

    // ── Boot-animation assets ───────────────────────────────────────────
    // Colours lifted straight from the .dc.html design.
    static SDL_Color bootAccent()  { return {0,   168, 84,  255}; }  // #00A854
    static SDL_Color bootTitle()   { return {0,   117, 63,  255}; }  // #00753F
    static SDL_Color bootDot()     { return {105, 240, 174, 255}; }  // #69F0AE

    // radial-gradient(ellipse 70% 60% at 50% 42%, #FFF, #F2FBF6 55%, #E4F5EC)
    SDL_Texture* makeRadialBg() {
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, SW, SH, 32, SDL_PIXELFORMAT_RGBA32);
        if (!s) return nullptr;
        const SDL_Color c0 = {255,255,255,255}, c1 = {242,251,246,255}, c2 = {228,245,236,255};
        const float cx = SW * 0.50f, cy = SH * 0.42f, rx = SW * 0.70f, ry = SH * 0.60f;
        for (int y = 0; y < SH; y++) {
            Uint32* row = (Uint32*)((Uint8*)s->pixels + y * s->pitch);
            for (int x = 0; x < SW; x++) {
                float nx = (x + 0.5f - cx) / rx, ny = (y + 0.5f - cy) / ry;
                float d  = sqrtf(nx * nx + ny * ny);
                if (d > 1.0f) d = 1.0f;
                SDL_Color c = (d < 0.55f) ? lerpCol(c0, c1, d / 0.55f)
                                          : lerpCol(c1, c2, (d - 0.55f) / 0.45f);
                row[x] = SDL_MapRGBA(s->format, c.r, c.g, c.b, 255);
            }
        }
        SDL_Texture* t = SDL_CreateTextureFromSurface(rdr, s);
        SDL_FreeSurface(s);
        return t;
    }

    // An anti-aliased ring carrying one or more coloured arcs. Angles are in
    // degrees, 0 = right and rising counter-clockwise, matching how CSS lays a
    // border-<side>-color onto a circle (top = 45°..135°, right = 315°..45°).
    struct Arc { float a0, a1; SDL_Color c; };
    SDL_Texture* makeArcRing(int size, float radius, float thick,
                             const std::vector<Arc>& arcs) {
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
        if (!s) return nullptr;
        SDL_memset(s->pixels, 0, (size_t)s->h * s->pitch);
        const float cx = size * 0.5f, cy = size * 0.5f;
        for (int y = 0; y < size; y++) {
            Uint32* row = (Uint32*)((Uint8*)s->pixels + y * s->pitch);
            for (int x = 0; x < size; x++) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                float r  = sqrtf(dx * dx + dy * dy);
                float cov = thick * 0.5f - fabsf(r - radius) + 0.5f;  // AA coverage
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;
                float ang = atan2f(-dy, dx) * 180.0f / PI_F;
                if (ang < 0.0f) ang += 360.0f;
                for (const Arc& a : arcs) {
                    bool in = (a.a0 <= a.a1) ? (ang >= a.a0 && ang <= a.a1)
                                             : (ang >= a.a0 || ang <= a.a1);  // wraps 0°
                    if (!in) continue;
                    row[x] = SDL_MapRGBA(s->format, a.c.r, a.c.g, a.c.b,
                                         (Uint8)(a.c.a * cov));
                    break;
                }
            }
        }
        SDL_Texture* t = SDL_CreateTextureFromSurface(rdr, s);
        SDL_FreeSurface(s);
        return t;
    }

    // Horizontal transparent→white→transparent band (the gem's shine sweep).
    SDL_Texture* makeSweep(int w, int h) {
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
        if (!s) return nullptr;
        for (int x = 0; x < w; x++) {
            float t = w > 1 ? x / (float)(w - 1) : 0.0f;
            float a = (t < 0.5f ? t / 0.5f : (1.0f - t) / 0.5f) * 0.75f;
            Uint32 px = SDL_MapRGBA(s->format, 255, 255, 255, (Uint8)(a * 255));
            for (int y = 0; y < h; y++)
                ((Uint32*)((Uint8*)s->pixels + y * s->pitch))[x] = px;
        }
        SDL_Texture* t = SDL_CreateTextureFromSurface(rdr, s);
        SDL_FreeSurface(s);
        if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_ADD);
        return t;
    }

    void buildBootAssets() {
        if (bootReady) return;
        bootReady = true;
        romfsInit();
        SDL_Surface* g = IMG_Load("romfs:/viridite_gem.png");
        if (g) { gemTex = SDL_CreateTextureFromSurface(rdr, g); SDL_FreeSurface(g); }
        else   { logSDL("boot: gem png load failed"); }

        bootBgTex = makeRadialBg();
        // Outer: strong top arc + faint right arc. Inner: soft bottom arc.
        ringOutTex = makeArcRing(RING_TEX, 165.0f, 2.0f,
                                 {{45.0f, 135.0f, {0, 168, 84, 217}},
                                  {315.0f, 45.0f, {0, 168, 84, 38}}});
        ringInTex  = makeArcRing(RING_TEX, 151.0f, 1.5f,
                                 {{225.0f, 315.0f, {0, 200, 83, 89}}});
        sweepTex   = makeSweep(57, GEM_PX);
        gemFrame   = SDL_CreateTexture(rdr, SDL_PIXELFORMAT_RGBA8888,
                                       SDL_TEXTUREACCESS_TARGET, GEM_PX, GEM_PX);
        if (gemFrame) SDL_SetTextureBlendMode(gemFrame, SDL_BLENDMODE_BLEND);

        fBootT = openFont(15);
        fBootS = openFont(13);
        fBootF = openFont(11);
        if (!fBootT) fBootT = fSm;
        if (!fBootS) fBootS = fSm;
        if (!fBootF) fBootF = fSm;
    }

    // ── Tracked caps text (CSS letter-spacing) ──────────────────────────
    // Splits on UTF-8 boundaries so non-ASCII game titles don't get mangled.
    static std::vector<std::string> utf8Glyphs(const std::string& s) {
        std::vector<std::string> out;
        for (size_t i = 0; i < s.size();) {
            unsigned char c = (unsigned char)s[i];
            size_t n = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
            if (i + n > s.size()) n = 1;
            out.push_back(s.substr(i, n));
            i += n;
        }
        return out;
    }
    int trackedWidth(TTF_Font* f, const std::string& s, int track) {
        if (!f) return 0;
        auto gl = utf8Glyphs(s);
        int total = 0;
        for (auto& g : gl) { int w = 0, h = 0; TTF_SizeUTF8(f, g.c_str(), &w, &h); total += w + track; }
        return total > 0 ? total - track : 0;
    }
    void drawTrackedCentered(TTF_Font* f, const std::string& s, SDL_Color col,
                             int cx, int y, int track) {
        if (!f || s.empty()) return;
        int x = cx - trackedWidth(f, s, track) / 2;
        for (auto& g : utf8Glyphs(s)) {
            if (g == " ") { int w = 0, h = 0; TTF_SizeUTF8(f, " ", &w, &h); x += w + track; continue; }
            x += drawText(f, g, col, x, y) + track;
        }
    }
    static std::string upperAscii(std::string s) {
        for (char& c : s) c = (char)toupper((unsigned char)c);
        return s;
    }

    // Layout, transcribed from the design's centred flex column.
    static constexpr int GEM_PX   = 240;   // gem render size
    static constexpr int RING_TEX = 340;   // ring texture (340×340 box)
    static constexpr int BLOCK_Y  = 134;   // top of the gem box
    static constexpr int GEM_CY   = BLOCK_Y + 170;
    static constexpr int TITLE_Y  = BLOCK_Y + 340 + 36;
    static constexpr int STAT_Y   = TITLE_Y + 37;
    static constexpr int BAR_Y    = STAT_Y + 35;

    // ------------------------------------------------------------------
    // Progress screen — fully animated, called at ~60fps from main thread.
    // Reads shared state written by the loader thread (g_ui_stage, g_ui_pct).
    // Y toggles the old compat_log.txt feed back on for debugging.
    // ------------------------------------------------------------------
    // The old developer view: live compat_log.txt feed. The boot animation
    // replaced it as the default, but a log-driven project shouldn't lose the
    // ability to watch a stall happen — Y brings it back over the animation.
    void drawLogPanel() {
        const int LH = 21, N_SHOW = 13;
        const int BOX_X = 30, BOX_W = SW - 60;
        const int BOX_H = LH * (N_SHOW + 1) + 14;
        const int y0 = SH - FOOTER_H - BOX_H - 16;

        fill(BOX_X, y0, BOX_W, BOX_H, {247, 251, 249, 245});
        fill(BOX_X, y0, BOX_W, LH + 4, {237, 244, 240, 250});
        drawText(fSm, "  compat_log.txt", {120, 140, 225, 255}, BOX_X + 8, y0 + 3);
        drawText(fSm, "Y: hide", C_DIM, BOX_X + BOX_W - 70, y0 + 3);

        std::vector<std::string> logLines;
        snapDetailLog(logLines, N_SHOW);

        const SDL_Color C_LOG     = {125, 150, 230, 255};
        const SDL_Color C_LOG_NEW = {40,  60,  90,  255};
        const SDL_Color C_LOG_WARN= {176, 120, 0,   255};
        const SDL_Color C_LOG_ERR = {214, 48,  79,  255};

        int startIdx = (int)logLines.size() > N_SHOW ? (int)logLines.size() - N_SHOW : 0;
        int liy = y0 + LH + 8;
        for (int i = startIdx; i < (int)logLines.size(); i++) {
            bool isLast = (i == (int)logLines.size() - 1);
            const std::string& ln = logLines[i];
            SDL_Color c = isLast ? C_LOG_NEW : C_LOG;
            if (!isLast) {
                if (ln.find("FAULT") != std::string::npos ||
                    ln.find("fail")  != std::string::npos ||
                    ln.find("ERR")   != std::string::npos)
                    c = C_LOG_ERR;
                else if (ln.find("WARN") != std::string::npos ||
                         ln.find("warn") != std::string::npos)
                    c = C_LOG_WARN;
            }
            drawText(fMd ? fMd : fSm,
                     clamp(fMd ? fMd : fSm, (isLast ? "> " : "  ") + ln, BOX_W - 24),
                     c, BOX_X + 10, liy);
            liy += LH;
        }
    }

    // ------------------------------------------------------------------
    // Progress screen — the Viridite boot animation, called at ~60fps from the
    // main thread. Reads shared state written by the loader thread (g_ui_stage,
    // g_ui_pct). Y toggles the compat_log.txt feed back on for debugging.
    // ------------------------------------------------------------------
    void showProgress() {
        Uint32 now = SDL_GetTicks();
        buildBootAssets();

        // Track elapsed time per stage
        static char   s_stage[80] = {};
        static Uint32 s_stage_t   = 0;
        if (strncmp(g_ui_stage, s_stage, sizeof(s_stage)) != 0) {
            memcpy(s_stage, g_ui_stage, sizeof(s_stage));
            s_stage[sizeof(s_stage) - 1] = '\0';
            s_stage_t = now;
        }
        Uint32 elapsed_s = (now - s_stage_t) / 1000;

        // ── Backdrop ──
        if (bootBgTex) SDL_RenderCopy(rdr, bootBgTex, nullptr, nullptr);
        else           fill(0, 0, SW, SH, C_BG);

        // ── Counter-rotating rings (ringSpin 6s linear) ──
        float spin = (now % 6000) / 6000.0f * 360.0f;
        SDL_Rect ringDst = {SW / 2 - RING_TEX / 2, GEM_CY - RING_TEX / 2, RING_TEX, RING_TEX};
        if (ringOutTex) SDL_RenderCopyEx(rdr, ringOutTex, nullptr, &ringDst,  spin, nullptr, SDL_FLIP_NONE);
        if (ringInTex)  SDL_RenderCopyEx(rdr, ringInTex,  nullptr, &ringDst, -spin, nullptr, SDL_FLIP_NONE);

        // ── Sparkles (0%/100% hidden, 50% full) ──
        struct Spark { int cx, cy, r; SDL_Color c; float period, delay; };
        static const Spark SPARKS[] = {
            {754, 166, 4, {185, 246, 202, 255}, 2200.0f, 0.0f},
            {511, 407, 3, {105, 240, 174, 255}, 2800.0f, 900.0f},
            {496, 232, 2, {0,   200, 83,  255}, 3100.0f, 1600.0f},
        };
        for (const Spark& s : SPARKS) {
            float ph = fmodf((float)now + s.period - s.delay, s.period) / s.period;
            float k  = 0.5f - 0.5f * cosf(ph * 2.0f * PI_F);
            SDL_Color c = s.c; c.a = (Uint8)(255.0f * k);
            fillCircle(s.cx, s.cy, (int)(s.r * (0.4f + 0.6f * k) + 0.5f), c);
        }

        // ── Gem: float (gemFloat 3.4s) + shine sweep clipped to its silhouette ──
        float fph   = (now % 3400) / 3400.0f;
        float fk    = 0.5f - 0.5f * cosf(fph * 2.0f * PI_F);
        float lift  = -14.0f * fk;
        float scale = 1.0f + 0.015f * fk;
        if (gemTex && gemFrame) {
            SDL_Texture* prev = SDL_GetRenderTarget(rdr);
            SDL_SetRenderTarget(rdr, gemFrame);
            SDL_SetRenderDrawColor(rdr, 0, 0, 0, 0);
            SDL_RenderClear(rdr);
            SDL_RenderCopy(rdr, gemTex, nullptr, nullptr);
            // ADD leaves destination alpha untouched, so the band only lights up
            // pixels the gem already covers — the render-target equivalent of the
            // design's SVG clipPath.
            if (sweepTex) {
                float t = (now % 2800) / 2800.0f;
                float p = t / 0.55f; if (p > 1.0f) p = 1.0f;
                SDL_Rect sd = {(int)(-221.0f + p * 541.0f), 0, 57, GEM_PX};
                SDL_RenderCopyEx(rdr, sweepTex, nullptr, &sd, -18.0, nullptr, SDL_FLIP_NONE);
            }
            SDL_SetRenderTarget(rdr, prev);
            int gw = (int)(GEM_PX * scale + 0.5f);
            SDL_Rect gd = {SW / 2 - gw / 2, (int)(GEM_CY - gw / 2 + lift + 0.5f), gw, gw};
            SDL_RenderCopy(rdr, gemFrame, nullptr, &gd);
        }

        // ── Caption: game title over stage text + blinking dots ──
        drawTrackedCentered(fBootT,
                            launchTitle.empty() ? "NOW LOADING" : upperAscii(launchTitle),
                            bootTitle(), SW / 2, TITLE_Y, 6);

        std::string status = g_ui_stage[0] ? upperAscii(g_ui_stage) : "READING GAME DATA";
        if (elapsed_s >= 30) status += " — STILL WORKING";
        const SDL_Color statCol = {0, 102, 51, 140};
        int stW   = trackedWidth(fBootS, status, 4);
        int dotsW = 3 * 5 + 2 * 5;
        int rowX  = SW / 2 - (stW + 10 + dotsW) / 2;
        drawTrackedCentered(fBootS, status, statCol, rowX + stW / 2, STAT_Y, 4);
        for (int i = 0; i < 3; i++) {
            float ph = fmodf((float)now + 1400.0f - i * 200.0f, 1400.0f) / 1400.0f;
            float u  = ph < 0.4f ? ph / 0.4f : (ph < 0.8f ? (0.8f - ph) / 0.4f : 0.0f);
            SDL_Color c = bootDot(); c.a = (Uint8)(255.0f * (0.25f + 0.75f * u));
            fillCircle(rowX + stW + 12 + i * 10, STAT_Y + 8, 2, c);
        }

        // ── Progress bar (280×3) — real percentage when the loader reports one,
        //    otherwise the design's indeterminate slider.
        const int BW = 280, BX = SW / 2 - BW / 2;
        const SDL_Color BAR_A = {0, 200, 83, 255}, BAR_B = {0, 117, 63, 255};
        fill(BX, BAR_Y, BW, 3, {0, 168, 84, 38});
        if (g_ui_pct > 0) {
            int pct = g_ui_pct > 100 ? 100 : g_ui_pct;
            int fw  = BW * pct / 100;
            for (int i = 0; i < fw; i++)
                fill(BX + i, BAR_Y, 1, 3, lerpCol(BAR_A, BAR_B, i / (float)BW));
        } else {
            const int SLW = (int)(BW * 0.35f);
            int sx = (int)((now % 1600) / 1600.0f * (BW + SLW)) - SLW;
            int x0 = std::max(BX, BX + sx), x1 = std::min(BX + BW, BX + sx + SLW);
            for (int i = x0; i < x1; i++)
                fill(i, BAR_Y, 1, 3, lerpCol(BAR_A, BAR_B, (i - x0) / (float)SLW));
        }

        // ── Footer wordmark ──
        drawTrackedCentered(fBootF, "VIRIDITE", {0, 102, 51, 77}, SW / 2, SH - 48, 3);

        if (showLogPanel) drawLogPanel();

        if (!shotLoading && g_ui_pct >= 40) {  // mid-load, gem in frame
            shotLoading = true;
            saveScreenshot("ui_loading.png");
        }
        SDL_RenderPresent(rdr);
    }

    // ------------------------------------------------------------------
    // Run launchApk on a background thread while this method drives the
    // animated progress screen on the main thread at ~60fps.
    // ------------------------------------------------------------------
    LaunchResult runLaunch(const ApkInfo& apk, bool skipInstall) {
        std::string pkg = apk.packageName.empty() ? apk.filename : apk.packageName;

        // Detail log is in-memory — no cache to invalidate, always fresh

        // The boot animation captions itself with the game being loaded
        launchTitle = !apk.appName.empty() ? apk.appName
                    : (!apk.packageName.empty() ? apk.packageName : apk.filename);

        // Set initial stage before thread starts so first frame looks right
        const char* verb = skipInstall ? "Launching (cached)" : "Installing + Launching";
        strncpy(g_ui_stage, verb, sizeof(g_ui_stage) - 1);

        LoaderCtx ctx;
        ctx.apk_path    = apk.path;
        ctx.pkg_name    = pkg;
        ctx.skip_install = skipInstall;
        g_loader_ctx    = &ctx;

        // The loader thread is pure CPU work (APK unzip, ELF relocation, JIT
        // dual-mapping) with nothing meaningful on screen but our own idle
        // progress UI — no GPU work competing for clocks. Real Switch titles
        // use this exact FastLoad boost mode during their own load screens
        // for the same reason. Reset back to Normal before the game (which
        // *does* need real GPU clocks) ever gets control.
        appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);

        Thread t;
        threadCreate(&t, loaderThreadFn, nullptr, nullptr,
                     0x100000 /*1MB stack*/, 0x2C, 1 /*CPU 1*/);
        threadStart(&t);

        // Main thread render loop — keeps the UI alive until the loader finishes
        bool quitting = false;
        while (!ctx.done.load(std::memory_order_acquire) && !quitting) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) quitting = true;
                if (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == BTN_PLUS) quitting = true;
                // Y peeks at the live compat_log.txt feed behind the animation
                if (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == BTN_Y)
                    showLogPanel = !showLogPanel;
            }
            showProgress();
            SDL_Delay(16); // ~60fps
        }

        // Render a final frame so the last log line is visible
        showProgress();

        threadWaitForExit(&t);
        threadClose(&t);
        g_loader_ctx = nullptr;

        // FastLoad throttles the GPU to its minimum clock — fine while we're
        // just drawing a static progress bar, disastrous once real gameplay
        // starts. Always drop back to Normal here, even if loading failed.
        appletSetCpuBoostMode(ApmCpuBoostMode_Normal);

        // If game loaded OK, run it here on the main thread (SDL2's EGL context
        // is current on this thread, so GL calls reach the screen).
        if (ctx.result.game_so) {
            std::string base_dir = std::string("sdmc:/Viridite/games/") + pkg;
            runGameOnMainThread(ctx.result.game_so, win, ctx.apk_path, base_dir);
            gameRanOnce = true;
        }

        return ctx.result;
    }

    // ------------------------------------------------------------------
    void showLaunchResult(const LaunchResult& res, int idx) {
        if (idx < 0 || idx >= (int)apks.size()) return;
        const ApkInfo& apk = apks[idx];

        bool done = false;
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { done = true; }
                if (ev.type == SDL_JOYBUTTONDOWN &&
                    (ev.jbutton.button == BTN_B || ev.jbutton.button == BTN_PLUS)) { done = true; }
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) { done = true; }
            }

            drawBackground();
            drawHeaderBar();

            // Translucent result panel so text reads over the starfield
            fill(40, LIST_Y + 6, SW - 80, SH - LIST_Y - FOOTER_H - 12, {247, 251, 249, 220});
            {
                SDL_Rect panel = {40, LIST_Y + 6, SW - 80, SH - LIST_Y - FOOTER_H - 12};
                SDL_SetRenderDrawColor(rdr, res.ok ? 52 : 235, res.ok ? 230 : 90,
                                       res.ok ? 134 : 90, 200);
                SDL_RenderDrawRect(rdr, &panel);
            }

            int iconSz = 112;
            if (idx < (int)icons.size() && icons[idx]) {
                SDL_Rect dst = {(SW - iconSz) / 2, LIST_Y + 16, iconSz, iconSz};
                SDL_RenderCopy(rdr, icons[idx], nullptr, &dst);
            } else {
                drawMonogram(apk.appName, (SW - iconSz) / 2, LIST_Y + 16, iconSz);
            }

            int nameY = LIST_Y + 16 + iconSz + 12;
            {
                int w = 0, h = 0;
                std::string nm = clamp(fLg, apk.appName, SW - 80);
                TTF_SizeUTF8(fLg, nm.c_str(), &w, &h);
                drawText(fLg, nm, C_WHITE, (SW - w) / 2, nameY);
            }

            std::string statusStr = res.ok ? "Launch OK" : "Launch Failed";
            SDL_Color   statusCol = res.ok ? C_OK : C_ERR;
            {
                int w = 0, h = 0;
                TTF_SizeUTF8(fLg, statusStr.c_str(), &w, &h);
                drawText(fLg, statusStr, statusCol, (SW - w) / 2, nameY + 44);
            }

            int y = nameY + 100;
            if (!res.ok) {
                if (!res.errorStage.empty())
                    { drawText(fSm, "Failed at:  " + res.errorStage, C_WARN, 60, y); y += 28; }
                if (!res.errorDetail.empty())
                    { drawText(fSm, res.errorDetail, C_GRAY, 60, y); y += 28; }
            }
            if (res.unresolved > 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "Unresolved symbols: %d  (may crash when those code paths execute)",
                         res.unresolved);
                drawText(fSm, buf, C_WARN, 60, y); y += 28;
            }
            if (res.svcPermCode != 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "JIT alloc: 0x%08X — code segment not executable", res.svcPermCode);
                drawText(fSm, buf, C_ERR, 60, y); y += 28;
                drawText(fSm,
                         "jitCreate/jitTransitionToExecutable failed. Needs Atmosphere CFW.",
                         C_GRAY, 60, y); y += 28;
            }
            y += 8;
            drawText(fSm, "Full log: sdmc:/Viridite/compat_log.txt", C_DIM, 60, y);

            drawFooterBar({{BG(GLYPH_B, "B"), "Back to menu"}, {BG(GLYPH_PLUS, "+"), "Menu"}});

            if (!shotResult) {
                shotResult = true;
                saveScreenshot("ui_result.png");
            }
            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }
    }

    // ------------------------------------------------------------------
    void showAbout() {
        bool done = false;
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { done = true; }
                if (ev.type == SDL_JOYBUTTONDOWN &&
                    (ev.jbutton.button == BTN_B || ev.jbutton.button == BTN_MINUS))
                    { done = true; }
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
                    { done = true; }
            }

            std::vector<uint8_t> img;
            if (avatarPollNewImage(img)) {
                SDL_RWops* rw = SDL_RWFromConstMem(img.data(), (int)img.size());
                SDL_Surface* surf = IMG_Load_RW(rw, 1);
                if (surf) {
                    if (avatarTex) SDL_DestroyTexture(avatarTex);
                    avatarTex = SDL_CreateTextureFromSurface(rdr, surf);
                    SDL_FreeSurface(surf);
                }
            }

            drawBackground();
            drawHeaderBar();

            int avSz = 160;
            int avX  = (SW - avSz) / 2;
            int avY  = LIST_Y + 30;
            if (avatarTex) {
                SDL_Rect dst = {avX, avY, avSz, avSz};
                SDL_RenderCopy(rdr, avatarTex, nullptr, &dst);
            } else {
                drawMonogram("Viridite", avX, avY, avSz);
                // Centred placeholder text below the monogram — only shown
                // for the one frame before the bundled avatar decodes
                static const std::string FETCH = "Loading avatar...";
                int fw = 0, fh = 0;
                TTF_SizeUTF8(fSm, FETCH.c_str(), &fw, &fh);
                drawText(fSm, FETCH, C_DIM, (SW - fw) / 2, avY + avSz + 8);
            }

            int y = avY + avSz + 40;
            auto center = [&](TTF_Font* f, const std::string& s, SDL_Color col) {
                int w = 0, h = 0;
                TTF_SizeUTF8(f, s.c_str(), &w, &h);
                drawText(f, s, col, (SW - w) / 2, y);
                y += h + 10;
            };
            center(fLg, "Viridite", C_WHITE);
            center(fSm, BUILD_VERSION, C_DIM);
            center(fSm, "by aaronworld.uk", C_GRAY);
            y += 10;
            center(fSm, "Android NDK compatibility layer for Nintendo Switch (HorizonOS)", C_GRAY);

            drawFooterBar({{BG(GLYPH_B, "B"), "Back to menu"}});

            if (!shotAbout && avatarTex) {  // wait for the avatar to arrive
                shotAbout = true;
                saveScreenshot("ui_about.png");
            }
            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }
    }
};

// ---------------------------------------------------------------------------
// Forwarder support — a small NRO (or Sphaira/hbmenu entry) can chain-load us
// with a package name via libnx's envSetNextLoad(path, argv), the same
// mechanism RetroArch forwarders use to boot straight into a ROM+core instead
// of RetroArch's own content browser. libnx's crt0 already parses the
// loader-provided argument block into plain argc/argv before main() runs, so
// no envGetArgv() plumbing is needed here — just read argv like any other
// command-line program.
//
// argv[0] MUST be this binary's own real path, NOT the package name. libnx's
// romfsInit() falls back to argv[0] to find and open its own .nro file on
// the SD card and read its embedded RomFS section — overwrite argv[0] with
// anything else and RomFS mounting silently fails, which took down every
// font load immediately (confirmed via this binary's own log.txt: "BFTTF
// open failed" + "romfs font open failed" for all three fonts, then "Font
// load failed" and an early exit — no compat_log.txt was ever opened,
// because that happens later in runLaunch(), well past where this failed).
// The caller (launcher/source/main.cpp) passes "<our own path> <package>"
// as the argv string, so the real argument lands at argv[1], same as any
// normal argv[0]-is-the-program-path command line.
// ---------------------------------------------------------------------------
// This binary is the ENGINE half of a two-part split: the AHNX launcher
// (kept as a separate, smaller NRO — see launcher/) shows the app list and
// chain-loads here via envSetNextLoad(path, argv) with a package name in
// argv[1] — the same mechanism the earlier single-binary "forwarder mode"
// used, just now the ALWAYS path instead of a fallback. This binary has no
// interactive picker of its own any more; it always expects a package name.
int main(int argc, char** argv) {
    App app;

    if (!app.init()) return 1;

    mkdir(APK_DIR, 0777);

    app.drawBackground();
    app.drawHeaderBar();
    app.drawText(app.fSm, "Scanning for APKs...", C_GRAY, 30, LIST_Y + 30);
    SDL_RenderPresent(app.rdr);

    app.apks = scanApks(APK_DIR);

    // log.txt (opened early in app.init(), unlike compat_log.txt which only
    // opens once launchApk() runs) — cheap insurance so a failure anywhere
    // between here and runLaunch() still leaves a trace of what argv held.
    char argvDbg[160];
    snprintf(argvDbg, sizeof(argvDbg), "core-x64: argc=%d argv[0]=%s argv[1]=%s",
             argc, (argc > 0 && argv[0]) ? argv[0] : "(none)",
             (argc > 1 && argv[1]) ? argv[1] : "(none)");
    logMsg(argvDbg);

    const char* wantPkg = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : nullptr;
    int idx = -1;

    // The launcher records the exact file it chose (two builds of one game can
    // share a package id). Prefer that so we launch the right one.
    std::string wantPath;
    if (FILE* lm = fopen("sdmc:/Viridite/.launch_apk", "r")) {
        char b[512] = {0};
        if (fgets(b, sizeof(b), lm)) wantPath = b;
        fclose(lm);
        while (!wantPath.empty() && (wantPath.back() == '\n' || wantPath.back() == '\r')) wantPath.pop_back();
    }
    if (!wantPath.empty()) {
        for (size_t i = 0; i < app.apks.size(); i++)
            if (app.apks[i].path == wantPath) { idx = (int)i; break; }
        if (idx >= 0) logMsg(("core-x64: launch target -> " + wantPath).c_str());
    }
    if (idx < 0 && wantPkg) {
        for (size_t i = 0; i < app.apks.size(); i++)
            if (app.apks[i].packageName == wantPkg) { idx = (int)i; break; }
    }

    if (idx < 0) {
        // Launched with no/unknown package — this binary isn't meant to be
        // run directly. Say so plainly instead of showing a blank screen.
        compatLogFmt("core-x64: no valid package argument (got '%s') — this binary "
                     "is launched by the AHNX launcher, not directly",
                     wantPkg ? wantPkg : "(none)");
        app.drawBackground();
        app.drawHeaderBar();
        app.drawText(app.fLg, "AHNX Translation Core (x64)", C_WHITE, 30, LIST_Y + 30);
        app.drawText(app.fSm,
            "This is the game-loading engine, not the launcher — it needs a "
            "package name to run.", C_GRAY, 30, LIST_Y + 76);
        app.drawText(app.fSm,
            "Launch a game from the Viridite app list instead.",
            C_GRAY, 30, LIST_Y + 104);
        app.drawFooterBar({{app.BG(app.GLYPH_PLUS, "+"), "Quit"}});
        SDL_RenderPresent(app.rdr);
        bool done = false;
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) done = true;
                if (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == BTN_PLUS) done = true;
            }
            SDL_Delay(16);
        }
        app.cleanup();
        return 1;
    }

    const ApkInfo& apk = app.apks[idx];
    bool skip = apk.installed;
    LaunchResult res = app.runLaunch(apk, skip);
    if (!skip) app.apks[idx].installed = true;
    // Every successful game session now exits the whole process directly
    // from inside the game loop itself (crash or deliberate quit alike) —
    // runLaunch() only returns here at all when that DIDN'T happen (APK/ELF
    // load failure, or nativeRender missing), so reaching this line always
    // means something worth explaining before closing back to the launcher
    // (or the Switch home menu, if this was launched standalone for testing).
    app.showLaunchResult(res, idx);
    app.cleanup();
    return 0;
}
