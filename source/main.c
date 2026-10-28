// Canvas 3DS: courses, assignments, announcements, modules, pages, grades and inbox
// from Canvas LMS, through its official REST API (personal access token).
//
// Screens: the top shows details (or the first part of a document), the bottom is the
// touch list (or the rest of the document). B goes back, A opens, X reloads.
#include "app.h"
#include "updater.h"
#include <curl/curl.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOP_W 400
#define BOT_W 320
#define HDR_H 24          // bottom header
#define FOOT_Y 214        // bottom footer starts here
#define TOP_BAR 22
#define ROW_H 32
#define HROW_H 20
#define DOC_TOP_H (240 - TOP_BAR)
#define DOC_BOT_H (FOOT_Y - HDR_H)

static C3D_RenderTarget *t_top, *t_bot;
static View *stack[16];
static int depth;
static char toast_msg[160];
static u64 toast_until, now_ms;

typedef struct { float x, y, w, h; int id; } Btn;
static Btn btns[16];
static int nbtns;
enum { B_BACK = 1, B_RELOAD, B_ACTION, B_SETTINGS, B_HOME, B_TZ_DN, B_TZ_UP };

static bool touch_down, dragging;
static int touch_y0, touch_x0, last_ty;
static float scroll0;

void toast(const char *m)
{
	snprintf(toast_msg, sizeof toast_msg, "%s", m);
	toast_until = osGetTime() + 3000;
}

bool keyboard(const char *hint, char *out, size_t n, bool multiline)
{
	SwkbdState kb;
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, (int)n - 1);
	swkbdSetHintText(&kb, hint);
	swkbdSetInitialText(&kb, out);
	if (multiline) swkbdSetFeatures(&kb, SWKBD_MULTILINE);
	swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, "Cancelar", false);
	swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "OK", true);
	char buf[2048];
	if (n > sizeof buf) n = sizeof buf;
	SwkbdButton b = swkbdInputText(&kb, buf, n);
	if (b != SWKBD_BUTTON_RIGHT) return false;
	snprintf(out, n, "%s", buf);
	return true;
}

// ---------------------------------------------------------------- navigation

static View *top(void) { return depth ? stack[depth - 1] : NULL; }

static void push(View *v)
{
	if (!v) return;
	if (depth == 16) { view_free(stack[1]); memmove(stack + 1, stack + 2, 14 * sizeof(View *)); depth--; }
	stack[depth++] = v;
	if (v->loader != L_STATIC && v->loader != L_SETTINGS && v->loader != L_SETUP && v->loader != L_COURSE &&
	    !(v->loader == L_ASSIGNMENT && v->doc_html))
		view_start(v);
}

static void pop(void)
{
	if (depth <= 1) return;
	view_free(stack[--depth]);
	View *t = top();
	if (t && t->loader == L_SETTINGS) view_refresh_settings(t);
	// a screen that never loaded (e.g. home before the token existed) loads now
	if (t && t->kind == V_LIST && !t->nrows && !t->loading && t->loader != L_COURSE && t->loader != L_SETTINGS) view_start(t);
}

static void go_home(void)
{
	while (depth > 1) pop();
}

static bool has_token(void) { return g_cfg.token[0] != 0; }

static void reload(void)
{
	View *v = top();
	if (!v) return;
	if (v->loader == L_SETTINGS) { view_refresh_settings(v); return; }
	if (v->loader == L_SETUP || v->loader == L_STATIC || v->loader == L_COURSE) return;
	if (v->loader == L_ASSIGNMENT && v->id[0]) { free(v->doc_html); v->doc_html = NULL; }
	view_start(v);
}

// ---------------------------------------------------------------- list geometry

static float row_h(const Row *r) { return r->act == A_HEADER ? HROW_H : ROW_H; }

static float row_y(const View *v, int i)
{
	float y = 0;
	for (int k = 0; k < i; k++) y += row_h(&v->rows[k]);
	return y;
}

static float list_h(const View *v) { return row_y(v, v->nrows); }

static bool selectable(const Row *r) { return r->act != A_HEADER && r->act != A_NONE; }

static void clamp_scroll(View *v)
{
	float maxs = v->kind == V_LIST ? list_h(v) - DOC_BOT_H : html_height(v->doc) - DOC_TOP_H - DOC_BOT_H + 20;
	if (maxs < 0) maxs = 0;
	if (v->scroll > maxs) { v->scroll = maxs; v->vel = 0; }
	if (v->scroll < 0) { v->scroll = 0; v->vel = 0; }
}

static void ensure_visible(View *v)
{
	if (v->sel < 0 || v->sel >= v->nrows) return;
	float y = row_y(v, v->sel), h = row_h(&v->rows[v->sel]);
	if (y < v->scroll) v->scroll = y - (v->sel > 0 && v->rows[v->sel - 1].act == A_HEADER ? HROW_H : 0);
	if (y + h > v->scroll + DOC_BOT_H) v->scroll = y + h - DOC_BOT_H;
	clamp_scroll(v);
}

static void move_sel(View *v, int dir)
{
	int i = v->sel;
	for (int n = 0; n < v->nrows; n++) {
		i += dir;
		if (i < 0 || i >= v->nrows) return;
		if (selectable(&v->rows[i])) { v->sel = i; ensure_visible(v); return; }
	}
}

static void fix_sel(View *v)
{
	if (v->sel >= 0 && v->sel < v->nrows && selectable(&v->rows[v->sel])) return;
	for (int i = 0; i < v->nrows; i++) if (selectable(&v->rows[i])) { v->sel = i; return; }
}

// ---------------------------------------------------------------- drawing helpers

static void reg_btn(float x, float y, float w, float h, int id)
{
	if (nbtns < 16) btns[nbtns++] = (Btn){ x, y, w, h, id };
}

static void button(float x, float y, float w, float h, const char *label, int id, bool primary)
{
	C2D_DrawRectSolid(x, y, 0.5f, w, h, primary ? C_ACCENT : C_BAR2);
	text_draw(label, x + w / 2, y + (h - 14) / 2, 0.45f, C_WHITE, w - 6, 1);
	reg_btn(x, y, w, h, id);
}

static void spinner(float cx, float cy, u32 c)
{
	float a = (now_ms % 1000) / 1000.0f * 6.2832f;
	for (int i = 0; i < 8; i++) {
		float t = a + i * 0.785f;
		u8 al = 40 + i * 26;
		C2D_DrawCircleSolid(cx + cosf(t) * 7, cy + sinf(t) * 7, 0.5f, 1.8f, (c & 0x00FFFFFF) | ((u32)al << 24));
	}
}

static Doc *g_preview;
static const Row *g_preview_row;
static int g_preview_size = -1;

static void preview_doc(const Row *r)
{
	if (r == g_preview_row && g_preview_size == g_cfg.text_size) return;
	html_free(g_preview);
	g_preview = NULL;
	g_preview_row = r;
	g_preview_size = g_cfg.text_size;
	if (r && r->html) g_preview = html_layout(r->html, 380, doc_scale() * 0.95f);
}

// ---------------------------------------------------------------- top screen

static void draw_top(void)
{
	C2D_TargetClear(t_top, C_BG);
	C2D_SceneBegin(t_top);
	View *v = top();

	C2D_DrawRectSolid(0, 0, 0.5f, TOP_W, TOP_BAR, C_BAR);
	C2D_DrawRectSolid(0, TOP_BAR - 2, 0.5f, TOP_W, 2, C_ACCENT);
	char hdr[160];
	snprintf(hdr, sizeof hdr, "Canvas 3DS%s%s", g_user_name[0] ? " · " : "", g_user_name);
	text_draw(hdr, 8, 3, 0.48f, C_WHITE, 250, 0);
	time_t t = time(NULL);
	struct tm tm;
	gmtime_r(&t, &tm);
	char clock[16];
	snprintf(clock, sizeof clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
	text_draw(clock, TOP_W - 8, 3, 0.48f, C_WHITE, 0, 2);
	if (net_busy()) spinner(TOP_W - 62, 11, C_WHITE);

	if (!v) return;
	if (v->kind == V_DOC) {
		if (v->doc) html_draw(v->doc, v->scroll, DOC_TOP_H, (TOP_W - doc_width()) / 2, TOP_BAR, v->focus);
		if (v->loading) { spinner(200, 120, C_SUB); text_draw("Carregando...", 200, 140, 0.45f, C_SUB, 0, 1); }
		return;
	}

	// list: details of the selected row
	const Row *r = v->sel >= 0 && v->sel < v->nrows ? &v->rows[v->sel] : NULL;
	if (v->loading && !v->nrows) {
		spinner(200, 110, C_SUB);
		text_draw("Carregando...", 200, 130, 0.45f, C_SUB, 0, 1);
		return;
	}
	if (v->err[0]) {
		text_draw("Algo deu errado", 200, 80, 0.6f, C_RED, 380, 1);
		char l1[200], l2[200];
		text_wrap2(v->err, 0.45f, 380, l1, l2, sizeof l1);
		text_draw(l1, 200, 110, 0.45f, C_TEXT, 380, 1);
		text_draw(l2, 200, 128, 0.45f, C_TEXT, 380, 1);
		text_draw("X: tentar de novo", 200, 160, 0.42f, C_SUB, 0, 1);
		return;
	}
	if (!r) return;
	preview_doc(r);
	if (g_preview) {
		html_draw(g_preview, 0, DOC_TOP_H - 6, 10, TOP_BAR + 6, -1);
		if (html_height(g_preview) > DOC_TOP_H - 6) {
			C2D_DrawRectSolid(0, 222, 0.5f, TOP_W, 18, C_BG);
			text_draw("A: abrir para ler tudo", 200, 223, 0.4f, C_SUB, 0, 1);
		}
		return;
	}
	// no document: a big card with the row
	char l1[200], l2[200];
	int n = text_wrap2(r->title, 0.62f, 380, l1, l2, sizeof l1);
	text_draw(l1, 12, 40, 0.62f, C_TEXT, 380, 0);
	if (n > 1) text_draw(l2, 12, 62, 0.62f, C_TEXT, 380, 0);
	float y = n > 1 ? 92 : 70;
	if (r->sub) { text_draw(r->sub, 12, y, 0.45f, C_SUB, 380, 0); y += 20; }
	if (r->right) text_draw(r->right, 12, y, 0.5f, r->right_color, 380, 0);
	const char *hint = r->act == A_COURSE ? "A: abrir o curso" : r->act == A_SETTINGS && r->section == 4 ? "Direcional esquerda/direita: mudar o fuso" :
	                   selectable(r) ? "A: abrir" : NULL;
	if (hint) text_draw(hint, 200, 215, 0.42f, C_SUB, 0, 1);
}

// ---------------------------------------------------------------- bottom screen

static void draw_row(const View *v, int i, float y)
{
	const Row *r = &v->rows[i];
	if (r->act == A_HEADER) {
		C2D_DrawRectSolid(0, y, 0.5f, BOT_W, HROW_H, C2D_Color32(232, 235, 240, 255));
		text_draw(r->title, 8, y + 3, 0.42f, C_BAR2, 300, 0);
		return;
	}
	bool s = i == v->sel && selectable(r);
	C2D_DrawRectSolid(0, y, 0.5f, BOT_W, ROW_H, s ? C_SEL : C_CARD);
	C2D_DrawRectSolid(8, y + ROW_H - 1, 0.5f, BOT_W - 8, 1, C_LINE);
	if (s) C2D_DrawRectSolid(0, y, 0.5f, 3, ROW_H, C_ACCENT);
	if (r->dot) C2D_DrawCircleSolid(12, y + 11, 0.5f, 3.2f, r->dot);
	float rw = r->right ? text_width(r->right, 0.4f) + 8 : 0;
	text_draw(r->title, 22, y + 3, 0.46f, r->act == A_NONE ? C_SUB : C_TEXT, BOT_W - 30 - rw, 0);
	if (r->sub) text_draw(r->sub, 22, y + 17, 0.37f, C_SUB, BOT_W - 30, 0);
	if (r->right) text_draw(r->right, BOT_W - 6, y + 5, 0.4f, r->right_color, 120, 2);
}

static void draw_bottom(void)
{
	C2D_TargetClear(t_bot, C_BG);
	C2D_SceneBegin(t_bot);
	nbtns = 0;
	View *v = top();
	if (!v) return;

	// content
	if (v->kind == V_LIST) {
		float y0 = HDR_H - v->scroll;
		float y = y0;
		for (int i = 0; i < v->nrows; i++) {
			float h = row_h(&v->rows[i]);
			if (y + h >= HDR_H && y < FOOT_Y) draw_row(v, i, y);
			y += h;
			if (y > FOOT_Y) break;
		}
		if (v->loading) { spinner(160, 120, C_SUB); }
		if (v->err[0] && !v->nrows) {
			text_draw("Não deu para carregar.", 160, 100, 0.48f, C_RED, 300, 1);
			text_draw("X: tentar de novo", 160, 122, 0.42f, C_SUB, 0, 1);
		}
	} else {
		if (v->doc) html_draw(v->doc, v->scroll + DOC_TOP_H, DOC_BOT_H, (BOT_W - doc_width()) / 2, HDR_H, v->focus);
		else if (v->loading) spinner(160, 120, C_SUB);
		else if (v->err[0]) {
			char l1[200], l2[200];
			text_wrap2(v->err, 0.45f, 300, l1, l2, sizeof l1);
			text_draw(l1, 160, 90, 0.45f, C_RED, 300, 1);
			text_draw(l2, 160, 108, 0.45f, C_RED, 300, 1);
			text_draw("X: tentar de novo", 160, 140, 0.42f, C_SUB, 0, 1);
		}
		// scroll bar
		float total = html_height(v->doc);
		float view = DOC_TOP_H + DOC_BOT_H;
		if (total > view) {
			float h = (FOOT_Y - HDR_H) * view / total;
			if (h < 16) h = 16;
			float y = HDR_H + (FOOT_Y - HDR_H - h) * (v->scroll / (total - view + 20));
			if (y > FOOT_Y - h) y = FOOT_Y - h;
			C2D_DrawRectSolid(BOT_W - 3, y, 0.5f, 2, h, C2D_Color32(170, 175, 185, 255));
		}
	}

	// header
	C2D_DrawRectSolid(0, 0, 0.5f, BOT_W, HDR_H, C_BAR);
	if (depth > 1) {
		text_draw("<", 10, 3, 0.55f, C_WHITE, 0, 0);
		reg_btn(0, 0, 40, HDR_H, B_BACK);
	}
	text_draw(v->title, depth > 1 ? 28 : 8, 4, 0.47f, C_WHITE, BOT_W - (depth > 1 ? 36 : 16), 0);

	// footer
	C2D_DrawRectSolid(0, FOOT_Y, 0.5f, BOT_W, 240 - FOOT_Y, C_BAR);
	if (v->note[0]) {
		text_draw(v->note, 6, FOOT_Y + 6, 0.4f, C_WHITE, 196, 0);
	} else if (depth > 1) {
		button(4, FOOT_Y + 3, 60, 20, "Voltar", B_BACK, false);
		button(68, FOOT_Y + 3, 60, 20, "Início", B_HOME, false);
	} else {
		button(4, FOOT_Y + 3, 70, 20, "Atualizar", B_RELOAD, false);
	}
	if (v->footer == FB_REPLY) button(208, FOOT_Y + 3, 108, 20, "Y: Responder", B_ACTION, true);
	else if (v->footer == FB_DOWNLOAD) button(208, FOOT_Y + 3, 108, 20, "Y: Baixar", B_ACTION, true);
	else if (v->loader == L_SETTINGS && v->sel >= 0 && v->sel < v->nrows && v->rows[v->sel].section == 4) {
		button(208, FOOT_Y + 3, 52, 20, "-", B_TZ_DN, false);
		button(264, FOOT_Y + 3, 52, 20, "+", B_TZ_UP, false);
	} else if (v->loader != L_SETTINGS) button(236, FOOT_Y + 3, 80, 20, "Ajustes", B_SETTINGS, false);

	// toast
	if (now_ms < toast_until) {
		float w = text_width(toast_msg, 0.45f) + 20;
		if (w > 310) w = 310;
		C2D_DrawRectSolid(160 - w / 2, 176, 0.5f, w, 26, C2D_Color32(20, 24, 30, 230));
		text_draw(toast_msg, 160, 181, 0.45f, C_WHITE, 300, 1);
	}
}

// ---------------------------------------------------------------- input

static void change_tz(int d)
{
	g_cfg.tz += d;
	if (g_cfg.tz < -720) g_cfg.tz = -720;
	if (g_cfg.tz > 840) g_cfg.tz = 840;
	config_save();
	view_refresh_settings(top());
}

static void open_selected(void)
{
	View *v = top();
	if (!v) return;
	if (v->kind == V_LIST) {
		if (v->sel < 0 || v->sel >= v->nrows || !selectable(&v->rows[v->sel])) return;
		Row *r = &v->rows[v->sel];
		int before = g_cfg.text_size;
		View *n = view_open_row(v, r);
		if (before != g_cfg.text_size) {
			// re-layout open documents with the new size
			for (int i = 0; i < depth; i++) if (stack[i]->doc_html) { char *h = xstrdup(stack[i]->doc_html); view_set_doc(stack[i], h); free(h); }
		}
		push(n);
	} else if (v->focus >= 0) {
		push(view_open_link(v, html_link(v->doc, v->focus)));
	}
}

static void press(int id)
{
	View *v = top();
	switch (id) {
	case B_BACK: pop(); break;
	case B_HOME: go_home(); break;
	case B_RELOAD: reload(); break;
	case B_SETTINGS: push(view_settings()); break;
	case B_TZ_DN: change_tz(-30); break;
	case B_TZ_UP: change_tz(30); break;
	case B_ACTION:
		if (v && v->footer == FB_REPLY) view_reply(v);
		else if (v && v->footer == FB_DOWNLOAD) view_download(v);
		break;
	}
}

static void handle_touch(u32 down, u32 held, u32 up)
{
	View *v = top();
	touchPosition tp;
	hidTouchRead(&tp);
	if (down & KEY_TOUCH) {
		touch_down = true;
		dragging = false;
		touch_x0 = tp.px;
		touch_y0 = last_ty = tp.py;
		scroll0 = v ? v->scroll : 0;
		if (v) v->vel = 0;
		return;
	}
	if ((held & KEY_TOUCH) && touch_down && v) {
		if (abs(tp.py - touch_y0) > 6 && touch_y0 > HDR_H && touch_y0 < FOOT_Y) dragging = true;
		if (dragging) {
			v->vel = (float)(last_ty - tp.py);
			v->scroll = scroll0 + (touch_y0 - tp.py);
			clamp_scroll(v);
		}
		last_ty = tp.py;
		return;
	}
	if ((up & KEY_TOUCH) && touch_down) {
		touch_down = false;
		if (dragging || !v) return;
		int x = touch_x0, y = touch_y0;
		for (int i = 0; i < nbtns; i++) {
			Btn *b = &btns[i];
			if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) { press(b->id); return; }
		}
		if (y < HDR_H || y >= FOOT_Y) return;
		if (v->kind == V_LIST) {
			float ly = y - HDR_H + v->scroll, acc = 0;
			for (int i = 0; i < v->nrows; i++) {
				float h = row_h(&v->rows[i]);
				if (ly >= acc && ly < acc + h) {
					if (!selectable(&v->rows[i])) return;
					v->sel = i;
					open_selected();
					return;
				}
				acc += h;
			}
		} else if (v->doc) {
			float dx = x - (BOT_W - doc_width()) / 2, dy = y - HDR_H + v->scroll + DOC_TOP_H;
			int l = html_hit(v->doc, dx, dy);
			if (l >= 0) { v->focus = l; push(view_open_link(v, html_link(v->doc, l))); }
		}
	}
}

static void handle_keys(u32 down, u32 rep, u32 held)
{
	View *v = top();
	if (!v) return;
	if (down & KEY_B) { if (depth > 1) pop(); return; }
	if (down & KEY_X) { reload(); return; }
	if (down & KEY_SELECT) { push(view_settings()); return; }
	if (down & KEY_Y) {
		if (v->footer == FB_REPLY) view_reply(v);
		else if (v->footer == FB_DOWNLOAD) view_download(v);
		else if (v->loader == L_SETUP || (v->loader == L_SETTINGS)) {
			char t[256] = "";
			if (keyboard("Cole ou digite o token do Canvas", t, sizeof t, false) && t[0]) {
				config_save_token(t);
				net_set_auth(g_cfg.token);
				toast("Token salvo.");
				go_home();
				reload();
			}
		}
		return;
	}
	if (down & KEY_A) { open_selected(); return; }

	circlePosition cp;
	hidCircleRead(&cp);
	float stick = abs(cp.dy) > 20 ? -cp.dy / 12.0f : 0;

	if (v->kind == V_LIST) {
		if (rep & KEY_DUP) move_sel(v, -1);
		if (rep & KEY_DDOWN) move_sel(v, 1);
		if (v->loader == L_SETTINGS && v->sel >= 0 && v->sel < v->nrows && v->rows[v->sel].section == 4) {
			if (rep & KEY_DLEFT) change_tz(-30);
			if (rep & KEY_DRIGHT) change_tz(30);
		}
		if (rep & (KEY_R | KEY_L)) {
			int step = (rep & KEY_R) ? 6 : -6;
			for (int i = 0; i < 6; i++) move_sel(v, step > 0 ? 1 : -1);
		}
		if (stick) { v->scroll += stick; clamp_scroll(v); }
	} else {
		float view = DOC_TOP_H + DOC_BOT_H;
		if (rep & KEY_DUP) v->scroll -= 24;
		if (rep & KEY_DDOWN) v->scroll += 24;
		if (rep & KEY_R) v->scroll += view - 40;
		if (rep & KEY_L) v->scroll -= view - 40;
		v->scroll += stick;
		clamp_scroll(v);
		// links: left/right walk through the links on screen
		if (rep & (KEY_DRIGHT | KEY_DLEFT)) {
			int dir = (rep & KEY_DRIGHT) ? 1 : -1;
			int f = v->focus;
			float fy = f >= 0 ? html_link_y(v->doc, f) : -1;
			bool visible = f >= 0 && fy >= v->scroll && fy < v->scroll + view;
			int n = visible ? html_link_near(v->doc, f, dir, v->scroll, view) : html_link_near(v->doc, -1, dir, v->scroll, view);
			if (n >= 0) {
				v->focus = n;
				float y = html_link_y(v->doc, n);
				if (y < v->scroll || y > v->scroll + view - 30) { v->scroll = y - 40; clamp_scroll(v); }
			}
		}
	}
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
	updater_init("ChunoB/Canvas3DS", APP_VERSION, "Canvas3DS.3dsx", argc > 0 ? argv[0] : NULL);
	romfsInit();
	gfxInitDefault();
	osSetSpeedupEnable(true);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(6144);
	C2D_Prepare();
	t_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	t_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	text_init();
	hidSetRepeatParameters(18, 4);

	u32 *socbuf = memalign(0x1000, 0x100000);
	bool net_ok = socbuf && R_SUCCEEDED(socInit(socbuf, 0x100000));
	curl_global_init(CURL_GLOBAL_DEFAULT);
	config_load();
	net_init("romfs:/cacert.pem");
	net_set_auth(g_cfg.token);

	View *home = view_new(V_LIST, L_HOME, "Início");
	if (has_token()) push(home);
	else {
		// first run: show how to get a token; the home screen loads once there is one
		stack[depth++] = home;
		Row r = { .act = A_SETUP };
		push(view_open_row(home, &r));
	}
	if (!net_ok) toast("Rede indisponível");

	u64 t_start = osGetTime();
	bool upd_checked = false, upd_told = false;

	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp(), rep = hidKeysDownRepeat();
		now_ms = osGetTime();
		if (down & KEY_START) break;

		if (net_ok && !upd_checked && now_ms - t_start > 6000) { upd_checked = true; updater_check(); }
		if (!upd_told && updater_state() == UPD_AVAILABLE) {
			upd_told = true;
			char m[100];
			snprintf(m, sizeof m, "Versão %s disponível: Ajustes > Atualização", updater_latest());
			toast(m);
		}
		static UpdState last_us;
		if (updater_state() != last_us) { last_us = updater_state(); view_refresh_settings(top()); }

		for (int i = 0; i < depth; i++) view_poll(stack[i]);
		View *v = top();
		if (v && v->kind == V_LIST) fix_sel(v);

		handle_keys(down, rep, held);
		handle_touch(down, held, up);

		// fling
		v = top();
		if (v && !touch_down && fabsf(v->vel) > 0.3f) { v->scroll += v->vel; v->vel *= 0.92f; clamp_scroll(v); }
		if (v && v->note[0] && !v->job3 && now_ms % 6000 < 20) v->note[0] = 0;

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		draw_top();
		draw_bottom();
		C3D_FrameEnd(0);
	}

	while (depth) view_free(stack[--depth]);
	html_free(g_preview);
	updater_exit();
	net_exit();
	curl_global_cleanup();
	if (net_ok) socExit();
	text_exit();
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	romfsExit();
	return 0;
}
