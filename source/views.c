// Screens of the app: each View asks the Canvas API for its data and turns the JSON
// into rows (lists) or an HTML document (pages, assignments, announcements...).
#include "app.h"
#include "updater.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char g_user_name[96];
char g_course_ids[64][32];
char g_course_names[64][96];
int  g_ncourses;

static const char *section_names[] = {
	"Página inicial", "Avisos", "Tarefas", "Módulos", "Páginas", "Discussões", "Arquivos", "Notas",
};
#define NSECTIONS 8

// ---------------------------------------------------------------- small helpers

char *xstrdup(const char *s) { return s ? strdup(s) : NULL; }

const char *api_base(void)
{
	static char b[160];
	snprintf(b, sizeof b, "https://%s", g_cfg.domain);
	return b;
}

static int get(const char *fmt, int pages, ...)
{
	char path[768], url[1024];
	va_list ap;
	va_start(ap, pages);
	vsnprintf(path, sizeof path, fmt, ap);
	va_end(ap);
	if (!strncmp(path, "http", 4)) snprintf(url, sizeof url, "%s", path);
	else snprintf(url, sizeof url, "%s%s", api_base(), path);
	return net_get(url, pages);
}

const char *course_name(const char *id)
{
	for (int i = 0; i < g_ncourses; i++) if (!strcmp(g_course_ids[i], id)) return g_course_names[i];
	return "";
}

static long days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	long yoe = y - era * 400;
	long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

time_t parse_iso(const char *s)
{
	if (!s || !*s) return 0;
	int Y, M, D, h = 0, mi = 0, se = 0;
	if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &se) < 3) return 0;
	time_t t = (time_t)days_from_civil(Y, M, D) * 86400 + h * 3600 + mi * 60 + se;
	// offset suffix: Z, +hh:mm or -hh:mm
	const char *p = strchr(s, 'T');
	if (p) {
		const char *z = p + 1;
		while (*z && *z != 'Z' && *z != '+' && *z != '-') z++;
		if (*z == '+' || *z == '-') {
			int oh = 0, om = 0;
			sscanf(z + 1, "%d:%d", &oh, &om);
			int off = (oh * 60 + om) * 60;
			t += *z == '+' ? -off : off;
		}
	}
	return t;
}

static time_t now_utc(void) { return time(NULL) - g_cfg.tz * 60; }

void fmt_when(time_t utc, char *out, size_t n)
{
	static const char *wd[7] = { "dom", "seg", "ter", "qua", "qui", "sex", "sáb" };
	if (!utc) { snprintf(out, n, "sem data"); return; }
	time_t t = utc + g_cfg.tz * 60;
	struct tm tm;
	gmtime_r(&t, &tm);
	snprintf(out, n, "%s %02d/%02d %02d:%02d", wd[tm.tm_wday], tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
}

void fmt_due(time_t utc, char *out, size_t n, u32 *color)
{
	*color = C_SUB;
	if (!utc) { snprintf(out, n, "sem prazo"); return; }
	time_t now = now_utc();
	long diff = (long)(utc - now);
	time_t lt = utc + g_cfg.tz * 60, ln = now + g_cfg.tz * 60;
	long dday = (long)(lt / 86400) - (long)(ln / 86400);
	struct tm tm;
	gmtime_r(&lt, &tm);
	if (diff < 0) { snprintf(out, n, "atrasada"); *color = C_RED; return; }
	if (dday == 0) { snprintf(out, n, "hoje %02d:%02d", tm.tm_hour, tm.tm_min); *color = C_RED; return; }
	if (dday == 1) { snprintf(out, n, "amanhã %02d:%02d", tm.tm_hour, tm.tm_min); *color = C_ORANGE; return; }
	if (dday < 7) {
		static const char *wd[7] = { "dom", "seg", "ter", "qua", "qui", "sex", "sáb" };
		snprintf(out, n, "%s %02d:%02d", wd[tm.tm_wday], tm.tm_hour, tm.tm_min);
		*color = dday < 3 ? C_ORANGE : C_SUB;
		return;
	}
	snprintf(out, n, "%02d/%02d", tm.tm_mday, tm.tm_mon + 1);
}

static void fmt_score(double v, char *out, size_t n)
{
	if (v == (long)v) snprintf(out, n, "%ld", (long)v);
	else { snprintf(out, n, "%.1f", v); for (char *p = out; *p; p++) if (*p == '.') *p = ','; }
}

// Appends text to a growing buffer.
typedef struct { char *s; size_t n, cap; } Buf;

static void bput(Buf *b, const char *s)
{
	if (!s) return;
	size_t l = strlen(s);
	if (b->n + l + 1 > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 1024;
		while (nc < b->n + l + 1) nc *= 2;
		char *ns = realloc(b->s, nc);
		if (!ns) return;
		b->s = ns;
		b->cap = nc;
	}
	memcpy(b->s + b->n, s, l + 1);
	b->n += l;
}

static void bprintf(Buf *b, const char *fmt, ...)
{
	char tmp[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	bput(b, tmp);
}

// HTML-escapes plain text (titles, names) before putting it inside a document.
static void besc(Buf *b, const char *s)
{
	if (!s) return;
	char tmp[2];
	tmp[1] = 0;
	for (; *s; s++) {
		if (*s == '<') bput(b, "&lt;");
		else if (*s == '>') bput(b, "&gt;");
		else if (*s == '&') bput(b, "&amp;");
		else if (*s == '\n') bput(b, "<br>");
		else { tmp[0] = *s; bput(b, tmp); }
	}
}

// ---------------------------------------------------------------- views

float doc_width(void) { return 308; }
float doc_scale(void) { return g_cfg.text_size == 0 ? 0.42f : g_cfg.text_size == 2 ? 0.56f : 0.48f; }

View *view_new(VKind k, Loader l, const char *title)
{
	View *v = calloc(1, sizeof(View));
	if (!v) return NULL;
	v->kind = k;
	v->loader = l;
	v->focus = -1;
	snprintf(v->title, sizeof v->title, "%s", title ? title : "");
	return v;
}

static void free_rows(View *v)
{
	for (int i = 0; i < v->nrows; i++) {
		Row *r = &v->rows[i];
		free(r->title); free(r->sub); free(r->right); free(r->html); free(r->extra);
	}
	free(v->rows);
	v->rows = NULL;
	v->nrows = v->crows = 0;
}

static void drop_jobs(View *v)
{
	if (v->job > 0) net_forget(v->job);
	if (v->job2 > 0) net_forget(v->job2);
	if (v->job3 > 0) net_forget(v->job3);
	v->job = v->job2 = v->job3 = 0;
	free(v->r1.body); free(v->r2.body); free(v->r3.body);
	memset(&v->r1, 0, sizeof v->r1); memset(&v->r2, 0, sizeof v->r2); memset(&v->r3, 0, sizeof v->r3);
	v->have1 = v->have2 = v->have3 = false;
}

void view_free(View *v)
{
	if (!v) return;
	drop_jobs(v);
	free_rows(v);
	html_free(v->doc);
	free(v->doc_html);
	free(v->arg);
	free(v);
}

Row *view_add(View *v, Act a, const char *title, const char *sub, const char *right)
{
	if (v->nrows == v->crows) {
		int nc = v->crows ? v->crows * 2 : 32;
		Row *nr = realloc(v->rows, nc * sizeof(Row));
		if (!nr) return NULL;
		v->rows = nr;
		v->crows = nc;
	}
	Row *r = &v->rows[v->nrows++];
	memset(r, 0, sizeof *r);
	r->act = a;
	r->title = xstrdup(title && *title ? title : "(sem título)");
	r->sub = xstrdup(sub && *sub ? sub : NULL);
	r->right = xstrdup(right && *right ? right : NULL);
	r->right_color = C_SUB;
	return r;
}

void view_set_doc(View *v, const char *html)
{
	html_free(v->doc);
	free(v->doc_html);
	v->doc_html = xstrdup(html ? html : "");
	v->doc = html_layout(v->doc_html, doc_width(), doc_scale());
	v->focus = -1;
}

// ---------------------------------------------------------------- requests

void view_start(View *v)
{
	drop_jobs(v);
	v->err[0] = 0;
	v->loading = true;
	const char *c = v->course, *id = v->id;
	switch (v->loader) {
	case L_HOME:
		v->job = get("/api/v1/courses?enrollment_state=active&include[]=total_scores&include[]=term&per_page=50", 3);
		v->job2 = get("/api/v1/users/self/todo?per_page=50", 2);
		v->job3 = get("/api/v1/users/self/profile", 1);
		break;
	case L_ANNOUNCEMENTS:
		v->job = get("/api/v1/announcements?context_codes[]=course_%s&start_date=2010-01-01&per_page=40", 2, c);
		break;
	case L_ALL_ANNOUNCEMENTS: {
		if (!g_ncourses) { snprintf(v->err, sizeof v->err, "Abra a tela inicial primeiro para carregar os cursos."); v->loading = false; return; }
		char q[1024] = "/api/v1/announcements?per_page=40&start_date=2010-01-01";
		for (int i = 0; i < g_ncourses && strlen(q) < sizeof q - 64; i++) {
			strcat(q, "&context_codes[]=course_");
			strcat(q, g_course_ids[i]);
		}
		v->job = get("%s", 2, q);
		break;
	}
	case L_ASSIGNMENTS:
		v->job = get("/api/v1/courses/%s/assignments?include[]=submission&order_by=due_at&per_page=100", 3, c);
		break;
	case L_GRADES:
		v->job = get("/api/v1/courses/%s/assignments?include[]=submission&order_by=due_at&per_page=100", 3, c);
		v->job2 = get("/api/v1/courses/%s/enrollments?user_id=self", 1, c);
		break;
	case L_MODULES:
		v->job = get("/api/v1/courses/%s/modules?include[]=items&per_page=50", 4, c);
		break;
	case L_PAGES:
		v->job = get("/api/v1/courses/%s/pages?per_page=100&sort=title", 3, c);
		break;
	case L_PAGE:
		if (v->arg && !strcmp(v->arg, "front_page")) v->job = get("/api/v1/courses/%s/front_page", 1, c);
		else v->job = get("/api/v1/courses/%s/pages/%s", 1, c, v->arg ? v->arg : "");
		break;
	case L_DISCUSSIONS:
		v->job = get("/api/v1/courses/%s/discussion_topics?per_page=50", 2, c);
		break;
	case L_DISCUSSION:
		v->job = get("/api/v1/courses/%s/discussion_topics/%s", 1, c, id);
		v->job2 = get("/api/v1/courses/%s/discussion_topics/%s/entries?per_page=50", 2, c, id);
		break;
	case L_FILES:
		v->job = get("/api/v1/courses/%s/files?per_page=100&sort=updated_at&order=desc", 3, c);
		break;
	case L_FILE:
		v->job = get("/api/v1/files/%s", 1, id);
		break;
	case L_INBOX:
		v->job = get("/api/v1/conversations?per_page=40", 2);
		break;
	case L_CONVERSATION:
		v->job = get("/api/v1/conversations/%s", 1, id);
		break;
	case L_ASSIGNMENT:
		if (v->doc_html) { v->loading = false; return; }
		v->job = get("/api/v1/courses/%s/assignments/%s?include[]=submission", 1, c, id);
		break;
	case L_QUIZ:
		v->job = get("/api/v1/courses/%s/quizzes/%s", 1, c, id);
		break;
	default:
		v->loading = false;
		break;
	}
}

// ---------------------------------------------------------------- building screens

static void assignment_html(Buf *b, const JNode *a, const char *course_title)
{
	char when[48], due[48];
	u32 col;
	time_t t = parse_iso(json_str(a, "due_at"));
	fmt_when(t, when, sizeof when);
	fmt_due(t, due, sizeof due, &col);
	bput(b, "<h2>"); besc(b, json_str(a, "name")); bput(b, "</h2>");
	if (course_title && *course_title) { bput(b, "<p><small>"); besc(b, course_title); bput(b, "</small></p>"); }
	bprintf(b, "<p><b>Entrega:</b> %s (%s)", t ? when : "sem prazo", due);
	double pts = json_num(a, "points_possible", -1);
	if (pts >= 0) { char s[24]; fmt_score(pts, s, sizeof s); bprintf(b, "<br><b>Pontos:</b> %s", s); }
	const JNode *sub = json_get(a, "submission");
	if (sub && sub->type == J_OBJ) {
		const char *st = json_str(sub, "workflow_state");
		const char *grade = json_str(sub, "grade");
		bool missing = json_bool(sub, "missing"), late = json_bool(sub, "late");
		const char *txt = missing ? "faltando" : st && !strcmp(st, "graded") ? "corrigida" :
		                  st && !strcmp(st, "submitted") ? "entregue" : st && !strcmp(st, "pending_review") ? "aguardando correção" : "não entregue";
		bprintf(b, "<br><b>Situação:</b> %s%s", txt, late ? " (com atraso)" : "");
		if (grade && *grade) { bput(b, "<br><b>Nota:</b> "); besc(b, grade); }
	}
	bput(b, "</p>");
	JNode *types = json_get(a, "submission_types");
	if (types && types->count) {
		bput(b, "<p><small>Tipo de entrega:");
		json_each(types, it) if (it->str) { bput(b, " "); besc(b, it->str); }
		bput(b, "</small></p>");
	}
	bput(b, "<hr>");
	const char *desc = json_str(a, "description");
	bput(b, desc && *desc ? desc : "<p><small>Sem descrição.</small></p>");
}

static void add_assignment_row(View *v, const JNode *a, const char *course_id, bool grade_mode)
{
	char due[48];
	u32 col;
	time_t t = parse_iso(json_str(a, "due_at"));
	fmt_due(t, due, sizeof due, &col);
	const JNode *sub = json_get(a, "submission");
	const char *st = sub ? json_str(sub, "workflow_state") : NULL;
	bool done = st && (!strcmp(st, "submitted") || !strcmp(st, "graded") || !strcmp(st, "pending_review"));
	char right[48], subl[96] = "";
	if (grade_mode) {
		const char *score = sub ? json_str(sub, "score") : NULL;
		double pts = json_num(a, "points_possible", -1);
		char p[24] = "-";
		if (pts >= 0) fmt_score(pts, p, sizeof p);
		if (score) { char s[24]; fmt_score(atof(score), s, sizeof s); snprintf(right, sizeof right, "%s / %s", s, p); col = C_GREEN; }
		else { snprintf(right, sizeof right, "- / %s", p); col = C_SUB; }
		snprintf(subl, sizeof subl, "%s", done ? (st && !strcmp(st, "graded") ? "corrigida" : "entregue") : t && t < now_utc() ? "não entregue" : "pendente");
	} else {
		snprintf(right, sizeof right, "%s", done ? "entregue" : due);
		if (done) col = C_GREEN;
		char when[48];
		fmt_when(t, when, sizeof when);
		snprintf(subl, sizeof subl, "%s", t ? when : "sem prazo");
	}
	Row *r = view_add(v, A_ASSIGNMENT, json_str(a, "name"), subl, right);
	if (!r) return;
	r->right_color = col;
	r->dot = done ? C_GREEN : t && t < now_utc() ? C_RED : C_ORANGE;
	snprintf(r->id, sizeof r->id, "%s", json_str(a, "id") ? json_str(a, "id") : "");
	snprintf(r->course, sizeof r->course, "%s", course_id);
	Buf b = {0};
	assignment_html(&b, a, NULL);
	r->html = b.s;
}

static void build_home(View *v)
{
	free_rows(v);
	// profile
	if (v->have3 && v->r3.body) {
		JDoc *d = json_parse(v->r3.body, v->r3.len);
		const char *n = json_str(json_root(d), "short_name");
		if (!n) n = json_str(json_root(d), "name");
		if (n) snprintf(g_user_name, sizeof g_user_name, "%s", n);
		json_free(d);
	}
	// courses
	JDoc *cd = v->r1.body ? json_parse(v->r1.body, v->r1.len) : NULL;
	g_ncourses = 0;
	json_each(json_root(cd), c) {
		const char *id = json_str(c, "id"), *name = json_str(c, "name");
		if (!id || !name || g_ncourses >= 64) continue;
		snprintf(g_course_ids[g_ncourses], 32, "%s", id);
		snprintf(g_course_names[g_ncourses], 96, "%s", name);
		g_ncourses++;
	}

	// to-do
	view_add(v, A_HEADER, "Para fazer", NULL, NULL);
	int ntodo = 0;
	JDoc *td = v->r2.body ? json_parse(v->r2.body, v->r2.len) : NULL;
	json_each(json_root(td), it) {
		const JNode *a = json_get(it, "assignment");
		if (!a) continue;
		const char *cid = json_str(it, "course_id");
		if (!cid) cid = json_str(a, "course_id");
		char due[48];
		u32 col;
		fmt_due(parse_iso(json_str(a, "due_at")), due, sizeof due, &col);
		const char *ctx = json_str(it, "context_name");
		Row *r = view_add(v, A_ASSIGNMENT, json_str(a, "name"), ctx ? ctx : course_name(cid ? cid : ""), due);
		if (!r) break;
		r->right_color = col;
		r->dot = col == C_SUB ? C_ORANGE : col;
		snprintf(r->id, sizeof r->id, "%s", json_str(a, "id") ? json_str(a, "id") : "");
		snprintf(r->course, sizeof r->course, "%s", cid ? cid : "");
		Buf b = {0};
		assignment_html(&b, a, ctx);
		r->html = b.s;
		ntodo++;
	}
	if (!ntodo) view_add(v, A_NONE, "Nada pendente por enquanto", v->r2.err[0] ? v->r2.err : NULL, NULL);
	json_free(td);

	view_add(v, A_HEADER, "Cursos", NULL, NULL);
	json_each(json_root(cd), c) {
		const char *id = json_str(c, "id"), *name = json_str(c, "name");
		if (!id || !name) continue;
		char right[32] = "", sub[128] = "";
		const JNode *enr = json_get(c, "enrollments");
		if (enr && enr->child) {
			JNode *sc = json_get(enr->child, "computed_current_score");
			if (sc && sc->type == J_NUM) { fmt_score(sc->num, right, sizeof right); strcat(right, "%"); }
		}
		const char *code = json_str(c, "course_code");
		const char *term = json_path(c, "term.name");
		snprintf(sub, sizeof sub, "%s%s%s", code ? code : "", code && term ? " · " : "", term ? term : "");
		Row *r = view_add(v, A_COURSE, name, sub, right);
		if (!r) break;
		snprintf(r->id, sizeof r->id, "%s", id);
		r->dot = C_ACCENT;
		r->right_color = C_GREEN;
	}
	if (!cd) view_add(v, A_NONE, "Não consegui carregar os cursos", v->r1.err[0] ? v->r1.err : NULL, NULL);
	json_free(cd);

	view_add(v, A_HEADER, "Mais", NULL, NULL);
	view_add(v, A_INBOX, "Caixa de entrada", "Mensagens dos professores e colegas", NULL);
	view_add(v, A_ALL_ANNOUNCEMENTS, "Avisos de todos os cursos", NULL, NULL);
	view_add(v, A_SETTINGS, "Ajustes", "Token, fuso, tamanho do texto, atualização", NULL);
	if (!g_cfg.token[0]) snprintf(v->err, sizeof v->err, "Ainda sem token. Veja em Ajustes > Como gerar o token.");
	else if (v->r1.err[0] && v->r1.status == 401) snprintf(v->err, sizeof v->err, "%s", v->r1.err);
}

static void build_course(View *v)
{
	free_rows(v);
	for (int i = 0; i < NSECTIONS; i++) {
		Row *r = view_add(v, A_SECTION, section_names[i], NULL, NULL);
		if (!r) break;
		r->section = i;
		snprintf(r->course, sizeof r->course, "%s", v->course);
		r->dot = C_BAR2;
	}
	v->loading = false;
}

static void build_announcements(View *v, bool all)
{
	free_rows(v);
	JDoc *d = json_parse(v->r1.body, v->r1.len);
	json_each(json_root(d), a) {
		char when[48], sub[160];
		fmt_when(parse_iso(json_str(a, "posted_at")), when, sizeof when);
		const char *author = json_path(a, "author.display_name");
		const char *ctx = json_str(a, "context_code");
		const char *cname = ctx && !strncmp(ctx, "course_", 7) ? course_name(ctx + 7) : "";
		if (all && *cname) snprintf(sub, sizeof sub, "%s · %s", cname, when);
		else snprintf(sub, sizeof sub, "%s%s%s", author ? author : "", author ? " · " : "", when);
		Row *r = view_add(v, A_ANNOUNCEMENT, json_str(a, "title"), sub, NULL);
		if (!r) break;
		r->dot = json_bool(a, "read_state") || (json_str(a, "read_state") && !strcmp(json_str(a, "read_state"), "read")) ? C_LINE : C_ACCENT;
		if (ctx && !strncmp(ctx, "course_", 7)) snprintf(r->course, sizeof r->course, "%s", ctx + 7);
		snprintf(r->id, sizeof r->id, "%s", json_str(a, "id") ? json_str(a, "id") : "");
		Buf b = {0};
		bput(&b, "<h2>"); besc(&b, json_str(a, "title")); bput(&b, "</h2><p><small>");
		besc(&b, sub);
		bput(&b, "</small></p><hr>");
		bput(&b, json_str(a, "message") ? json_str(a, "message") : "");
		r->html = b.s;
	}
	if (!v->nrows) view_add(v, A_NONE, "Nenhum aviso", NULL, NULL);
	json_free(d);
}

static void build_assignments(View *v, bool grades)
{
	free_rows(v);
	if (grades && v->have2 && v->r2.body) {
		JDoc *e = json_parse(v->r2.body, v->r2.len);
		JNode *en = json_root(e) ? json_root(e)->child : NULL;
		if (en) {
			JNode *g = json_get(en, "grades");
			double cur = json_num(g, "current_score", -1);
			const char *letter = json_str(g, "current_grade");
			char t[64] = "Sem nota ainda", s[24];
			if (cur >= 0) { fmt_score(cur, s, sizeof s); snprintf(t, sizeof t, "Nota atual: %s%%%s%s", s, letter ? " · " : "", letter ? letter : ""); }
			Row *r = view_add(v, A_NONE, t, "Média calculada pelo Canvas", NULL);
			if (r) r->dot = C_GREEN;
		}
		json_free(e);
	}
	JDoc *d = json_parse(v->r1.body, v->r1.len);
	// upcoming first, then past (Canvas sorts by due date ascending)
	time_t now = now_utc();
	for (int pass = 0; pass < 2; pass++) {
		bool any = false;
		json_each(json_root(d), a) {
			time_t t = parse_iso(json_str(a, "due_at"));
			bool future = !t || t >= now;
			if ((pass == 0) != future) continue;
			if (!any) { view_add(v, A_HEADER, pass == 0 ? "Próximas" : "Anteriores", NULL, NULL); any = true; }
			add_assignment_row(v, a, v->course, grades);
		}
	}
	if (!v->nrows) view_add(v, A_NONE, "Nenhuma tarefa", NULL, NULL);
	json_free(d);
}

static void build_modules(View *v)
{
	free_rows(v);
	JDoc *d = json_parse(v->r1.body, v->r1.len);
	json_each(json_root(d), m) {
		view_add(v, A_HEADER, json_str(m, "name"), NULL, NULL);
		JNode *items = json_get(m, "items");
		if (!items) { view_add(v, A_NONE, "(abra pelo site para ver os itens)", NULL, NULL); continue; }
		json_each(items, it) {
			const char *type = json_str(it, "type");
			if (!type) continue;
			if (!strcmp(type, "SubHeader")) { Row *r = view_add(v, A_NONE, json_str(it, "title"), NULL, NULL); if (r) r->dot = C_LINE; continue; }
			const char *label = !strcmp(type, "Page") ? "Página" : !strcmp(type, "Assignment") ? "Tarefa" :
			                    !strcmp(type, "Discussion") ? "Discussão" : !strcmp(type, "File") ? "Arquivo" :
			                    !strcmp(type, "Quiz") ? "Questionário" : !strcmp(type, "ExternalUrl") ? "Link" :
			                    !strcmp(type, "ExternalTool") ? "Ferramenta externa" : type;
			Row *r = view_add(v, A_MODULE_ITEM, json_str(it, "title"), label, NULL);
			if (!r) break;
			snprintf(r->course, sizeof r->course, "%s", v->course);
			const char *cid = json_str(it, "content_id");
			snprintf(r->id, sizeof r->id, "%s", cid ? cid : "");
			// extra = "Type\tpage_url_or_external_url"
			char ex[600];
			const char *u = json_str(it, "page_url");
			if (!u) u = json_str(it, "external_url");
			if (!u) u = json_str(it, "html_url");
			snprintf(ex, sizeof ex, "%s\t%s", type, u ? u : "");
			r->extra = xstrdup(ex);
			r->dot = !strcmp(type, "Assignment") || !strcmp(type, "Quiz") ? C_ORANGE : C_BAR2;
		}
	}
	if (!v->nrows) view_add(v, A_NONE, "Nenhum módulo", NULL, NULL);
	json_free(d);
}

static void build_pages(View *v)
{
	free_rows(v);
	JDoc *d = json_parse(v->r1.body, v->r1.len);
	json_each(json_root(d), p) {
		char when[48];
		fmt_when(parse_iso(json_str(p, "updated_at")), when, sizeof when);
		char sub[80];
		snprintf(sub, sizeof sub, "atualizada %s", when);
		Row *r = view_add(v, A_PAGE, json_str(p, "title"), sub, json_bool(p, "front_page") ? "inicial" : NULL);
		if (!r) break;
		snprintf(r->course, sizeof r->course, "%s", v->course);
		r->extra = xstrdup(json_str(p, "url"));
		r->dot = C_BAR2;
	}
	if (!v->nrows) view_add(v, A_NONE, "Nenhuma página", NULL, NULL);
	json_free(d);
}

static void build_discussions(View *v)
{
	free_rows(v);
	JDoc *d = json_parse(v->r1.body, v->r1.len);
	json_each(json_root(d), t) {
		char when[48], sub[120], right[24] = "";
		fmt_when(parse_iso(json_str(t, "last_reply_at") ? json_str(t, "last_reply_at") : json_str(t, "posted_at")), when, sizeof when);
		int n = (int)json_num(t, "discussion_subentry_count", 0);
		int unread = (int)json_num(t, "unread_count", 0);
		snprintf(sub, sizeof sub, "%d resposta%s · %s", n, n == 1 ? "" : "s", when);
		if (unread) snprintf(right, sizeof right, "%d nova%s", unread, unread == 1 ? "" : "s");
		Row *r = view_add(v, A_DISCUSSION, json_str(t, "title"), sub, right);
		if (!r) break;
		r->right_color = C_ACCENT;
		r->dot = unread ? C_ACCENT : C_LINE;
		snprintf(r->course, sizeof r->course, "%s", v->course);
		snprintf(r->id, sizeof r->id, "%s", json_str(t, "id") ? json_str(t, "id") : "");
	}
	if (!v->nrows) view_add(v, A_NONE, "Nenhuma discussão", NULL, NULL);
	json_free(d);
}

static void size_text(double b, char *out, size_t n)
{
	if (b < 1024) snprintf(out, n, "%.0f B", b);
	else if (b < 1024 * 1024) snprintf(out, n, "%.0f KB", b / 1024);
	else snprintf(out, n, "%.1f MB", b / 1048576);
}

static void build_files(View *v)
{
	free_rows(v);
	JDoc *d = json_parse(v->r1.body, v->r1.len);
	json_each(json_root(d), f) {
		char sz[24], when[48], sub[100];
		size_text(json_num(f, "size", 0), sz, sizeof sz);
		fmt_when(parse_iso(json_str(f, "updated_at")), when, sizeof when);
		snprintf(sub, sizeof sub, "%s · %s", sz, when);
		Row *r = view_add(v, A_FILE, json_str(f, "display_name"), sub, NULL);
		if (!r) break;
		snprintf(r->id, sizeof r->id, "%s", json_str(f, "id") ? json_str(f, "id") : "");
		snprintf(r->course, sizeof r->course, "%s", v->course);
		r->dot = C_BAR2;
	}
	if (!v->nrows) view_add(v, A_NONE, "Nenhum arquivo", NULL, NULL);
	json_free(d);
}

static void build_inbox(View *v)
{
	free_rows(v);
	JDoc *d = json_parse(v->r1.body, v->r1.len);
	json_each(json_root(d), c) {
		char when[48], sub[200];
		fmt_when(parse_iso(json_str(c, "last_message_at")), when, sizeof when);
		const char *last = json_str(c, "last_message");
		snprintf(sub, sizeof sub, "%s · %s", when, last ? last : "");
		const char *subj = json_str(c, "subject");
		Row *r = view_add(v, A_CONVERSATION, subj && *subj ? subj : "(sem assunto)", sub, NULL);
		if (!r) break;
		const char *st = json_str(c, "workflow_state");
		r->dot = st && !strcmp(st, "unread") ? C_ACCENT : C_LINE;
		snprintf(r->id, sizeof r->id, "%s", json_str(c, "id") ? json_str(c, "id") : "");
		// participants in the preview
		Buf b = {0};
		bput(&b, "<h3>"); besc(&b, r->title); bput(&b, "</h3><p><small>");
		JNode *ps = json_get(c, "participants");
		int k = 0;
		json_each(ps, p) { if (k++) bput(&b, ", "); besc(&b, json_str(p, "name")); }
		bput(&b, "</small></p><p>");
		besc(&b, last);
		bput(&b, "</p>");
		r->html = b.s;
	}
	if (!v->nrows) view_add(v, A_NONE, "Nenhuma mensagem", NULL, NULL);
	json_free(d);
}

static void build_conversation(View *v)
{
	JDoc *d = json_parse(v->r1.body, v->r1.len);
	JNode *c = json_root(d);
	// participant id -> name
	Buf b = {0};
	bput(&b, "<h2>"); besc(&b, json_str(c, "subject")); bput(&b, "</h2>");
	JNode *ps = json_get(c, "participants");
	JNode *msgs = json_get(c, "messages");
	json_each(msgs, m) {
		const char *aid = json_str(m, "author_id");
		const char *who = "?";
		json_each(ps, p) if (aid && json_str(p, "id") && !strcmp(json_str(p, "id"), aid)) who = json_str(p, "name");
		char when[48];
		fmt_when(parse_iso(json_str(m, "created_at")), when, sizeof when);
		bput(&b, "<p><b>"); besc(&b, who); bput(&b, "</b> <small>"); besc(&b, when); bput(&b, "</small><br>");
		besc(&b, json_str(m, "body"));
		bput(&b, "</p><hr>");
	}
	view_set_doc(v, b.s ? b.s : "");
	free(b.s);
	json_free(d);
	v->footer = FB_REPLY;
}

static void build_discussion(View *v)
{
	JDoc *t = json_parse(v->r1.body, v->r1.len);
	JDoc *e = v->r2.body ? json_parse(v->r2.body, v->r2.len) : NULL;
	JNode *tp = json_root(t);
	Buf b = {0};
	char when[48];
	fmt_when(parse_iso(json_str(tp, "posted_at")), when, sizeof when);
	bput(&b, "<h2>"); besc(&b, json_str(tp, "title")); bput(&b, "</h2><p><small>");
	besc(&b, json_path(tp, "author.display_name")); bprintf(&b, " · %s</small></p>", when);
	bput(&b, json_str(tp, "message") ? json_str(tp, "message") : "");
	JNode *entries = json_root(e);
	if (entries && entries->count) {
		bprintf(&b, "<hr><h3>Respostas (%d)</h3>", entries->count);
		json_each(entries, en) {
			fmt_when(parse_iso(json_str(en, "created_at")), when, sizeof when);
			const char *who = json_str(en, "user_name");
			bput(&b, "<p><b>"); besc(&b, who ? who : "Participante"); bprintf(&b, "</b> <small>%s</small></p>", when);
			bput(&b, json_str(en, "message") ? json_str(en, "message") : "");
			JNode *rr = json_get(en, "recent_replies");
			json_each(rr, rp) {
				const char *rw = json_str(rp, "user_name");
				bput(&b, "<blockquote><p><b>"); besc(&b, rw ? rw : "Participante"); bput(&b, "</b></p>");
				bput(&b, json_str(rp, "message") ? json_str(rp, "message") : "");
				bput(&b, "</blockquote>");
			}
			bput(&b, "<hr>");
		}
	}
	view_set_doc(v, b.s ? b.s : "");
	free(b.s);
	json_free(t);
	json_free(e);
}

static void build_file(View *v)
{
	JDoc *d = json_parse(v->r1.body, v->r1.len);
	JNode *f = json_root(d);
	Buf b = {0};
	char sz[24], when[48];
	size_text(json_num(f, "size", 0), sz, sizeof sz);
	fmt_when(parse_iso(json_str(f, "updated_at")), when, sizeof when);
	bput(&b, "<h2>"); besc(&b, json_str(f, "display_name")); bput(&b, "</h2>");
	bprintf(&b, "<p><b>Tamanho:</b> %s<br><b>Atualizado:</b> %s<br><b>Tipo:</b> ", sz, when);
	besc(&b, json_str(f, "content-type"));
	bput(&b, "</p><p>O 3DS não abre PDF nem documentos do Office. Aperte <b>Y</b> (ou o botão <b>Baixar</b>) para salvar o "
	         "arquivo no cartão SD, na pasta <b>3ds/Canvas3DS/arquivos</b>, e abrir depois no computador.</p>");
	view_set_doc(v, b.s);
	free(b.s);
	free(v->arg);
	v->arg = xstrdup(json_str(f, "url"));   // signed download URL
	snprintf(v->title, sizeof v->title, "%s", json_str(f, "display_name") ? json_str(f, "display_name") : "Arquivo");
	v->footer = FB_DOWNLOAD;
	json_free(d);
}

static void build_page(View *v)
{
	JDoc *d = json_parse(v->r1.body, v->r1.len);
	JNode *p = json_root(d);
	Buf b = {0};
	bput(&b, "<h2>"); besc(&b, json_str(p, "title")); bput(&b, "</h2>");
	const char *body = json_str(p, "body");
	bput(&b, body && *body ? body : "<p><small>Página vazia.</small></p>");
	view_set_doc(v, b.s);
	free(b.s);
	if (json_str(p, "title")) snprintf(v->title, sizeof v->title, "%s", json_str(p, "title"));
	json_free(d);
}

static void build_quiz(View *v)
{
	JDoc *d = json_parse(v->r1.body, v->r1.len);
	JNode *q = json_root(d);
	Buf b = {0};
	char when[48];
	time_t t = parse_iso(json_str(q, "due_at"));
	fmt_when(t, when, sizeof when);
	bput(&b, "<h2>"); besc(&b, json_str(q, "title")); bput(&b, "</h2>");
	bprintf(&b, "<p><b>Entrega:</b> %s<br><b>Perguntas:</b> %d", t ? when : "sem prazo", (int)json_num(q, "question_count", 0));
	double lim = json_num(q, "time_limit", 0);
	if (lim > 0) bprintf(&b, "<br><b>Tempo:</b> %d min", (int)lim);
	bput(&b, "</p><p><small>Questionários precisam ser feitos pelo site ou pelo app do Canvas no celular.</small></p><hr>");
	bput(&b, json_str(q, "description") ? json_str(q, "description") : "");
	view_set_doc(v, b.s);
	free(b.s);
	json_free(d);
}

static void build_assignment(View *v)
{
	JDoc *d = json_parse(v->r1.body, v->r1.len);
	Buf b = {0};
	assignment_html(&b, json_root(d), course_name(v->course));
	view_set_doc(v, b.s);
	free(b.s);
	json_free(d);
}

static void build_settings(View *v);

void view_poll(View *v)
{
	if (!v) return;
	// action jobs (reply / download)
	if (v->job3 && v->loader != L_HOME) {
		NetResult r;
		if (net_poll(v->job3, &r)) {
			v->job3 = 0;
			if (r.err[0]) snprintf(v->note, sizeof v->note, "%s", r.err);
			else if (v->loader == L_CONVERSATION) { snprintf(v->note, sizeof v->note, "Resposta enviada."); free(r.body); view_start(v); return; }
			else if (v->loader == L_FILE) snprintf(v->note, sizeof v->note, "Arquivo salvo no cartão SD (3ds/Canvas3DS/arquivos).");
			free(r.body);
		} else if (v->loader == L_FILE) {
			snprintf(v->note, sizeof v->note, "Baixando... %d%%", (int)(net_progress(v->job3) * 100));
		}
	}
	if (!v->loading) return;
	if (v->job && !v->have1 && net_poll(v->job, &v->r1)) { v->have1 = true; v->job = -1; }
	if (v->job2 && !v->have2 && net_poll(v->job2, &v->r2)) { v->have2 = true; v->job2 = -1; }
	if (v->loader == L_HOME && v->job3 && !v->have3 && net_poll(v->job3, &v->r3)) { v->have3 = true; v->job3 = -1; }
	bool need2 = v->job2 != 0, need3 = v->loader == L_HOME && v->job3 != 0;
	if ((v->job && !v->have1) || (need2 && !v->have2) || (need3 && !v->have3)) return;
	v->loading = false;

	if (v->loader != L_HOME && v->r1.err[0]) {
		snprintf(v->err, sizeof v->err, "%s", v->r1.err);
	} else {
		switch (v->loader) {
		case L_HOME: build_home(v); break;
		case L_ANNOUNCEMENTS: build_announcements(v, false); break;
		case L_ALL_ANNOUNCEMENTS: build_announcements(v, true); break;
		case L_ASSIGNMENTS: build_assignments(v, false); break;
		case L_GRADES: build_assignments(v, true); break;
		case L_MODULES: build_modules(v); break;
		case L_PAGES: build_pages(v); break;
		case L_PAGE: build_page(v); break;
		case L_DISCUSSIONS: build_discussions(v); break;
		case L_DISCUSSION: build_discussion(v); break;
		case L_FILES: build_files(v); break;
		case L_FILE: build_file(v); break;
		case L_INBOX: build_inbox(v); break;
		case L_CONVERSATION: build_conversation(v); break;
		case L_ASSIGNMENT: build_assignment(v); break;
		case L_QUIZ: build_quiz(v); break;
		default: break;
		}
	}
	free(v->r1.body); free(v->r2.body); free(v->r3.body);
	memset(&v->r1, 0, sizeof v->r1); memset(&v->r2, 0, sizeof v->r2); memset(&v->r3, 0, sizeof v->r3);
	v->have1 = v->have2 = v->have3 = false;
	v->job = v->job2 = 0;
	if (v->loader == L_HOME) v->job3 = 0;
	if (v->sel >= v->nrows) v->sel = 0;
}

// ---------------------------------------------------------------- settings / setup

static void static_doc(View *v, const char *html)
{
	view_set_doc(v, html);
	v->loading = false;
}

static const char *setup_html(void)
{
	static char h[2600];
	snprintf(h, sizeof h,
		"<h2>Conectar ao Canvas</h2>"
		"<p>O app usa um <b>token de acesso</b>, uma senha especial que o próprio Canvas gera. "
		"Você só faz isso uma vez.</p>"
		"<ol><li>No computador ou celular, entre em <b>https://%s/profile/settings</b></li>"
		"<li>Role até <b>Integrações aprovadas</b> e toque em <b>+ Novo token de acesso</b>.</li>"
		"<li>Em <i>Finalidade</i> escreva <b>3DS</b>, deixe a data em branco e toque em <b>Gerar token</b>.</li>"
		"<li>Copie o token (aquele texto longo). Ele só aparece uma vez.</li></ol>"
		"<h3>Jeito fácil: pelo PC</h3>"
		"<p>Coloque o cartão SD do 3DS no PC e rode <b>pc\\configurar_token.bat</b> do pacote. "
		"Ele pede o token, confere se funciona e salva no cartão.</p>"
		"<h3>Ou à mão</h3>"
		"<p>Crie o arquivo <b>3ds/Canvas3DS/token.txt</b> no cartão SD com o token dentro, ou aperte <b>Y</b> aqui "
		"para digitar no teclado do 3DS.</p>"
		"<p><small>O token fica só no seu cartão SD. Se perder o 3DS, apague o token no site do Canvas.</small></p>",
		g_cfg.domain);
	return h;
}

static void build_settings(View *v)
{
	free_rows(v);
	char t[160];
	Row *r;
	view_add(v, A_HEADER, "Conta", NULL, NULL);
	snprintf(t, sizeof t, "%s", g_cfg.token[0] ? "Token salvo" : "Sem token");
	r = view_add(v, A_SETTINGS, "Token de acesso", t, "trocar"); if (r) r->section = 1;
	r = view_add(v, A_SETTINGS, "Ler o token do cartão SD de novo", "3ds/Canvas3DS/token.txt", NULL); if (r) r->section = 2;
	r = view_add(v, A_SETTINGS, "Endereço do Canvas", g_cfg.domain, "trocar"); if (r) r->section = 3;
	r = view_add(v, A_SETTINGS, "Como gerar o token", NULL, NULL); if (r) r->section = 8;
	view_add(v, A_HEADER, "Aparência e hora", NULL, NULL);
	int a = g_cfg.tz < 0 ? -g_cfg.tz : g_cfg.tz;
	snprintf(t, sizeof t, "UTC%c%02d:%02d (toque para mudar)", g_cfg.tz < 0 ? '-' : '+', a / 60, a % 60);
	r = view_add(v, A_SETTINGS, "Fuso horário", t, "-  +"); if (r) r->section = 4;
	static const char *sizes[] = { "pequeno", "normal", "grande" };
	r = view_add(v, A_SETTINGS, "Tamanho do texto", sizes[g_cfg.text_size], "mudar"); if (r) r->section = 6;
	view_add(v, A_HEADER, "Atualização", NULL, NULL);
	UpdState us = updater_state();
	const char *msg = us == UPD_IDLE ? "Toque para procurar versão nova" : updater_message();
	snprintf(t, sizeof t, "Versão %s (%s)", updater_version(), updater_is_cia() ? "CIA" : "3DSX");
	r = view_add(v, A_SETTINGS, t, msg, us == UPD_AVAILABLE ? "instalar" : NULL);
	if (r) { r->section = 7; r->right_color = C_ACCENT; }
	v->loading = false;
}

// ---------------------------------------------------------------- opening things

static View *open_doc_html(const char *title, const char *html, Loader l)
{
	View *n = view_new(V_DOC, l, title);
	if (n) static_doc(n, html);
	return n;
}

static View *section_view(const char *course, int s)
{
	static const Loader ls[NSECTIONS] = { L_PAGE, L_ANNOUNCEMENTS, L_ASSIGNMENTS, L_MODULES, L_PAGES, L_DISCUSSIONS, L_FILES, L_GRADES };
	View *n = view_new(s == 0 ? V_DOC : V_LIST, ls[s], section_names[s]);
	if (!n) return NULL;
	snprintf(n->course, sizeof n->course, "%s", course);
	if (s == 0) n->arg = xstrdup("front_page");
	return n;
}

static View *external_link(const char *href)
{
	Buf b = {0};
	bput(&b, "<h2>Link externo</h2><p>Este link abre fora do Canvas:</p><p><b>");
	besc(&b, href);
	bput(&b, "</b></p><p><small>O 3DS não consegue abrir sites modernos. Abra no celular ou no computador.</small></p>");
	View *n = open_doc_html("Link", b.s, L_STATIC);
	free(b.s);
	return n;
}

View *view_open_row(View *from, Row *r)
{
	View *n = NULL;
	switch (r->act) {
	case A_COURSE:
		n = view_new(V_LIST, L_COURSE, r->title);
		if (n) { snprintf(n->course, sizeof n->course, "%s", r->id); build_course(n); }
		return n;
	case A_SECTION: {
		n = section_view(r->course, r->section);
		if (n && from && from->title[0]) {
			char t[96];
			snprintf(t, sizeof t, "%s", n->title);
			snprintf(n->title, sizeof n->title, "%s", t);
		}
		return n;
	}
	case A_ASSIGNMENT:
		n = view_new(V_DOC, L_ASSIGNMENT, r->title);
		if (!n) return NULL;
		snprintf(n->course, sizeof n->course, "%s", r->course);
		snprintf(n->id, sizeof n->id, "%s", r->id);
		if (r->html) static_doc(n, r->html);
		return n;
	case A_ANNOUNCEMENT:
	case A_DOCROW:
		return open_doc_html(r->title, r->html ? r->html : "", L_STATIC);
	case A_PAGE:
		n = view_new(V_DOC, L_PAGE, r->title);
		if (n) { snprintf(n->course, sizeof n->course, "%s", r->course); n->arg = xstrdup(r->extra); }
		return n;
	case A_DISCUSSION:
		n = view_new(V_DOC, L_DISCUSSION, r->title);
		if (n) { snprintf(n->course, sizeof n->course, "%s", r->course); snprintf(n->id, sizeof n->id, "%s", r->id); }
		return n;
	case A_FILE:
		n = view_new(V_DOC, L_FILE, r->title);
		if (n) snprintf(n->id, sizeof n->id, "%s", r->id);
		return n;
	case A_INBOX: return view_new(V_LIST, L_INBOX, "Caixa de entrada");
	case A_CONVERSATION:
		n = view_new(V_DOC, L_CONVERSATION, r->title);
		if (n) snprintf(n->id, sizeof n->id, "%s", r->id);
		return n;
	case A_ALL_ANNOUNCEMENTS: return view_new(V_LIST, L_ALL_ANNOUNCEMENTS, "Avisos");
	case A_SETUP: return open_doc_html("Conectar ao Canvas", setup_html(), L_SETUP);
	case A_URL: return view_open_link(from, r->extra);
	case A_MODULE_ITEM: {
		char type[32] = "", *u = NULL;
		if (r->extra) {
			const char *tab = strchr(r->extra, '\t');
			size_t l = tab ? (size_t)(tab - r->extra) : strlen(r->extra);
			if (l >= sizeof type) l = sizeof type - 1;
			memcpy(type, r->extra, l);
			type[l] = 0;
			u = tab ? (char *)tab + 1 : NULL;
		}
		if (!strcmp(type, "Page")) { n = view_new(V_DOC, L_PAGE, r->title); if (n) n->arg = xstrdup(u); }
		else if (!strcmp(type, "Assignment")) n = view_new(V_DOC, L_ASSIGNMENT, r->title);
		else if (!strcmp(type, "Discussion")) n = view_new(V_DOC, L_DISCUSSION, r->title);
		else if (!strcmp(type, "File")) n = view_new(V_DOC, L_FILE, r->title);
		else if (!strcmp(type, "Quiz")) n = view_new(V_DOC, L_QUIZ, r->title);
		else if (!strcmp(type, "ExternalUrl") && u && *u) return external_link(u);
		else {
			Buf b = {0};
			bput(&b, "<h2>"); besc(&b, r->title);
			bput(&b, "</h2><p>Este item usa uma ferramenta que só funciona no site ou no app do Canvas no celular.</p>");
			n = open_doc_html(r->title, b.s, L_STATIC);
			free(b.s);
			return n;
		}
		if (n) { snprintf(n->course, sizeof n->course, "%s", r->course); snprintf(n->id, sizeof n->id, "%s", r->id); }
		return n;
	}
	case A_SETTINGS: {
		switch (r->section) {
		case 1: {
			char t[256] = "";
			if (keyboard("Cole ou digite o token do Canvas", t, sizeof t, false) && t[0]) {
				config_save_token(t);
				net_set_auth(g_cfg.token);
				toast("Token salvo. Volte para a tela inicial.");
			}
			break;
		}
		case 2:
			config_load();
			net_set_auth(g_cfg.token);
			toast(g_cfg.token[0] ? "Token lido do cartão SD" : "Não achei 3ds/Canvas3DS/token.txt");
			break;
		case 3: {
			char t[128];
			snprintf(t, sizeof t, "%s", g_cfg.domain);
			if (keyboard("Endereço do Canvas (ex.: faculdade.instructure.com)", t, sizeof t, false) && t[0]) {
				snprintf(g_cfg.domain, sizeof g_cfg.domain, "%s", t);
				config_save();
				config_load();
			}
			break;
		}
		case 4: break;   // handled by left/right in main
		case 6:
			g_cfg.text_size = (g_cfg.text_size + 1) % 3;
			config_save();
			break;
		case 7:
			if (updater_state() == UPD_AVAILABLE) ui_show_update();
			else if (!updater_busy() && updater_state() != UPD_DONE) updater_check();
			break;
		case 8: return open_doc_html("Como gerar o token", setup_html(), L_SETUP);
		}
		if (from && from->loader == L_SETTINGS) {
			int sel = from->sel;
			build_settings(from);
			from->sel = sel;
		}
		return NULL;
	}
	default: return NULL;
	}
}

View *view_settings(void)
{
	View *v = view_new(V_LIST, L_SETTINGS, "Ajustes");
	if (v) build_settings(v);
	return v;
}

void view_refresh_settings(View *v)
{
	if (v && v->loader == L_SETTINGS) { int s = v->sel; build_settings(v); v->sel = s; }
}

// Turns a Canvas link from inside a page into the matching screen.
View *view_open_link(View *from, const char *href)
{
	if (!href || !*href || href[0] == '#') return NULL;
	char path[700];
	const char *p = href;
	char base[160];
	snprintf(base, sizeof base, "https://%s", g_cfg.domain);
	if (!strncmp(p, base, strlen(base))) p += strlen(base);
	else if (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8) || !strncmp(p, "mailto:", 7)) return external_link(href);
	snprintf(path, sizeof path, "%s", p);
	char *q = strpbrk(path, "?#");
	if (q) *q = 0;
	if (!strncmp(path, "/api/v1", 7)) memmove(path, path + 7, strlen(path + 7) + 1);

	char c[32] = "", a[32] = "", what[40] = "", rest[300] = "";
	if (sscanf(path, "/courses/%31[0-9]/%39[a-z_]/%299s", c, what, rest) >= 2 || sscanf(path, "/courses/%31[0-9]", c) == 1) {
		View *n = NULL;
		char *slash = strchr(rest, '/');
		if (slash) *slash = 0;
		snprintf(a, sizeof a, "%.31s", rest);
		if (!what[0]) {
			n = view_new(V_LIST, L_COURSE, course_name(c)[0] ? course_name(c) : "Curso");
			if (n) { snprintf(n->course, sizeof n->course, "%s", c); build_course(n); }
			return n;
		}
		if (!strcmp(what, "pages") || !strcmp(what, "wiki")) {
			if (!rest[0]) { n = section_view(c, 4); return n; }
			n = view_new(V_DOC, L_PAGE, "Página");
			if (n) { n->arg = xstrdup(rest); }
		} else if (!strcmp(what, "assignments")) {
			if (!a[0] || !isdigit((unsigned char)a[0])) return section_view(c, 2);
			n = view_new(V_DOC, L_ASSIGNMENT, "Tarefa");
		} else if (!strcmp(what, "discussion_topics") || !strcmp(what, "announcements")) {
			if (!a[0] || !isdigit((unsigned char)a[0])) return section_view(c, !strcmp(what, "announcements") ? 1 : 5);
			n = view_new(V_DOC, L_DISCUSSION, "Discussão");
		} else if (!strcmp(what, "files")) {
			if (!a[0] || !isdigit((unsigned char)a[0])) return section_view(c, 6);
			n = view_new(V_DOC, L_FILE, "Arquivo");
		} else if (!strcmp(what, "quizzes")) {
			if (!a[0] || !isdigit((unsigned char)a[0])) return section_view(c, 2);
			n = view_new(V_DOC, L_QUIZ, "Questionário");
		} else if (!strcmp(what, "modules")) return section_view(c, 3);
		else if (!strcmp(what, "grades")) return section_view(c, 7);
		else return external_link(href);
		if (n) { snprintf(n->course, sizeof n->course, "%s", c); snprintf(n->id, sizeof n->id, "%s", a); }
		return n;
	}
	if (sscanf(path, "/files/%31[0-9]", a) == 1 || sscanf(path, "/users/%*[0-9]/files/%31[0-9]", a) == 1) {
		View *n = view_new(V_DOC, L_FILE, "Arquivo");
		if (n) snprintf(n->id, sizeof n->id, "%s", a);
		return n;
	}
	if (!strncmp(path, "/conversations", 14)) return view_new(V_LIST, L_INBOX, "Caixa de entrada");
	return external_link(href);
}

// ---------------------------------------------------------------- actions

void view_reply(View *v)
{
	if (!v || v->loader != L_CONVERSATION || v->job3) return;
	char text[1500] = "";
	if (!keyboard("Sua resposta", text, sizeof text, true) || !text[0]) return;
	char enc[4600], form[4700], url[300];
	url_encode(text, enc, sizeof enc);
	snprintf(form, sizeof form, "body=%s", enc);
	snprintf(url, sizeof url, "%s/api/v1/conversations/%s/add_message", api_base(), v->id);
	v->job3 = net_post(url, form);
	snprintf(v->note, sizeof v->note, "Enviando...");
}

void view_download(View *v)
{
	if (!v || v->loader != L_FILE || !v->arg || v->job3) return;
	mkdir(APP_DIR "/arquivos", 0777);
	char name[128];
	size_t o = 0;
	for (const char *s = v->title; *s && o + 1 < sizeof name; s++)
		name[o++] = strchr("\\/:*?\"<>|", *s) ? '_' : *s;
	name[o] = 0;
	char path[256];
	snprintf(path, sizeof path, APP_DIR "/arquivos/%s", name[0] ? name : "arquivo");
	// the signed URL needs no token, and must not get it (it points to another server)
	v->job3 = net_download(v->arg, path);
	snprintf(v->note, sizeof v->note, "Baixando...");
}
