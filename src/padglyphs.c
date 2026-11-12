// Pictures of a pad's buttons in the game's texts, as the console game shows them: a hint's "[{+usereload}]" names the key bound to the
// command (rva 0xd0e40: local client, command, two names of 0x80 chars; called at 0x2513ce by the text localizer and at 0x1913fa by the
// scripts' GetKeyBinding). For a player on a pad (his context is live while his half's texts are made) it gives a marker of the pad's
// button for that command in his layout instead: ESC and a letter, two bytes that pass through every string copy. Right before a text
// is drawn (the render thread's text command, rva 0x44b20, calls the text draw 0x433b0 at 0x44c12 with the text in rcx), a marker
// becomes the engine's own inline picture: '^', 1, width, height and the Material * (8 bytes: zeros in it, so it never goes through a
// string copy), the brackets around it dropped. The text draw scales width and height (bytes) as ((n - 16) * font height + 16) / 32.
// The pictures are drawn here into textures of copies of the "white" material (its texture table +0x60, 16-byte entries with the
// GfxImage * at +8; the GfxImage's first field the IDirect3DTexture9 *, width and height u16 at +0x18).
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "game.h"
#include "hook.h"
#include "log.h"
#include "modtex.h"
#include "padglyphs.h"

#ifdef _WIN64
#define RVA_DB_FIND XR(0x0ef5d0) // void *DB_FindXAssetHeader(int type, const char *name)
#define MATERIAL_TABLE 0x60
#define IMAGE_DIMS 0x18
#define TEXT_DRAW_SITE XR(0x44c12) // the text command's call of the text draw
#define BINDING_SITE_A XR(0x2513ce) // the text localizer's call of the binding function
#define BINDING_SITE_B XR(0x1913fa) // the scripts' GetKeyBinding
#else
// the 32-bit official build (docs/32bit-native-map/inputui.md)
#define RVA_DB_FIND XR(0x0f6f10)
#define MATERIAL_TABLE 0x54
#define IMAGE_DIMS 0x14
#define TEXT_DRAW_SITE XR(0x13919d)
#define BINDING_SITE_A XR(0x2235f6)
#define BINDING_SITE_B XR(0x1d51f6)
#endif
#define ASSET_MATERIAL 5
#define ICON 64
#define ICON_SIZE 54 // the text draw's size byte: (54 - 16) / 32 of the font's height ~ 1.2

enum { B_A, B_B, B_X, B_Y, B_LB, B_RB, B_LT, B_RT, B_LS, B_RS, B_BACK, B_START, B_UP, B_DOWN, B_LEFT, B_RIGHT, BUTTONS };
static const char *const fallback[BUTTONS] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "Back", "Start", "Up", "Down", "Left", "Right"};

// the game's commands and the pad buttons they are on (the layout of native.c's pad_frame_player; tactical: B and the right stick change places)
static int button_of(const char *cmd, int tactical)
{
	static const struct {
		const char *cmd;
		int button;
	} map[] = {
	    {"gostand", B_A},      {"raisestance", B_A},   {"stance", B_B},       {"movedown", B_B},          {"togglecrouch", B_B},
	    {"toggleprone", B_B},  {"prone", B_B},         {"lowerstance", B_B},  {"usereload", B_X},         {"activate", B_X},
	    {"reload", B_X},       {"weapnext", B_Y},      {"weapprev", B_Y},     {"smoke", B_LB},            {"frag", B_RB},
	    {"breath_sprint", B_LS}, {"sprint", B_LS},     {"holdbreath", B_LS},  {"melee", B_RS},            {"melee_breath", B_RS},
	    {"scores", B_BACK},    {"togglemenu", B_START}, {"attack", B_RT},     {"speed_throw", B_LT},      {"toggleads_throw", B_LT},
	    {"speed", B_LT},       {"actionslot 1", B_UP}, {"actionslot 2", B_DOWN}, {"actionslot 3", B_LEFT}, {"actionslot 4", B_RIGHT},
	};
	if (*cmd == '+' || *cmd == '-')
		cmd++;
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (!_stricmp(cmd, map[i].cmd)) {
			int b = map[i].button;
			if (tactical && b == B_B)
				return B_RS;
			if (tactical && b == B_RS)
				return B_B;
			return b;
		}
	return -1;
}

static uint8_t *base;
int padglyphs_log; // set by native.c under hudlog=3
static int (*pad_player_of)(int *);
static void *materials[BUTTONS];

// ---- the pictures (64 x 64, drawn with distance fields, 4 x 4 samples a pixel; the letters by GDI)
static float sd_rrect(float x, float y, float cx, float cy, float hw, float hh, float r)
{
	float qx = fabsf(x - cx) - hw + r, qy = fabsf(y - cy) - hh + r;
	float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
	return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

// the shape of a button: its distance (negative inside) and the fill colour; *lit: a d-pad arm drawn light
static float shape(int b, float x, float y, uint32_t *fill, int *lit)
{
	static const uint32_t face[4] = {0x2f9e36, 0xd2392c, 0x2a6fd6, 0xe6b512}; // A green, B red, X blue, Y yellow
	*lit = 0;
	*fill = 0x2b2b2b;
	if (b <= B_Y) {
		*fill = face[b];
		return sqrtf((x - 32) * (x - 32) + (y - 32) * (y - 32)) - 29;
	}
	if (b == B_LB || b == B_RB)
		return sd_rrect(x, y, 32, 32, 30, 15, 7);
	if (b == B_LT || b == B_RT)
		return sd_rrect(x, y, 32, 33, 21, 28, 11);
	if (b == B_LS || b == B_RS)
		return sqrtf((x - 32) * (x - 32) + (y - 32) * (y - 32)) - 28;
	if (b == B_BACK || b == B_START)
		return sd_rrect(x, y, 32, 32, 29, 17, 17);
	// the d-pad: a cross, the pressed arm lit
	float v = sd_rrect(x, y, 32, 32, 10, 29, 3), h = sd_rrect(x, y, 32, 32, 29, 10, 3);
	float d = v < h ? v : h;
	if (d < 0)
		*lit = (b == B_UP && y < 22) || (b == B_DOWN && y > 42) || (b == B_LEFT && x < 22) || (b == B_RIGHT && x > 42);
	return d;
}

// the label's coverage, rendered by GDI (antialiased, bold)
static void label_mask(const wchar_t *text, int px, float mask[ICON * ICON])
{
	memset(mask, 0, sizeof(float) * ICON * ICON);
	HDC dc = CreateCompatibleDC(NULL);
	BITMAPINFO bi;
	memset(&bi, 0, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader), bi.bmiHeader.biWidth = ICON, bi.bmiHeader.biHeight = -ICON, bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
	void *bits = NULL;
	HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!dc || !bmp || !bits) {
		if (bmp)
			DeleteObject(bmp);
		if (dc)
			DeleteDC(dc);
		return;
	}
	HGDIOBJ old_bmp = SelectObject(dc, bmp);
	memset(bits, 0, ICON * ICON * 4);
	HFONT font = CreateFontW(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Arial");
	HGDIOBJ old_font = SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(255, 255, 255));
	RECT r = {0, 0, ICON, ICON + 1};
	DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	GdiFlush();
	for (int i = 0; i < ICON * ICON; i++)
		mask[i] = ((const uint8_t *)bits)[i * 4 + 1] / 255.0f;
	SelectObject(dc, old_font);
	SelectObject(dc, old_bmp);
	DeleteObject(font);
	DeleteObject(bmp);
	DeleteDC(dc);
}

static void draw_icon(int b, uint8_t *bits, int pitch)
{
	static const wchar_t *const labels[BUTTONS] = {L"A", L"B", L"X", L"Y", L"LB", L"RB", L"LT", L"RT", L"LS", L"RS", L"\x25c0", L"\x25b6", L"", L"", L"", L""};
	static const int sizes[BUTTONS] = {40, 40, 40, 40, 22, 22, 22, 22, 22, 22, 22, 22, 0, 0, 0, 0};
	static float mask[ICON * ICON];
	label_mask(labels[b], sizes[b] ? sizes[b] : 10, mask);
	for (int py = 0; py < ICON; py++) {
		uint32_t *row = (uint32_t *)(bits + py * pitch);
		for (int px = 0; px < ICON; px++) {
			float r = 0, g = 0, bl = 0, a = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++) {
					uint32_t fill;
					int lit;
					float d = shape(b, px + (sx + 0.5f) / 4, py + (sy + 0.5f) / 4, &fill, &lit);
					if (d > 1.5f)
						continue;
					uint32_t c = d > -1.5f ? 0xdedede : lit ? 0xf2f2f2 : fill; // a light rim, then the fill
					if (d > 0) // a dark edge outside the rim keeps it apart from a light background
						c = 0x101010;
					r += (c >> 16 & 255) / 255.0f, g += (c >> 8 & 255) / 255.0f, bl += (c & 255) / 255.0f, a += 1;
				}
			if (a > 0)
				r /= a, g /= a, bl /= a;
			a /= 16;
			float m = sizes[b] ? mask[py * ICON + px] : 0; // the label in white over it
			r += (1 - r) * m, g += (1 - g) * m, bl += (1 - bl) * m;
			uint32_t A = (uint32_t)(a * 255 + 0.5f), R = (uint32_t)(r * 255 + 0.5f), G = (uint32_t)(g * 255 + 0.5f), B = (uint32_t)(bl * 255 + 0.5f);
			row[px] = A << 24 | R << 16 | G << 8 | B;
		}
	}
}

// the materials, made once the "white" material's texture is there (main thread)
static void make_materials(void)
{
	static int failed;
	if (materials[0] || failed > 3)
		return;
	uint8_t *white = ((uint8_t * (*)(int, const char *))(base + RVA_DB_FIND))(ASSET_MATERIAL, "white");
	uint8_t *table = white ? *(uint8_t **)(white + MATERIAL_TABLE) : NULL;
	uint8_t *image = table ? *(uint8_t **)(table + 8) : NULL;
	IDirect3DTexture9 *tex0 = image ? *(IDirect3DTexture9 **)image : NULL;
	if (!tex0) {
		failed++;
		return;
	}
	static char names[BUTTONS][24];
	for (int b = 0; b < BUTTONS; b++) {
		IDirect3DTexture9 *tex = modtex_create(tex0, ICON, ICON, 1, D3DFMT_A8R8G8B8);
		D3DLOCKED_RECT lr;
		if (!tex || FAILED(modtex_lock(tex, 0, &lr, 0))) {
			if (tex)
				modtex_release(tex);
			continue;
		}
		draw_icon(b, (uint8_t *)lr.pBits, lr.Pitch);
		modtex_unlock(tex, 0);
		uint8_t *mat = HeapAlloc(GetProcessHeap(), 0, 0x100), *tab = HeapAlloc(GetProcessHeap(), 0, 0x40), *img = HeapAlloc(GetProcessHeap(), 0, 0x80);
		memcpy(mat, white, 0x100); // (the copies read a little over the ends, as the menus' copies do)
		memcpy(tab, table, 0x40);
		memcpy(img, image, 0x80);
		*(IDirect3DTexture9 **)img = tex;
		*(uint16_t *)(img + IMAGE_DIMS) = ICON, *(uint16_t *)(img + IMAGE_DIMS + 2) = ICON;
		for (int i = 0; i < 4; i++)
			if (*(uint8_t **)(tab + i * 16 + 8) == image)
				*(uint8_t **)(tab + i * 16 + 8) = img;
		*(uint8_t **)(mat + MATERIAL_TABLE) = tab;
		lstrcpynA(names[b], "mw2coop_btn_", sizeof(names[b]));
		lstrcpynA(names[b] + 12, fallback[b], sizeof(names[b]) - 12);
		*(const char **)mat = names[b];
		materials[b] = mat;
	}
	log_printf("padglyphs: %d button pictures made", materials[0] ? BUTTONS : 0);
}

static unsigned (*binding_original[2])(intptr_t, const char *, char *);
static unsigned binding(int which, intptr_t lc, const char *cmd, char *out)
{
	int tactical = 0;
	int pl = pad_player_of && cmd ? pad_player_of(&tactical) : -1;
	int b = pl >= 0 ? button_of(cmd, tactical) : -1;
	if (b < 0)
		return binding_original[which](lc, cmd, out);
	make_materials();
	out[0] = 0x1b, out[1] = (char)('a' + b), out[2] = 0;
	out[0x80] = 0;
	return 1;
}
static unsigned binding0(intptr_t lc, const char *cmd, char *out)
{
	return binding(0, lc, cmd, out);
}
static unsigned binding1(intptr_t lc, const char *cmd, char *out)
{
	return binding(1, lc, cmd, out);
}

// A text with its markers made pictures, into out (cap bytes). map (optional, one entry per output byte and one for the end) gives for each output byte the offset in the
// text it came from. Returns the length, or -1 when the text did not fit.
static int convert(const char *text, char *out, size_t cap, uint16_t *map)
{
	size_t n = 0;
	const char *p = text;
	for (; *p; ) {
		if (n >= cap - 16)
			return -1;
		if (p[0] == 0x1b && p[1] >= 'a' && p[1] < 'a' + BUTTONS) {
			int b = p[1] - 'a';
			const char *start = p;
			p += 2;
			if (n && out[n - 1] == '[' && *p == ']')
				n--, p++;
			if (n >= 2 && out[n - 2] == '^' && out[n - 1] >= '0' && out[n - 1] <= '9')
				n -= 2; // the key's highlight colour ("^3", the localizer's): it would tint the picture
			size_t from = n;
			if (materials[b]) {
				out[n++] = '^', out[n++] = 1, out[n++] = ICON_SIZE, out[n++] = ICON_SIZE;
				memcpy(out + n, &materials[b], sizeof(void *));
				n += sizeof(void *); // (8 bytes in 64 bits, 4 in 32)
			} else
				for (const char *s = fallback[b]; *s; s++)
					out[n++] = *s;
			if (map)
				for (size_t i = from; i < n; i++)
					map[i] = (uint16_t)(start - text);
			continue;
		}
		if (map)
			map[n] = (uint16_t)(p - text);
		out[n++] = *p++;
	}
	if (map)
		map[n] = (uint16_t)(p - text);
	out[n] = 0;
	return (int)n;
}

// render thread: a text about to be drawn, its markers made pictures (the brackets around one dropped)
__attribute__((used)) const char *padglyphs_text(const char *text, float x, float y)
{
	static char buf[2][2048];
	static int at;
	if (!text || !strchr(text, 0x1b))
		return text;
#ifdef _WIN64
	static DWORD logged;
	if (padglyphs_log && GetTickCount() - logged > 1000) { // QA ([native] hudlog=3): where a text with a button picture is drawn (pixels)
		logged = GetTickCount();
		log_printf("padglyphs: text with a picture drawn at x %g y %g", x, y);
	}
#endif
	char *out = buf[at ^= 1];
	convert(text, out, sizeof(buf[0]), NULL);
	return out;
}

// ---- the sizes. The engine measures a text for centring, right alignment and line breaks before the draw makes the pictures, and sees a marker as two letters; the
// pictures are wider (the draw's width formula, the same as the line break function's own for a picture it finds).
#ifdef _WIN64
#define RVA_TEXT_WIDTH 0x9b30 // int (char *text, int max chars, Font *font): the width of the widest line, in the font's pixels
#define RVA_TEXT_WRAP 0x9c90 // char *(char *text, int max bytes, int width, Font *font, float scale): where the line ends
#define FONT_HEIGHT 8
static const uint8_t width_head[17] = {0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x56, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20};
static const uint8_t wrap_head[15] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x44, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54};
#else
// the official 32-bit build: the same two functions, cdecl, VA 0x508030 and 0x5080f0 (the font's pixel height at +4); in the iw4x-sp client XR() finds them, and hook_function
// leaves the code alone when its first bytes are not these
#define RVA_TEXT_WIDTH XR(0x108030)
#define RVA_TEXT_WRAP XR(0x1080f0)
#define FONT_HEIGHT 4
static const uint8_t width_head[5] = {0x51, 0x53, 0x55, 0x33, 0xed}; // push ecx; push ebx; push ebp; xor ebp, ebp
static const uint8_t wrap_head[5] = {0x83, 0xec, 0x0c, 0x53, 0x55};   // sub esp, 0xc; push ebx; push ebp
#endif
static int (*width_original)(char *, int, void *);
static char *(*wrap_original)(char *, int, int, void *, float);

static int picture_width(void *font)
{
	return font ? ((ICON_SIZE - 16) * *(int *)((uint8_t *)font + FONT_HEIGHT) + 16) / 32 : 0;
}

static int text_width(char *text, int max_chars, void *font)
{
	char buf[2048];
	if (!text || !strchr(text, 0x1b) || max_chars > 0)
		return width_original(text, max_chars, font);
	int len = convert(text, buf, sizeof(buf), NULL);
	if (len < 0)
		return width_original(text, max_chars, font);
	int line = 0, widest = 0, run = 0;
	for (int i = 0; i <= len; i++) {
		int picture = i < len && buf[i] == '^' && buf[i + 1] == 1, stop = i == len || picture || buf[i] == '\n' || buf[i] == '\r';
		if (!stop)
			continue;
		if (i > run) { // the plain part before it
			char saved = buf[i];
			buf[i] = 0;
			line += width_original(buf + run, 0, font);
			buf[i] = saved;
		}
		if (picture) {
			line += picture_width(font);
			i += 3 + sizeof(void *); // '^' is at i; 1, width, height and the pointer follow
			run = i + 1;
		} else {
			if (line > widest)
				widest = line;
			line = 0;
			run = i + 1;
		}
	}
	return line > widest ? line : widest;
}

static char *text_wrap(char *text, int max_bytes, int width, void *font, float scale)
{
	char buf[2048];
	uint16_t map[2048 + 1];
	if (!text || !strchr(text, 0x1b) || max_bytes > 0)
		return wrap_original(text, max_bytes, width, font, scale);
	int len = convert(text, buf, sizeof(buf), map);
	if (len < 0)
		return wrap_original(text, max_bytes, width, font, scale);
	char *end = wrap_original(buf, 0, width, font, scale); // (it finds a picture's size itself)
	long at = end - buf;
	if (at < 0 || at > len)
		return wrap_original(text, max_bytes, width, font, scale);
	return text + map[at];
}

// the stub at the text command's call of the text draw: the text (rcx) through padglyphs_text, every other argument (rdx, r8, r9, xmm0-3, the stack) as it was
#ifdef _WIN64
__attribute__((used)) static void *draw_original;
void padglyphs_draw_stub(void);
__asm__(".text\n"
	".globl padglyphs_draw_stub\n"
	"padglyphs_draw_stub:\n"
	"\tpush %rdx\n"
	"\tpush %r8\n"
	"\tpush %r9\n"
	"\tpush %rcx\n"
	"\tsub $0x68, %rsp\n"
	"\tmovdqu %xmm0, 0x20(%rsp)\n"
	"\tmovdqu %xmm1, 0x30(%rsp)\n"
	"\tmovdqu %xmm2, 0x40(%rsp)\n"
	"\tmovdqu %xmm3, 0x50(%rsp)\n"
	"\tcall padglyphs_text\n"
	"\tmovdqu 0x20(%rsp), %xmm0\n"
	"\tmovdqu 0x30(%rsp), %xmm1\n"
	"\tmovdqu 0x40(%rsp), %xmm2\n"
	"\tmovdqu 0x50(%rsp), %xmm3\n"
	"\tadd $0x68, %rsp\n"
	"\tpop %rcx\n"
	"\tpop %r9\n"
	"\tpop %r8\n"
	"\tpop %rdx\n"
	"\tmov %rax, %rcx\n"
	"\tjmp *draw_original(%rip)\n");

#else
// 32 bits: the text is the first stack argument of the text draw
__attribute__((used)) void *draw_original;
void padglyphs_draw_stub(void);
__asm__(".text\n"
	".globl _padglyphs_draw_stub\n"
	"_padglyphs_draw_stub:\n"
	"\tpushl 4(%esp)\n"
	"\tcall _padglyphs_text\n"
	"\taddl $4, %esp\n"
	"\tmovl %eax, 4(%esp)\n"
	"\tjmp *_draw_original\n");
#endif


#ifdef _WIN64
// QA ([native] hudlog=3): the rectangle the cursor hint's draw (rva 0xac1f0: local client, rect x y w h, font, scale, style) is given
#define RVA_HINT_DRAW XR(0xac1f0)
static const uint8_t hint_head[14] = {0x4c, 0x8b, 0xdc, 0x55, 0x56, 0x41, 0x55, 0x48, 0x81, 0xec, 0x20, 0x03, 0x00, 0x00};
static void (*hint_original)(int, float *, void *, float, int);
static void hint_hook(int lc, float *rect, void *font, float scale, int style)
{
	static DWORD last;
	if (padglyphs_log && rect && GetTickCount() - last > 1000) {
		last = GetTickCount();
		log_printf("padglyphs: hint draw: client %d rect %g %g %g %g scale %g", lc, rect[0], rect[1], rect[2], rect[3], scale);
		static int dumped;
		if (dumped++ < 2) // the engine's screen placements (rva 0x5e0b80, 0x70 bytes each; six of them in case the console's four local clients have their own)
			for (int n = 0; n < 6; n++) {
				const float *q = (const float *)(base + XR(0x5e0b80) + n * 0x70);
				log_printf("padglyphs: placement %d: scale %g %g | full %g %g | viewport %g %g %g %g | virt view %g %g %g %g | real view %g %g %g %g | virt adj %g %g %g %g | real adj %g %g %g %g | %g %g", n, q[0], q[1], q[2], q[3], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15], q[16], q[17], q[18], q[19], q[20], q[21], q[22], q[23], q[24], q[25], q[26], q[27]);
			}
	}
	hint_original(lc, rect, font, scale, style);
}
#endif

void padglyphs_install(int (*pad_player)(int *tactical))
{
	base = (uint8_t *)GetModuleHandleA(NULL);
	pad_player_of = pad_player;
	// the draw first: without it a marker would be drawn as two letters
	int ok = hook_call(base + TEXT_DRAW_SITE, (void *)padglyphs_draw_stub, &draw_original);
	if (ok) {
		ok &= hook_call(base + BINDING_SITE_A, (void *)binding0, (void **)&binding_original[0]);
		ok &= hook_call(base + BINDING_SITE_B, (void *)binding1, (void **)&binding_original[1]);
	}
	log_printf("padglyphs: %s", ok ? "pad buttons as pictures in texts" : "a call site is not as expected, some texts keep key names");
#ifdef _WIN64
	hook_function(base + RVA_HINT_DRAW, hint_head, sizeof(hint_head), (void *)hint_hook, (void **)&hint_original);
#endif
	char ini[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, ini, MAX_PATH);
	while (n > 0 && ini[n - 1] != 0x5c)
		n--;
	ini[n] = 0;
	lstrcatA(ini, "mw2coop\\settings.ini");
	if (ok && GetPrivateProfileIntA("native", "padwidth", 1, ini)) { // [native] padwidth=0: the engine's own widths (a marker counts as two letters)
		int w = hook_function(base + RVA_TEXT_WIDTH, width_head, sizeof(width_head), (void *)text_width, (void **)&width_original);
		int r = hook_function(base + RVA_TEXT_WRAP, wrap_head, sizeof(wrap_head), (void *)text_wrap, (void **)&wrap_original);
		log_printf("padglyphs: text width %s, line break %s", w ? "counts the pictures" : "NOT hooked", r ? "counts the pictures" : "NOT hooked");
	}
}
