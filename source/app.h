#pragma once
#include "ui.h"
#include "html.h"
#include "json.h"
#include "net.h"
#include <time.h>

// ---------- settings (config.c) ----------
typedef struct {
	char domain[128];   // einstein.instructure.com
	char token[256];    // personal access token (kept on the SD card only)
	int  tz;            // minutes from UTC (Brasília = -180)
	int  text_size;     // 0 small, 1 normal, 2 large
} Config;

extern Config g_cfg;
void config_load(void);
void config_save(void);
bool config_save_token(const char *tok);

// ---------- views (views.c) ----------
typedef enum {
	A_NONE, A_HEADER, A_COURSE, A_SECTION, A_ASSIGNMENT, A_ANNOUNCEMENT, A_PAGE, A_DISCUSSION,
	A_MODULE_ITEM, A_FILE, A_INBOX, A_CONVERSATION, A_ALL_ANNOUNCEMENTS, A_SETTINGS, A_URL,
	A_QUIZ, A_SETUP, A_DOCROW,
} Act;

typedef struct {
	char *title, *sub, *right, *html, *extra;
	u32   right_color, dot;
	Act   act;
	char  id[32], course[32];
	int   section;           // for A_SECTION / setting rows
} Row;

typedef enum { V_LIST, V_DOC } VKind;

typedef enum {
	L_NONE, L_HOME, L_COURSE, L_ANNOUNCEMENTS, L_ASSIGNMENTS, L_GRADES, L_MODULES, L_PAGES, L_PAGE,
	L_DISCUSSIONS, L_DISCUSSION, L_FILES, L_FILE, L_INBOX, L_CONVERSATION, L_ASSIGNMENT, L_QUIZ,
	L_SETTINGS, L_SETUP, L_STATIC, L_ALL_ANNOUNCEMENTS, L_MODULE_ITEM,
} Loader;

enum { FB_NONE, FB_REPLY, FB_DOWNLOAD };

typedef struct {
	VKind  kind;
	Loader loader;
	char   title[96];
	char   course[32], id[32];
	char  *arg;              // page url, file url...
	Row   *rows; int nrows, crows;
	int    sel;
	float  scroll, vel;      // list or document scroll (pixels)
	Doc   *doc;              // V_DOC
	char  *doc_html;
	int    focus;            // focused link in V_DOC
	int    job, job2, job3;  // pending requests
	NetResult r1, r2, r3;
	bool   have1, have2, have3;
	char   err[160];
	char   note[160];        // small status line (e.g. "Salvo em ...")
	int    footer;           // FB_*
	bool   loading;
} View;

View *view_new(VKind k, Loader l, const char *title);
void  view_free(View *v);
void  view_start(View *v);         // send its requests
void  view_poll(View *v);          // check requests, build rows/doc
Row  *view_add(View *v, Act a, const char *title, const char *sub, const char *right);
void  view_set_doc(View *v, const char *html);
View *view_open_row(View *from, Row *r);           // NULL if handled in place
View *view_open_link(View *from, const char *href); // Canvas links inside documents
View *view_settings(void);
void  view_refresh_settings(View *v);
void  view_reply(View *v);                          // conversation reply (keyboard)
void  view_download(View *v);                       // file download

// ---------- helpers ----------
time_t      parse_iso(const char *s);                // UTC; 0 if missing
void        fmt_when(time_t utc, char *out, size_t n);   // "seg 03/11 23:59"
void        fmt_due(time_t utc, char *out, size_t n, u32 *color);  // "em 2 dias", "atrasada"
const char *api_base(void);                          // "https://domain"
char       *xstrdup(const char *s);
float       doc_width(void);
float       doc_scale(void);
void        toast(const char *m);
void        ui_show_update(void);                    // opens the "update now?" window
bool        keyboard(const char *hint, char *out, size_t n, bool multiline);

extern char g_user_name[96];
extern char g_course_ids[64][32];
extern char g_course_names[64][96];
extern int  g_ncourses;
const char *course_name(const char *id);
