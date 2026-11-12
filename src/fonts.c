// A font closer to the English original for the Russian game.
//
// The original game draws its menus in Bank Gothic and ITC Conduit and its briefing screens in T26 Carbon. Those fonts have no
// Cyrillic, so the Russian build draws everything in one generic condensed sans. The mod replaces the glyphs of the game's
// font assets with glyphs rendered by Windows GDI from the user-selectable files bank.ttf, default.otf and defaultBold.otf in
// <game dir>\mw2coop\fonts. The bundled H*-Mod fonts are defaults (SIL OFL), but users can replace the files without changing their names.
//
// How the game's fonts work (mapped with "!font <name>", memdump.c):
//   Font_s (asset type 24): name, pixelHeight (int), glyphCount (int), Material *material, Material *glowMaterial, Glyph *glyphs.
//   Glyph (24 bytes): letter (u16, a Windows-1251 code in this build), x0 y0 (s8: from the pen to the glyph's top left; the pen is
//   at the bottom of the font's cell, the baseline about pixelHeight / 8 above it), dx (u8 advance), pixelWidth pixelHeight (u8), pad,
//   s0 t0 s1 t1 (floats: the glyph's rectangle in the atlas).
//   All fonts of the build share one atlas: material "fonts/gamefonts_pc" (the glow material uses the same texture), a DXT5 texture
//   (the original is 512 x 844, white colour with glyph shapes in alpha) reached through Material -> textureTable (MaterialTextureDef,
//   16 bytes, GfxImage * at +8) -> GfxImage, whose first field is the IDirect3DTexture9 * and whose width/height are two u16 at +0x18.
// A replacement atlas is built on the game's own D3D9 device and installed into that GfxImage. Its glyphs are rasterized at 4x
// resolution, while the game's glyph metrics remain in logical 1x coordinates. The texture has ATLAS_MIPS levels (4x, 2x, 1x: a text
// drawn at its logical size shows the 1x level, so it is filtered, not skipped over) and uses the renderer's existing sampler state. Latin letters and their Cyrillic look-alikes share identical bitmaps, as they do in the game's own tables.
// The game places a glyph on a grid of whole units only (x0, y0, pixelWidth, pixelHeight, dx). Each glyph's bitmap is therefore grown with a transparent margin to a whole number
// of units on all four sides (fontgrid.h), and its quad is that size: the 4x bitmap lands on the quad exactly, with no stretch and no rounding of the ink's edges.
// The unit is 1 / m of a logical pixel (the method of ttf2iwi): a font's pixelHeight is written as its height * m and the engine scales text by it, so the size on the screen
// stays and the positions and advances get m times finer. m is 4, 2 or 1: the finest that all glyphs of the font fit in the record's 8-bit fields.
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fontgrid.h"
#include "fonts.h"
#include "localize.h"
#include "dx/natboot.h"
#include "log.h"
#include "settings.h"

#ifdef _WIN64
#define FONT_HEIGHT 8
#define FONT_COUNT 0xc
#define FONT_MATERIAL 0x10
#define FONT_GLYPHS 0x20
#define MATERIAL_TABLE 0x60
#define IMAGE_DIMS 0x18
#define IMAGE_DATALEN 0x10
#else
// 32-bit (verified against the supported build): 4-byte pointers; material textureTable at +0x54, GfxImage dimensions at +0x14 and data lengths at +0x0c/+0x10.
#define FONT_HEIGHT 4
#define FONT_COUNT 8
#define FONT_MATERIAL 0x0c
#define FONT_GLYPHS 0x14
#define MATERIAL_TABLE 0x54
#define IMAGE_DIMS 0x14
#define IMAGE_DATALEN 0x0c
#endif
#define DEF_IMAGE 8
#define GLYPH_SIZE 24
#define FONT_RENDER_SCALE 4
#define TEXT_FACE_Y_ADJUST 2
#define BANK_WIDTH_PERCENT 95
#define ATLAS_MIN_WIDTH 1024
#define ATLAS_MAX_SIZE 4096
#define ATLAS_GLYPH_PADDING 1
// Levels of the atlas: 4x, 2x and 1x of the logical size. The packer puts every glyph in a cell whose origin and size are multiples of 4 pixels, so a
// texel of these levels never covers two cells (a 0.5x level would, and bleed one glyph into its neighbour).
#define ATLAS_MIPS 3

// The faces are the supplied files in mw2coop\fonts; the bundled defaults approximate Bank Gothic, Conduit and Carbon.
// The font assets that live on the shared atlas, and the face assigned to each asset.
enum { FACE_BANK, FACE_TEXT, FACE_BOLD, FACE_COUNT };
static const char *const face_files[FACE_COUNT] = {"bank.ttf", "default.otf", "defaultBold.otf"};

static const struct {
	const char *name;
	int face;
} plan[] = {
	{"fonts/bigfont", FACE_TEXT}, {"fonts/extrabigfont", FACE_TEXT}, {"fonts/objectivefont", FACE_BOLD},
	{"fonts/hudbigfont", FACE_BANK}, {"fonts/hudsmallfont", FACE_BANK}, {"fonts/normalfont", FACE_TEXT},
	{"fonts/smallfont", FACE_TEXT}, {"fonts/boldfont", FACE_BOLD}, {"fonts/consolefont", FACE_TEXT},
};
// The target height of Й, including its breve, as a fraction of the game font asset height.
#define CAP_FACTOR 0.685
#define PLAN_COUNT (int)(sizeof(plan) / sizeof(plan[0]))

typedef struct {
	// A bitmap is made twice. render() gives the ink as GDI drew it (w, h tight, ox, oy its place). Once the font's grid is known (finish_job), grid_bitmap() makes
	// the bitmap that goes into the atlas from it: the same ink with a transparent margin up to the grid's units, w = lw * q, h = lh * q, and the tight one is dropped.
	int w, h;       // 4x bitmap dimensions used for atlas packing (0 for a blank glyph)
	int ox, oy;     // tight bitmap only: 4x left bearing and top above the baseline
	int left, top;  // gridded bitmap only: the quad in units: from the pen to its left edge, from the baseline up to its top edge (the glyph's x0 and, with the baseline, y0)
	int lw, lh;     // the quad's size in units (pixelWidth, pixelHeight)
	double adv;     // 4x advance; -1 means the face has no matching character
	uint8_t *alpha; // w * h, 0..255
	uint32_t hash;
	int x, y;       // where the bitmap sits in the atlas (set by the packer)
	int shared;     // index of an identical bitmap, or -1
} Bitmap;

static Bitmap *bitmaps;
static int bitmap_count, bitmap_cap;

static int readable(const void *p, size_t size)
{
	MEMORY_BASIC_INFORMATION mbi;
	if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
		return 0;
	return (const uint8_t *)p + size <= (const uint8_t *)mbi.BaseAddress + mbi.RegionSize;
}

// Resolve the shared atlas through Material -> textureTable -> GfxImage.
static uint8_t *material_image(const uint8_t *material)
{
	if (!material || !readable(material, MATERIAL_TABLE + sizeof(void *)))
		return NULL;
	const uint8_t *table = *(const uint8_t *const *)(material + MATERIAL_TABLE);
	if (!table || !readable(table, 16))
		return NULL;
	uint8_t *image = *(uint8_t *const *)(table + DEF_IMAGE);
	return image && readable(image, 0x30) ? image : NULL;
}

// ---- GDI: the faces and their glyphs --------------------------------------------------------------------
// GDI imports a file privately, then requires its registered face name when creating an HFONT.
static wchar_t font_dir[MAX_PATH];
static HDC dc;
static int faces_loaded;
static SRWLOCK faces_lock = SRWLOCK_INIT;
static wchar_t family[FACE_COUNT][LF_FACESIZE];
static LONG face_weight[FACE_COUNT];
static BYTE face_italic[FACE_COUNT];
static int font_registered[FACE_COUNT];

typedef BOOL (WINAPI *GetFontResourceInfoWFn)(LPCWSTR, LPDWORD, LPVOID, DWORD);

static int get_file_face(const wchar_t *path, int added_count, LOGFONTW *out)
{
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    GetFontResourceInfoWFn query = gdi ? (GetFontResourceInfoWFn)GetProcAddress(gdi, "GetFontResourceInfoW") : NULL;
    if (!query) {
        log_printf("fonts: GetFontResourceInfoW is unavailable");
        return 0;
    }

    DWORD bytes = 0;
    if (!query(path, &bytes, NULL, 2) || bytes < sizeof(LOGFONTW) || bytes > 1024 * 1024) {
        bytes = (DWORD)(added_count > 0 ? added_count : 1) * (DWORD)sizeof(LOGFONTW);
    }
    if (bytes < sizeof(LOGFONTW) || bytes > 1024 * 1024) {
        log_printf("fonts: invalid font descriptor size for %ls", path);
        return 0;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        LOGFONTW *entries = malloc(bytes);
        if (!entries) {
            log_printf("fonts: out of memory reading font descriptor for %ls", path);
            return 0;
        }
        DWORD used = bytes;
        BOOL ok = query(path, &used, entries, 2);
        if (ok && used >= sizeof(LOGFONTW) && used <= bytes) {
            size_t count = used / sizeof(LOGFONTW);
            for (size_t i = 0; i < count; i++) {
                if (entries[i].lfFaceName[0]) {
                    *out = entries[i];
                    free(entries);
                    return 1;
                }
            }
        }
        free(entries);
        if (used <= bytes || used > 1024 * 1024)
            break;
        bytes = used;
    }
    log_printf("fonts: Windows could not read the face descriptor from %ls", path);
    return 0;
}

static int make_font_path(int index, wchar_t *path, size_t path_count)
{
    static const wchar_t *const files[FACE_COUNT] = {L"bank.ttf", L"default.otf", L"defaultBold.otf"};
    int n = _snwprintf(path, path_count, L"%ls%ls", font_dir, files[index]);
    if (n < 0 || (size_t)n >= path_count) {
        if (path_count) path[path_count - 1] = 0;
        log_printf("fonts: path for %ls is too long", files[index]);
        return 0;
    }
    return 1;
}

static void release_faces(void)
{
    for (int i = 0; i < FACE_COUNT; i++) {
        wchar_t path[MAX_PATH];
        if (font_registered[i] && make_font_path(i, path, MAX_PATH))
            RemoveFontResourceExW(path, FR_PRIVATE, NULL);
        font_registered[i] = 0;
        family[i][0] = 0;
        face_weight[i] = 0;
        face_italic[i] = 0;
    }
    if (dc) {
        DeleteDC(dc);
        dc = NULL;
    }
}

static int load_faces_unlocked(void)
{
    static const wchar_t *const files[FACE_COUNT] = {L"bank.ttf", L"default.otf", L"defaultBold.otf"};
    if (faces_loaded)
        return dc != NULL;
    DWORD n = GetModuleFileNameW(NULL, font_dir, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        log_printf("fonts: cannot resolve the game executable path");
        return 0;
    }
    while (n > 0 && font_dir[n - 1] != L'\\' && font_dir[n - 1] != L'/') n--;
    if (!n || n + sizeof(L"mw2coop\\fonts\\") / sizeof(wchar_t) > MAX_PATH) {
        log_printf("fonts: invalid or oversized font directory path");
        font_dir[0] = 0;
        return 0;
    }
    lstrcpyW(font_dir + n, L"mw2coop\\fonts\\");
    dc = CreateCompatibleDC(NULL);
    if (!dc) {
        log_printf("fonts: CreateCompatibleDC failed");
        return 0;
    }

    for (int i = 0; i < FACE_COUNT; i++) {
        wchar_t path[MAX_PATH];
        if (!make_font_path(i, path, MAX_PATH)) {
            release_faces();
            return 0;
        }
        DWORD attributes = GetFileAttributesW(path);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            log_printf("fonts: %ls is missing from mw2coop\\fonts", files[i]);
            release_faces();
            return 0;
        }
        int added = AddFontResourceExW(path, FR_PRIVATE, NULL);
        if (added <= 0) {
            log_printf("fonts: Windows could not import %ls", files[i]);
            release_faces();
            return 0;
        }
        font_registered[i] = 1;
        LOGFONTW descriptor = {0};
        if (!get_file_face(path, added, &descriptor)) {
            release_faces();
            return 0;
        }
        lstrcpynW(family[i], descriptor.lfFaceName, LF_FACESIZE);
        face_weight[i] = descriptor.lfWeight;
        face_italic[i] = descriptor.lfItalic;
        log_printf("fonts: imported %ls (GDI face %ls, weight %ld)", files[i], family[i], face_weight[i]);
    }
    faces_loaded = 1;
    return 1;
}

static int load_faces(void)
{
    AcquireSRWLockExclusive(&faces_lock);
    int result = load_faces_unlocked();
    ReleaseSRWLockExclusive(&faces_lock);
    return result;
}

static HFONT make_font(int face, int px)
{
    return CreateFontW(-px, 0, 0, 0, face_weight[face] ? face_weight[face] : FW_DONTCARE, face_italic[face] != 0, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family[face]);
}

// Estimate glyph size from Й where available so the top breve is included in the target cap height.
static double cap_ratio(int face)
{
    HFONT font = make_font(face, 200);
    if (!font) return 0.7;
    HGDIOBJ old = SelectObject(dc, font);
    wchar_t ch = L'\x0419';
    WORD index = 0xffff;
    if (GetGlyphIndicesW(dc, &ch, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR || index == 0xffff) ch = L'H';
    GLYPHMETRICS gm;
    MAT2 id = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    double ratio = 0.7;
    if (GetGlyphOutlineW(dc, ch, GGO_METRICS, &gm, 0, NULL, &id) != GDI_ERROR && gm.gmBlackBoxY) ratio = gm.gmBlackBoxY / 200.0;
    if (ratio < 0.25 || ratio > 0.95) ratio = 0.7;
    SelectObject(dc, old);
    DeleteObject(font);
    return ratio;
}

// Hash glyph coverage first; equal hashes are confirmed by comparing the bitmap contents.
static uint32_t hash_bytes(const uint8_t *p, size_t n, uint32_t h)
{
	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

// Apply the Bank Gothic width correction while retaining the font face's vertical metrics.
static MAT2 glyph_matrix(int width_percent)
{
    MAT2 matrix = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    if (width_percent > 0 && width_percent < 100) {
        matrix.eM11.value = 0;
        matrix.eM11.fract = (WORD)((width_percent * 65536u + 50) / 100);
    }
    return matrix;
}

// Use the face's unhinted ABC advance instead of the rasterizer's grid-fitted cell increment.
static double glyph_advance(HDC context, wchar_t ch, int width_percent, int fallback)
{
    ABCFLOAT abc;
    if (GetCharABCWidthsFloatW(context, ch, ch, &abc)) {
        double advance = (double)abc.abcfA + abc.abcfB + abc.abcfC;
        advance *= (double)width_percent / 100.0;
        if (advance >= 0.0 && advance <= (double)ATLAS_MAX_SIZE * FONT_RENDER_SCALE)
            return advance;
    }
    return fallback;
}

// Render each glyph once with the face's selected width transform; never substitute a differently rendered face for a GDI error.
static int render(HFONT font, wchar_t ch, int width_percent, const char **failure)
{
    if (failure) *failure = NULL;
    if (bitmap_count == bitmap_cap) {
        if (bitmap_cap > 0x3fffffff) {
            if (failure) *failure = "bitmap table capacity";
            return -1;
        }
        int capacity = bitmap_cap ? bitmap_cap * 2 : 4096;
        Bitmap *items = realloc(bitmaps, (size_t)capacity * sizeof(Bitmap));
        if (!items) {
            if (failure) *failure = "bitmap table allocation";
            return -1;
        }
        bitmaps = items;
        bitmap_cap = capacity;
    }

    Bitmap *b = &bitmaps[bitmap_count];
    memset(b, 0, sizeof(*b));
    b->shared = -1;
    MAT2 matrix = glyph_matrix(width_percent);
    HGDIOBJ old = SelectObject(dc, font);
    if (!old || old == HGDI_ERROR) {
        if (failure) *failure = "GDI font selection";
        return -1;
    }

    WORD index = 0xffff;
    if (GetGlyphIndicesW(dc, &ch, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) {
        if (failure) *failure = "GDI glyph-index query";
        SelectObject(dc, old);
        return -1;
    }
    if (index == 0xffff) {
        b->adv = -1;
        b->hash = hash_bytes((const uint8_t *)"", 0, 2166136261u);
        SelectObject(dc, old);
        return bitmap_count++;
    }

    GLYPHMETRICS gm;
    DWORD size = GetGlyphOutlineW(dc, ch, GGO_GRAY8_BITMAP, &gm, 0, NULL, &matrix);
    if (size == GDI_ERROR) {
        if (failure) *failure = "GDI glyph bitmap size query";
        goto fail;
    }
    b->adv = glyph_advance(dc, ch, width_percent, (int)gm.gmCellIncX);
    if (size && gm.gmBlackBoxX && gm.gmBlackBoxY) {
        int width = (int)gm.gmBlackBoxX;
        int height = (int)gm.gmBlackBoxY;
        int stride = (width + 3) & ~3;
        if (width > ATLAS_MAX_SIZE - 2 * ATLAS_GLYPH_PADDING || height > ATLAS_MAX_SIZE - 2 * ATLAS_GLYPH_PADDING ||
            (size_t)stride > SIZE_MAX / (size_t)height || (size_t)stride * (size_t)height > (size_t)size) {
            if (failure) *failure = "invalid or oversized GDI glyph bitmap";
            goto fail;
        }
        uint8_t *raw = malloc(size);
        if (!raw) {
            if (failure) *failure = "GDI glyph bitmap allocation";
            goto fail;
        }
        GLYPHMETRICS read_gm;
        if (GetGlyphOutlineW(dc, ch, GGO_GRAY8_BITMAP, &read_gm, size, raw, &matrix) == GDI_ERROR) {
            free(raw);
            if (failure) *failure = "GDI glyph bitmap read";
            goto fail;
        }
        width = (int)read_gm.gmBlackBoxX;
        height = (int)read_gm.gmBlackBoxY;
        stride = (width + 3) & ~3;
        if (width <= 0 || height <= 0 || width > ATLAS_MAX_SIZE - 2 * ATLAS_GLYPH_PADDING ||
            height > ATLAS_MAX_SIZE - 2 * ATLAS_GLYPH_PADDING ||
            (size_t)stride > SIZE_MAX / (size_t)height || (size_t)stride * (size_t)height > (size_t)size) {
            free(raw);
            if (failure) *failure = "invalid GDI glyph bitmap stride";
            goto fail;
        }
        b->w = width;
        b->h = height;
        b->ox = (int)read_gm.gmptGlyphOrigin.x;
        b->oy = (int)read_gm.gmptGlyphOrigin.y;
        b->alpha = malloc((size_t)width * (size_t)height);
        if (!b->alpha) {
            free(raw);
            if (failure) *failure = "glyph coverage allocation";
            goto fail;
        }
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                int value = raw[(size_t)y * stride + x] * 255 / 64;
                b->alpha[(size_t)y * width + x] = (uint8_t)(value > 255 ? 255 : value);
            }
        free(raw);
    }
    SelectObject(dc, old);
    b->hash = hash_bytes(b->alpha ? b->alpha : (const uint8_t *)"", b->alpha ? (size_t)b->w * b->h : 0, 2166136261u ^ (uint32_t)(b->w * 977 + b->h));
    return bitmap_count++;

fail:
    free(b->alpha);
    memset(b, 0, sizeof(*b));
    SelectObject(dc, old);
    return -1;
}

static int is_per_glyph_gdi_failure(const char *failure)
{
    return failure && (!strcmp(failure, "GDI glyph bitmap size query") || !strcmp(failure, "GDI glyph bitmap read"));
}

// Store an empty glyph when a control code or an individual GDI raster operation has no bitmap.
static int add_blank_bitmap(double advance)
{
    if (bitmap_count == bitmap_cap) {
        if (bitmap_cap > 0x3fffffff)
            return -1;
        int capacity = bitmap_cap ? bitmap_cap * 2 : 4096;
        Bitmap *items = realloc(bitmaps, (size_t)capacity * sizeof(Bitmap));
        if (!items)
            return -1;
        bitmaps = items;
        bitmap_cap = capacity;
    }
    Bitmap *b = &bitmaps[bitmap_count];
    memset(b, 0, sizeof(*b));
    b->shared = -1;
    b->adv = advance;
    b->hash = hash_bytes((const uint8_t *)"", 0, 2166136261u);
    return bitmap_count++;
}

// The tight bitmap is not needed any more: free its coverage and make the packer skip it.
static void drop_bitmap(int index)
{
	Bitmap *b = &bitmaps[index];
	free(b->alpha);
	b->alpha = NULL;
	b->w = b->h = 0;
	b->hash = hash_bytes((const uint8_t *)"", 0, 2166136261u);
}

// The bitmap that goes into the atlas, made from a tight one for a grid of q texels per unit: the same ink with a transparent margin up to whole units (fontgrid.h), so the
// quad of lw x lh units and the bitmap of (lw * q) x (lh * q) texels match exactly. The tight bitmap is dropped. A bitmap without ink is returned as it is; -1 is out of memory.
static int grid_bitmap(int tight, int q)
{
	if (!bitmaps[tight].w || !bitmaps[tight].h)
		return tight;
	int tw = bitmaps[tight].w, th = bitmaps[tight].h;
	FontGridBox box = fontgrid_box(bitmaps[tight].ox, bitmaps[tight].oy, tw, th, q);
	int grid_w = box.width * q, grid_h = box.height * q;
	if (grid_w > ATLAS_MAX_SIZE - 2 * ATLAS_GLYPH_PADDING || grid_h > ATLAS_MAX_SIZE - 2 * ATLAS_GLYPH_PADDING)
		return -1;
	uint8_t *alpha = calloc((size_t)grid_w * (size_t)grid_h, 1);
	if (!alpha)
		return -1;
	int made = add_blank_bitmap(bitmaps[tight].adv); // (may move the table: take the pointers after it)
	if (made < 0) {
		free(alpha);
		return -1;
	}
	const Bitmap *t = &bitmaps[tight];
	for (int y = 0; y < th; y++)
		memcpy(alpha + (size_t)(y + box.pad_top) * grid_w + box.pad_left, t->alpha + (size_t)y * tw, (size_t)tw);
	Bitmap *b = &bitmaps[made];
	b->alpha = alpha;
	b->w = grid_w;
	b->h = grid_h;
	b->left = box.left;
	b->top = box.top;
	b->lw = box.width;
	b->lh = box.height;
	b->hash = hash_bytes(alpha, (size_t)grid_w * (size_t)grid_h, 2166136261u ^ (uint32_t)(grid_w * 977 + grid_h));
	drop_bitmap(tight);
	return made;
}

// ---- packing and DXT5 -----------------------------------------------------------------------------------
// Keep one transparent texel inside each edge DXT5 block; cells remain four-texel aligned to prevent glyphs sharing a block.
static int cmp_height(const void *a, const void *b)
{
	const Bitmap *x = &bitmaps[*(const int *)a], *y = &bitmaps[*(const int *)b];
	return y->h != x->h ? y->h - x->h : y->w - x->w;
}

static int pack(const int *order, int count, int width, int max_height)
{
    int x = 0, y = 0, row_h = 0;
    for (int i = 0; i < count; i++) {
        Bitmap *b = &bitmaps[order[i]];
        // one transparent 4-pixel texel before the bitmap (the next cell's before-texel serves as the one after it): the 1x level's bilinear
        // sample at the quad's edge reads a whole texel outside it, which must not hold a neighbour's glyph
        int cw = ((b->w + 3) & ~3) + 4;
        int ch = ((b->h + 3) & ~3) + 4;
        if (x + cw + 4 > width) {
            x = 0;
            y += row_h;
            row_h = 0;
        }
        if (cw > width) return 0;
        b->x = x + 4;
        b->y = y + 4;
        x += cw;
        if (ch > row_h) row_h = ch;
    }
    y += row_h + 4;
    y = (y + 3) & ~3;
    return y <= max_height ? y : 0;
}

// Encode one alpha-only 4x4 glyph-atlas block as DXT5 with white RGB, matching the game atlas.
static void encode_block(const uint8_t *alpha, int stride, uint8_t out[16])
{
	int mx = 0, mn = 255;
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++) {
			int a = alpha[y * stride + x];
			if (a > mx)
				mx = a;
			if (a < mn)
				mn = a;
		}
	int pal[8];
	pal[0] = mx;
	pal[1] = mn;
	if (mx > mn) {
		for (int i = 1; i < 7; i++)
			pal[1 + i] = ((7 - i) * mx + i * mn) / 7;
	} else {
		for (int i = 2; i < 8; i++)
			pal[i] = mx;
	}
	uint64_t bits = 0;
	for (int p = 0; p < 16; p++) {
		int a = alpha[(p / 4) * stride + (p % 4)], best = 0, bd = 1 << 30;
		for (int i = 0; i < 8; i++) {
			int d = abs(a - pal[i]);
			if (d < bd) {
				bd = d;
				best = i;
			}
		}
		bits |= (uint64_t)best << (3 * p);
	}
	out[0] = (uint8_t)mx;
	out[1] = (uint8_t)mn;
	for (int i = 0; i < 6; i++)
		out[2 + i] = (uint8_t)(bits >> (8 * i));
	out[8] = 0xff; // colour end points 0xffff and 0x0000, all indices 0: white, as the game's atlas
	out[9] = 0xff;
	out[10] = 0;
	out[11] = 0;
	memset(out + 12, 0, 4);
}

// Replace the glyph tables and shared texture.
static int patched;
static volatile LONG patch_state; /* 0 = available for retry, 1 = in progress, 2 = successfully patched */
static uint8_t *patched_image;
static IDirect3DTexture9 *patched_texture;
static void *patched_fonts[PLAN_COUNT];
static uint8_t *patched_glyphs[PLAN_COUNT];
static const uint8_t *patched_materials[PLAN_COUNT];
static int patched_counts[PLAN_COUNT];
static uint32_t patched_signatures[PLAN_COUNT];

// The game's glyph table stores Windows-1251 codes, while GDI expects Unicode.
static wchar_t unicode_of(unsigned letter)
{
	if (letter < 0x80 || letter > 0xff)
		return (wchar_t)letter;
	char c = (char)letter;
	wchar_t w = 0;
	if (MultiByteToWideChar(1251, 0, &c, 1, &w, 1) != 1)
		return 0;
	return w;
}

// One game Font_s and the generated bitmap index for each of its glyph entries.
typedef struct {
	uint8_t *font;
	int count;
	int *bitmap_of; // per glyph entry: the bitmap index
	int pixel_height; // the game's own (see height_original)
	int face;
	int plan_index;
	int m, q, base;   // the font's grid: m units per logical pixel, q = FONT_RENDER_SCALE / m texels per unit, the baseline's height above the pen in units (finish_job)
} FontJob;

// The table's pixelHeight is written as pixel_height * m, so a second patch of the same Font_s (after vid_restart the glyph tables stay patched while the texture is made again)
// must start from the game's own height and not from the one written. Not cleared with the rest of the tracking.
static void *height_font[PLAN_COUNT];
static int height_original[PLAN_COUNT], height_applied[PLAN_COUNT];

static int job_baseline(const FontJob *j, int m)
{
	double adjust = j->face == FACE_TEXT ? TEXT_FACE_Y_ADJUST : 0;
	return (int)((j->pixel_height * 0.13 + adjust) * m + 0.5); // m = 1: the whole-pixel baseline of the original tables
}

// Does every glyph of the font fit the game's 8-bit record on a grid of m units per pixel?
static int job_fits(const FontJob *j, int m)
{
	int q = FONT_RENDER_SCALE / m, base = job_baseline(j, m);
	for (int i = 0; i < j->count; i++) {
		const Bitmap *b = &bitmaps[j->bitmap_of[i]];
		FontGridMetrics g = fontgrid_metrics(b->ox, b->oy, b->w, b->h, b->adv, base, q);
		if (!fontgrid_in_range(&g, b->w > 0 && b->h > 0))
			return 0;
	}
	return 1;
}

// All glyphs of a font are rasterized (tight bitmaps): pick its grid, the finest of 4, 2 or 1 units per pixel that every glyph fits (a taller font
// leaves less room in the s8 y0), and make the bitmaps for the atlas. Returns 0 on out of memory.
static int finish_job(FontJob *j, const char *name)
{
	int m = 1;
	for (int c = FONT_RENDER_SCALE; c > 1; c--)
		if (FONT_RENDER_SCALE % c == 0 && job_fits(j, c)) {
			m = c;
			break;
		}
	j->m = m;
	j->q = FONT_RENDER_SCALE / m;
	j->base = job_baseline(j, m);
	int outside = 0;
	for (int i = 0; i < j->count; i++) {
		int tight = j->bitmap_of[i];
		if (!bitmaps[tight].w || !bitmaps[tight].h)
			continue;
		const Bitmap *b = &bitmaps[tight];
		FontGridMetrics g = fontgrid_metrics(b->ox, b->oy, b->w, b->h, b->adv, j->base, j->q);
		int bi;
		if (fontgrid_in_range(&g, 1)) {
			bi = grid_bitmap(tight, j->q);
		} else { // only on a grid of 1 unit per pixel: a glyph the record cannot hold is left blank rather than drawn off
			double adv = b->adv;
			outside++;
			bi = add_blank_bitmap(adv);
			if (bi >= 0)
				drop_bitmap(tight);
		}
		if (bi < 0)
			return 0;
		j->bitmap_of[i] = bi;
	}
	log_printf("fonts: %s: grid of %d unit%s per pixel (%d texels each), pixelHeight %d -> %d%s%s", name, m, m == 1 ? "" : "s", j->q, j->pixel_height, j->pixel_height * m,
		   m < FONT_RENDER_SCALE ? "; a finer grid does not fit the glyphs' 8-bit fields" : "", outside ? "; glyphs outside the record left blank" : "");
	if (outside)
		log_printf("fonts: %s: %d glyphs do not fit the game's glyph record even on the whole-pixel grid and are blank", name, outside);
	return 1;
}

static void free_bitmaps(void)
{
	for (int i = 0; i < bitmap_count; i++)
		free(bitmaps[i].alpha);
	free(bitmaps);
	bitmaps = NULL;
	bitmap_count = bitmap_cap = 0;
}

static void free_jobs(FontJob *jobs, int count)
{
	for (int i = 0; i < count; i++) {
		free(jobs[i].bitmap_of);
		jobs[i].bitmap_of = NULL;
	}
}

static void patch_all_impl(void *(*find)(int, const char *), const uint8_t *atlas_material)
{
	if (!load_faces()) {
		log_printf("fonts: unable to load all font files; will retry on the next font asset");
		return;
	}
	double ratio[FACE_COUNT];
	for (int i = 0; i < FACE_COUNT; i++)
		ratio[i] = cap_ratio(i);
	FontJob jobs[16];
	int njobs = 0;
	for (int p = 0; p < PLAN_COUNT && njobs < 16; p++) {
		uint8_t *f = find(24, plan[p].name);
		if (!f || !readable(f, 0x30) || *(const uint8_t *const *)(f + FONT_MATERIAL) != atlas_material)
			continue;
		FontJob *j = &jobs[njobs++];
		j->font = f;
		j->count = *(const int *)(f + FONT_COUNT);
		j->pixel_height = *(const int *)(f + FONT_HEIGHT);
		if (height_font[p] == (void *)f && height_applied[p] && j->pixel_height == height_applied[p])
			j->pixel_height = height_original[p]; // this table was patched before: its height is the one we wrote
		else {
			height_font[p] = f;
			height_original[p] = j->pixel_height;
			height_applied[p] = 0;
		}
		j->face = plan[p].face;
		j->plan_index = p;
		j->m = 1;
		j->q = FONT_RENDER_SCALE;
		const uint8_t *glyphs = *(const uint8_t *const *)(f + FONT_GLYPHS);
		if (j->count <= 0 || j->count > 4096 || j->pixel_height <= 0 || !readable(glyphs, (size_t)j->count * GLYPH_SIZE)) {
			njobs--;
			log_printf("fonts: invalid glyph data for %s", plan[p].name);
			continue;
		}
		j->bitmap_of = malloc((size_t)j->count * sizeof(int));
		if (!j->bitmap_of) {
			log_printf("fonts: out of memory allocating glyph map for %s", plan[p].name);
			free_jobs(jobs, njobs);
			free_bitmaps();
			return;
		}
		int fallback_face = plan[p].face == FACE_TEXT ? FACE_BANK : FACE_TEXT;
		int font_px = (int)(j->pixel_height * CAP_FACTOR / ratio[plan[p].face] + 0.5);
		int fallback_px = (int)(j->pixel_height * CAP_FACTOR / ratio[fallback_face] + 0.5);
		if (font_px < 1) font_px = 1;
		if (fallback_px < 1) fallback_px = 1;
		int font_width = plan[p].face == FACE_BANK ? BANK_WIDTH_PERCENT : 100;
		int fallback_width = fallback_face == FACE_BANK ? BANK_WIDTH_PERCENT : 100;
		HFONT font = make_font(plan[p].face, font_px * FONT_RENDER_SCALE);
		HFONT fallback = make_font(fallback_face, fallback_px * FONT_RENDER_SCALE);
		if (!font || !fallback) {
			log_printf("fonts: GDI could not create the requested font for %s", plan[p].name);
			if (font) DeleteObject(font);
			if (fallback) DeleteObject(fallback);
			free_jobs(jobs, njobs);
			free_bitmaps();
			return;
		}
		int fallback_count = 0;
		int fallback_still_missing = 0;
		int control_count = 0;
		int gdi_render_error_count = 0;
		int blank_after_failure_count = 0;
		int printable_count = 0;
		for (int i = 0; i < j->count; i++) {
			unsigned letter = *(const uint16_t *)(glyphs + (size_t)i * GLYPH_SIZE);
			wchar_t ch = unicode_of(letter);
			const char *render_failure = NULL;
			int bi;
			if (ch < L' ' || ch == 0x7f) {
				control_count++;
				bi = add_blank_bitmap(0);
			} else {
				printable_count++;
				bi = render(font, ch, font_width, &render_failure);
				int primary_failed = bi < 0;
				int primary_missing = bi >= 0 && bitmaps[bi].adv < 0;
				if (primary_failed && is_per_glyph_gdi_failure(render_failure)) {
					gdi_render_error_count++;
					bi = add_blank_bitmap(j->pixel_height * FONT_RENDER_SCALE / 3);
					if (bi >= 0) {
						blank_after_failure_count++;
						static LONG glyph_error_logged;
						if (InterlockedCompareExchange(&glyph_error_logged, 1, 0) == 0)
							log_printf("fonts: GDI could not rasterize U+%04X for %s (%s); leaving it blank rather than mixing font faces", (unsigned)ch, plan[p].name, render_failure);
					}
				} else if (primary_missing) {
					fallback_count++;
					const char *fallback_failure = NULL;
					int fallback_bi = render(fallback, ch, fallback_width, &fallback_failure);
					if (fallback_bi >= 0) {
						bi = fallback_bi;
						if (bitmaps[bi].adv < 0) {
							fallback_still_missing++;
							bitmaps[bi].adv = j->pixel_height * FONT_RENDER_SCALE / 3;
						}
					} else {
						bi = add_blank_bitmap(j->pixel_height * FONT_RENDER_SCALE / 3);
						if (bi >= 0) {
							fallback_still_missing++;
							blank_after_failure_count++;
							static LONG fallback_error_logged;
							if (InterlockedCompareExchange(&fallback_error_logged, 1, 0) == 0)
								log_printf("fonts: fallback could not rasterize U+%04X for %s (%s); using an empty glyph", (unsigned)ch, plan[p].name, fallback_failure ? fallback_failure : "unknown fallback error");
						}
					}
				}
			}
			if (bi < 0) {
				DeleteObject(font);
				DeleteObject(fallback);
				log_printf("fonts: unable to process glyph U+%04X for %s (%s)", (unsigned)ch, plan[p].name, render_failure ? render_failure : "glyph record allocation failed");
				free_jobs(jobs, njobs);
				free_bitmaps();
				return;
			}
			if (bitmaps[bi].adv < 0)
				bitmaps[bi].adv = j->pixel_height * FONT_RENDER_SCALE / 3;
			j->bitmap_of[i] = bi;
		}
		log_printf("fonts: %s fallback %d/%d printable glyphs from %s; unresolved %d, skipped controls %d, GDI render errors kept in primary face %d, empty after failure %d", plan[p].name, fallback_count, printable_count, face_files[plan[p].face], fallback_still_missing, control_count, gdi_render_error_count, blank_after_failure_count);
		DeleteObject(font);
		DeleteObject(fallback);
		if (!finish_job(j, plan[p].name)) {
			log_printf("fonts: out of memory making the glyph bitmaps for %s", plan[p].name);
			free_jobs(jobs, njobs);
			free_bitmaps();
			return;
		}
	}
	if (!njobs) {
		log_printf("fonts: none of the planned fonts lives on the shared atlas; will retry on the next font asset");
		free_bitmaps();
		return;
	}
	// Identical bitmaps (including Latin letters and their Cyrillic look-alikes) are stored once.
	int *unique = malloc((size_t)bitmap_count * sizeof(int));
	if (!unique) {
		log_printf("fonts: out of memory allocating atlas index");
		free_jobs(jobs, njobs);
		free_bitmaps();
		return;
	}
	int nuniq = 0;
	for (int i = 0; i < bitmap_count; i++) {
		Bitmap *b = &bitmaps[i];
		if (!b->w || !b->h)
			continue;
		int found = -1;
		for (int k = 0; k < nuniq && found < 0; k++) {
			Bitmap *other = &bitmaps[unique[k]];
			if (other->hash == b->hash && other->w == b->w && other->h == b->h && !memcmp(other->alpha, b->alpha, (size_t)b->w * b->h))
				found = unique[k];
		}
		if (found >= 0)
			b->shared = found;
		else
			unique[nuniq++] = i;
	}
	qsort(unique, (size_t)nuniq, sizeof(int), cmp_height);
	uint8_t *image = material_image(atlas_material);
	IDirect3DTexture9 *old_tex = image ? *(IDirect3DTexture9 **)image : NULL;
	IDirect3DDevice9 *caps_dev = NULL;
	D3DCAPS9 caps;
	if (!old_tex || FAILED(IDirect3DTexture9_GetDevice(old_tex, &caps_dev)) || !caps_dev || FAILED(IDirect3DDevice9_GetDeviceCaps(caps_dev, &caps))) {
		if (caps_dev) IDirect3DDevice9_Release(caps_dev);
		log_printf("fonts: cannot query the renderer texture limits");
		free(unique);
		free_jobs(jobs, njobs);
		free_bitmaps();
		return;
	}
	IDirect3DDevice9_Release(caps_dev);
	int max_width = caps.MaxTextureWidth < ATLAS_MAX_SIZE ? (int)caps.MaxTextureWidth : ATLAS_MAX_SIZE;
	int max_height = caps.MaxTextureHeight < ATLAS_MAX_SIZE ? (int)caps.MaxTextureHeight : ATLAS_MAX_SIZE;
	int widths[] = {ATLAS_MIN_WIDTH, 2048, ATLAS_MAX_SIZE};
	int atlas_width = 0, atlas_h = 0, content_h = 0;
	for (int i = 0; i < (int)(sizeof(widths) / sizeof(widths[0])); i++) {
		if (widths[i] > max_width)
			continue;
		int packed_h = pack(unique, nuniq, widths[i], max_height);
		int texture_h = 1;
		while (texture_h < packed_h && texture_h < ATLAS_MAX_SIZE)
			texture_h <<= 1;
		if (packed_h && texture_h >= packed_h && texture_h <= max_height) {
			atlas_width = widths[i];
			atlas_h = texture_h;
			content_h = packed_h;
			break;
		}
	}
	log_printf("fonts: %d fonts, %d glyph renders, %d distinct bitmaps, atlas %d x %d (content height %d), native raster %dx, ABC advances with GDI fallback, glyph quads grown to each font's grid (bitmap and quad match exactly), advances rounded to that grid, three mip levels, bank width %d%%", njobs, bitmap_count, nuniq, atlas_width, atlas_h, content_h, FONT_RENDER_SCALE, BANK_WIDTH_PERCENT);
	if (!atlas_h) {
		log_printf("fonts: glyphs do not fit supported texture dimensions; will retry on the next font asset");
		free(unique);
		free_jobs(jobs, njobs);
		free_bitmaps();
		return;
	}
	// Build the atlas as 8-bit alpha, then encode it as DXT5.
	uint8_t *alpha = calloc((size_t)atlas_width * atlas_h, 1);
	if (!alpha) {
		log_printf("fonts: out of memory allocating atlas pixels");
		free(unique);
		free_jobs(jobs, njobs);
		free_bitmaps();
		return;
	}
	for (int k = 0; k < nuniq; k++) {
		Bitmap *b = &bitmaps[unique[k]];
		for (int y = 0; y < b->h; y++)
			memcpy(alpha + (size_t)(b->y + y) * atlas_width + b->x, b->alpha + (size_t)y * b->w, (size_t)b->w);
	}
	// Create the texture on the game's own device.
	IDirect3DDevice9 *dev = NULL;
	IDirect3DTexture9 *tex = NULL;
	if (FAILED(IDirect3DTexture9_GetDevice(old_tex, &dev)) || !dev) {
		if (dev) IDirect3DDevice9_Release(dev);
		log_printf("fonts: cannot access the renderer device");
		free(alpha);
		free(unique);
		free_jobs(jobs, njobs);
		free_bitmaps();
		return;
	}
	HRESULT hr = IDirect3DDevice9_CreateTexture(dev, atlas_width, atlas_h, ATLAS_MIPS, 0, D3DFMT_DXT5, D3DPOOL_MANAGED, &tex, NULL);
	IDirect3DDevice9_Release(dev);
	if (FAILED(hr) || !tex) {
		log_printf("fonts: cannot create the replacement atlas texture (hr=0x%08lx); will retry on the next font asset", (unsigned long)hr);
		free(alpha);
		free(unique);
		free_jobs(jobs, njobs);
		free_bitmaps();
		return;
	}
	D3DLOCKED_RECT r;
	if (FAILED(IDirect3DTexture9_LockRect(tex, 0, &r, NULL, 0))) {
		log_printf("fonts: cannot lock the replacement atlas texture");
		free(alpha);
		free(unique);
		free_jobs(jobs, njobs);
		free_bitmaps();
		IDirect3DTexture9_Release(tex);
		return;
	}
	for (int by = 0; by < atlas_h / 4; by++)
		for (int bx = 0; bx < atlas_width / 4; bx++)
			encode_block(alpha + (size_t)(by * 4) * atlas_width + bx * 4, atlas_width, (uint8_t *)r.pBits + (size_t)by * r.Pitch + (size_t)bx * 16);
	if (FAILED(IDirect3DTexture9_UnlockRect(tex, 0))) {
		log_printf("fonts: cannot unlock the replacement atlas texture");
		free(alpha);
		free(unique);
		free_jobs(jobs, njobs);
		free_bitmaps();
		IDirect3DTexture9_Release(tex);
		return;
	}
	// The smaller levels: each texel the average of the 2 x 2 of the level above, encoded like level 0.
	{
		int lw = atlas_width, lh = atlas_h;
		uint8_t *prev = alpha;
		for (int lvl = 1; lvl < ATLAS_MIPS; lvl++) {
			int nw = lw / 2, nh = lh / 2;
			uint8_t *next = malloc((size_t)nw * nh);
			D3DLOCKED_RECT mr;
			if (!next || FAILED(IDirect3DTexture9_LockRect(tex, lvl, &mr, NULL, 0))) {
				log_printf("fonts: cannot build mip level %d of the atlas; the text is drawn without it", lvl);
				free(next);
				break;
			}
			for (int y = 0; y < nh; y++)
				for (int x = 0; x < nw; x++) {
					const uint8_t *q = prev + (size_t)(2 * y) * lw + 2 * x;
					next[(size_t)y * nw + x] = (uint8_t)((q[0] + q[1] + q[lw] + q[lw + 1] + 2) / 4);
				}
			for (int by = 0; by < nh / 4; by++)
				for (int bx = 0; bx < nw / 4; bx++)
					encode_block(next + (size_t)(by * 4) * nw + bx * 4, nw, (uint8_t *)mr.pBits + (size_t)by * mr.Pitch + (size_t)bx * 16);
			IDirect3DTexture9_UnlockRect(tex, lvl);
			if (prev != alpha)
				free(prev);
			prev = next;
			lw = nw;
			lh = nh;
		}
		if (prev != alpha)
			free(prev);
	}
	// Keep the renderer's sampler state unchanged; font filtering must be changed in the rendering path.
	free(alpha);
	// Update the glyph tables, then install the new texture.
	int yoff_clamped = 0; // glyphs whose y0 does not fit the game's s8 (they would be drawn off by the clamp)
	for (int n = 0; n < njobs; n++) {
		FontJob *j = &jobs[n];
		uint8_t *glyphs = *(uint8_t **)(j->font + FONT_GLYPHS);
		// The table's grid is 1 / m of a logical pixel (finish_job): the engine scales text by the font's pixelHeight, so the height goes up by m with all the units.
		*(int *)(j->font + FONT_HEIGHT) = j->pixel_height * j->m;
		height_applied[j->plan_index] = j->pixel_height * j->m;
		int baseline = j->base;
		for (int i = 0; i < j->count; i++) {
			Bitmap *b = &bitmaps[j->bitmap_of[i]];
			Bitmap *src = b->shared >= 0 ? &bitmaps[b->shared] : b;
			uint8_t *g = glyphs + (size_t)i * GLYPH_SIZE;
			int advance = fontgrid_round_div(b->adv, j->q);
			if (advance < 0) advance = 0;
			if (advance > 255) advance = 255;
			g[4] = (uint8_t)advance;
			if (src->w && src->h) {
				// The quad was fixed when the bitmap was made for the font's grid (grid_bitmap): its bitmap is exactly lw x lh units at q texels, so the
				// game's x0, y0, pixelWidth and pixelHeight carry it with no rounding and no stretch. Only y0 still depends on the baseline.
				int yoff = -(baseline + b->top);
				if (yoff < -128 || yoff > 127) {
					yoff_clamped++;
					yoff = yoff < -128 ? -128 : 127;
				}
				g[2] = (uint8_t)(int8_t)b->left;
				g[3] = (uint8_t)(int8_t)yoff;
				g[5] = (uint8_t)b->lw;
				g[6] = (uint8_t)b->lh;
				*(float *)(g + 8) = (float)src->x / atlas_width;
				*(float *)(g + 12) = (float)src->y / atlas_h;
				*(float *)(g + 16) = (float)(src->x + src->w) / atlas_width;
				*(float *)(g + 20) = (float)(src->y + src->h) / atlas_h;
			} else {
				g[2] = 0;
				g[3] = (uint8_t)(int8_t)(-j->pixel_height * j->m < -128 ? -128 : -j->pixel_height * j->m); // (no ink: where it sits does not matter, it must only fit the s8)
				g[5] = 0;
				g[6] = 0;
				*(float *)(g + 8) = *(float *)(g + 16) = 0.99f;
				*(float *)(g + 12) = *(float *)(g + 20) = 0.99f;
			}
		}
	}
	if (yoff_clamped)
		log_printf("fonts: %d glyphs have a vertical offset outside the game's glyph record range and were clamped", yoff_clamped);
	free_jobs(jobs, njobs);
	*(IDirect3DTexture9 **)image = tex;
	*(uint16_t *)(image + IMAGE_DIMS) = (uint16_t)atlas_width;
	*(uint16_t *)(image + IMAGE_DIMS + 2) = (uint16_t)atlas_h;
	uint32_t texture_bytes = (uint32_t)atlas_width * (uint32_t)atlas_h;
	*(uint32_t *)(image + IMAGE_DATALEN) = texture_bytes;
	*(uint32_t *)(image + IMAGE_DATALEN + 4) = texture_bytes;
	{
		uint8_t *table = *(uint8_t *const *)(atlas_material + MATERIAL_TABLE);
		uint8_t before = table[6];
		// The material's texture def says "no mip filter" (state 0xe2: bits 0-1 the filter, linear; bits 2-3 the mip mode, off; bits 5-7 clamp), so the new levels
		// were never read, and the game's r_texFilterMipMode does not reach this sampler. Bits 2-3 = 2 is linear between the levels ([display] fontmip=0 leaves it).
		if (settings_fontmip())
			table[6] = (uint8_t)((table[6] & ~0x0c) | 0x08);
		log_printf("fonts: atlas texture def sampler state 0x%02x -> 0x%02x (mip filter %s)", before, table[6], settings_fontmip() ? "linear" : "as the game has it");
	}
	patched_image = image;
	patched_texture = tex;
	for (int n = 0; n < njobs; n++) {
		FontJob *j = &jobs[n];
		int p = j->plan_index;
		patched_fonts[p] = j->font;
		patched_glyphs[p] = *(uint8_t **)(j->font + FONT_GLYPHS);
		patched_materials[p] = *(const uint8_t **)(j->font + FONT_MATERIAL);
		patched_counts[p] = j->count;
		patched_signatures[p] = hash_bytes(patched_glyphs[p], (size_t)j->count * GLYPH_SIZE, 2166136261u);
	}
	patched = 1;
	log_printf("fonts: %d fonts now use the mod's glyphs (bank.ttf, default.otf, defaultBold.otf)", njobs);
	free_bitmaps();
	free(unique);
}

static void clear_patch_tracking(void)
{
	patched_image = NULL;
	patched_texture = NULL;
	memset(patched_fonts, 0, sizeof(patched_fonts));
	memset(patched_glyphs, 0, sizeof(patched_glyphs));
	memset(patched_materials, 0, sizeof(patched_materials));
	memset(patched_counts, 0, sizeof(patched_counts));
	memset(patched_signatures, 0, sizeof(patched_signatures));
}

static void run_patch(void *(*find)(int, const char *), const uint8_t *atlas_material)
{
	patched = 0;
	clear_patch_tracking();
	patch_all_impl(find, atlas_material);
	InterlockedExchange(&patch_state, patched ? 2 : 0);
}

static void patch_all(void *(*find)(int, const char *), const uint8_t *atlas_material)
{
	if (natboot_active()) // the atlas patch reads the renderer's Direct3D 9 texture objects: not adapted to the native renderer yet
		return;
	if (InterlockedCompareExchange(&patch_state, 1, 0) != 0)
		return;
	run_patch(find, atlas_material);
}

void fonts_seen(void *font, const char *name, void *(*find)(int, const char *))
{
	(void)name;
	if (!font)
		return;
	const uint8_t *f = (const uint8_t *)font;
	if (!readable(f, 0x30))
		return;
	const uint8_t *mat = *(const uint8_t *const *)(f + FONT_MATERIAL);
	if (!mat || !readable(mat, 8))
		return;
	const char *mname = *(const char *const *)mat;
	if (!mname || !readable(mname, 20) || strcmp(mname, "fonts/gamefonts_pc") != 0)
		return;
	uint8_t *image = material_image(mat);
	if (!image || !*(void **)image)
		return;
	if (!settings_fonts() || !localize_is_russian())
		return;
	// Font lookups can be frequent while zones are loading. The state is published with
	// InterlockedExchange after the patch is complete, so an acquire load is sufficient;
	// a no-op compare-exchange here would serialize every font lookup unnecessarily.
	LONG state = __atomic_load_n(&patch_state, __ATOMIC_ACQUIRE);
	if (state == 2) {
		int changed = image != patched_image || *(IDirect3DTexture9 **)image != patched_texture;
		for (int p = 0; p < PLAN_COUNT && !changed; p++) {
			uint8_t *current = find(24, plan[p].name);
			if (!current || !readable(current, 0x30))
				continue;
			const uint8_t *current_mat = *(const uint8_t *const *)(current + FONT_MATERIAL);
			if (!current_mat || !readable(current_mat, 8) || material_image(current_mat) != image)
				continue;
			const uint8_t *glyphs = *(const uint8_t *const *)(current + FONT_GLYPHS);
			int count = *(const int *)(current + FONT_COUNT);
			if (!patched_fonts[p] || current != patched_fonts[p] || current_mat != patched_materials[p] || glyphs != patched_glyphs[p] || count != patched_counts[p] || count <= 0 || count > 4096 || !readable(glyphs, (size_t)count * GLYPH_SIZE) || hash_bytes(glyphs, (size_t)count * GLYPH_SIZE, 2166136261u) != patched_signatures[p])
				changed = 1;
		}
		if (!changed)
			return;
		if (InterlockedCompareExchange(&patch_state, 1, 2) != 2)
			return;
		run_patch(find, mat);
		return;
	}
	patch_all(find, mat);
}

// A font lookup can arrive before loc_language and/or the shared atlas is ready. The
// localization hook calls this on ordinary asset lookups too, so a transient startup miss cannot
// disable fonts for the rest of the session. Keep the work bounded: at most one retry every 500 ms.
static volatile LONG retry_tick;
static volatile LONG waiting_language_logged;
static volatile LONG waiting_atlas_logged;

void fonts_retry(void *(*find)(int, const char *))
{
	if (natboot_active()) // the atlas patch is off with the native renderer (patch_all)
		return;
	// Called for ordinary non-font asset lookups too. These cached-state reads are on a
	// potentially very hot path; reserve locked operations for the occasional retry claim.
	if (!find || !settings_fonts() || __atomic_load_n(&patch_state, __ATOMIC_ACQUIRE) != 0)
		return;

	DWORD now = GetTickCount();
	LONG last = __atomic_load_n(&retry_tick, __ATOMIC_RELAXED);
	if ((DWORD)(now - (DWORD)last) < 500)
		return;
	if (InterlockedCompareExchange(&retry_tick, (LONG)now, last) != last)
		return;

	if (!localize_is_russian()) {
		if (InterlockedCompareExchange(&waiting_language_logged, 1, 0) == 0)
			log_printf("fonts: waiting for Russian game localization; will retry on later asset lookups");
		return;
	}
	InterlockedExchange(&waiting_language_logged, 0);

	for (int p = 0; p < PLAN_COUNT; p++) {
		uint8_t *font = (uint8_t *)find(24, plan[p].name);
		if (!font || !readable(font, 0x30))
			continue;
		const uint8_t *mat = *(const uint8_t *const *)(font + FONT_MATERIAL);
		if (!mat || !readable(mat, 8))
			continue;
		const char *mname = *(const char *const *)mat;
		if (!mname || !readable(mname, 20) || strcmp(mname, "fonts/gamefonts_pc") != 0)
			continue;
		uint8_t *image = material_image(mat);
		if (!image || !*(void **)image)
			continue;

		InterlockedExchange(&waiting_atlas_logged, 0);
		log_printf("fonts: retrying font replacement after the game assets became ready");
		patch_all(find, mat);
		return;
	}
	if (InterlockedCompareExchange(&waiting_atlas_logged, 1, 0) == 0)
		log_printf("fonts: Russian localization is active, waiting for the shared font atlas");
}

