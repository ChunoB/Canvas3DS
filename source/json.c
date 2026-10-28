#include "json.h"
#include <stdlib.h>
#include <string.h>

typedef struct Block { struct Block *next; size_t used, cap; char data[]; } Block;

struct JDoc {
	Block *blocks;
	JNode *root;
	const char *p, *end;
	bool err;
	int depth;
};

static void *arena(JDoc *d, size_t n)
{
	n = (n + 7) & ~(size_t)7;
	Block *b = d->blocks;
	if (!b || b->used + n > b->cap) {
		size_t cap = n > 32 * 1024 ? n : 32 * 1024;
		b = malloc(sizeof(Block) + cap);
		if (!b) { d->err = true; return NULL; }
		b->next = d->blocks;
		b->used = 0;
		b->cap = cap;
		d->blocks = b;
	}
	void *r = b->data + b->used;
	b->used += n;
	return r;
}

static void ws(JDoc *d)
{
	while (d->p < d->end && (*d->p == ' ' || *d->p == '\t' || *d->p == '\n' || *d->p == '\r')) d->p++;
}

static int hexv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static unsigned u4(const char *s)
{
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		int h = hexv(s[i]);
		if (h < 0) return 0xFFFD;
		v = v * 16 + h;
	}
	return v;
}

static int put_utf8(char *o, unsigned cp)
{
	if (cp < 0x80) { o[0] = cp; return 1; }
	if (cp < 0x800) { o[0] = 0xC0 | (cp >> 6); o[1] = 0x80 | (cp & 63); return 2; }
	if (cp < 0x10000) { o[0] = 0xE0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 63); o[2] = 0x80 | (cp & 63); return 3; }
	o[0] = 0xF0 | (cp >> 18); o[1] = 0x80 | ((cp >> 12) & 63); o[2] = 0x80 | ((cp >> 6) & 63); o[3] = 0x80 | (cp & 63);
	return 4;
}

static char *parse_string(JDoc *d)
{
	// d->p is at the opening quote
	const char *s = ++d->p;
	size_t n = 0;
	while (s + n < d->end && s[n] != '"') { if (s[n] == '\\') n++; n++; }
	if (s + n >= d->end) { d->err = true; return NULL; }
	char *out = arena(d, n + 1);   // unescaped text is never longer
	if (!out) return NULL;
	size_t o = 0;
	const char *p = s, *e = s + n;
	while (p < e) {
		if (*p != '\\') { out[o++] = *p++; continue; }
		p++;
		switch (*p) {
		case 'n': out[o++] = '\n'; p++; break;
		case 't': out[o++] = '\t'; p++; break;
		case 'r': p++; break;
		case 'b': case 'f': p++; break;
		case 'u': {
			if (e - p < 5) { p = e; break; }
			unsigned cp = u4(p + 1);
			p += 5;
			if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
				unsigned lo = u4(p + 2);
				if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
			}
			if (cp == 0) cp = ' ';
			// \uXXXX takes 6 input bytes and at most 4 output bytes
			o += put_utf8(out + o, cp);
			break;
		}
		default: out[o++] = *p++; break;
		}
	}
	out[o] = 0;
	d->p = e + 1;
	return out;
}

static JNode *parse_value(JDoc *d);

static JNode *node(JDoc *d, JType t)
{
	JNode *n = arena(d, sizeof(JNode));
	if (n) { memset(n, 0, sizeof *n); n->type = t; }
	return n;
}

static JNode *parse_value(JDoc *d)
{
	ws(d);
	if (d->p >= d->end || d->err) { d->err = true; return NULL; }
	if (++d->depth > 64) { d->err = true; return NULL; }
	JNode *n = NULL;
	char c = *d->p;
	if (c == '{' || c == '[') {
		bool obj = c == '{';
		n = node(d, obj ? J_OBJ : J_ARR);
		if (!n) return NULL;
		d->p++;
		JNode **tail = &n->child;
		ws(d);
		if (d->p < d->end && *d->p == (obj ? '}' : ']')) { d->p++; d->depth--; return n; }
		for (;;) {
			char *key = NULL;
			if (obj) {
				ws(d);
				if (d->p >= d->end || *d->p != '"') { d->err = true; return NULL; }
				key = parse_string(d);
				ws(d);
				if (d->p >= d->end || *d->p != ':') { d->err = true; return NULL; }
				d->p++;
			}
			JNode *v = parse_value(d);
			if (!v) return NULL;
			v->key = key;
			*tail = v;
			tail = &v->next;
			n->count++;
			ws(d);
			if (d->p < d->end && *d->p == ',') { d->p++; continue; }
			if (d->p < d->end && *d->p == (obj ? '}' : ']')) { d->p++; break; }
			d->err = true;
			return NULL;
		}
	} else if (c == '"') {
		n = node(d, J_STR);
		if (n) n->str = parse_string(d);
	} else if (!strncmp(d->p, "true", 4)) { n = node(d, J_TRUE); d->p += 4; }
	else if (!strncmp(d->p, "false", 5)) { n = node(d, J_FALSE); d->p += 5; }
	else if (!strncmp(d->p, "null", 4)) { n = node(d, J_NULL); d->p += 4; }
	else if (c == '-' || (c >= '0' && c <= '9')) {
		const char *s = d->p;
		while (d->p < d->end && strchr("+-0123456789.eE", *d->p)) d->p++;
		n = node(d, J_NUM);
		if (n) {
			size_t l = d->p - s;
			n->str = arena(d, l + 1);
			if (n->str) { memcpy(n->str, s, l); n->str[l] = 0; n->num = strtod(n->str, NULL); }
		}
	} else d->err = true;
	d->depth--;
	return d->err ? NULL : n;
}

JDoc *json_parse(const char *text, size_t len)
{
	JDoc *d = calloc(1, sizeof(JDoc));
	if (!d) return NULL;
	d->p = text;
	d->end = text + len;
	d->root = parse_value(d);
	if (d->err || !d->root) { json_free(d); return NULL; }
	return d;
}

void json_free(JDoc *d)
{
	if (!d) return;
	for (Block *b = d->blocks; b;) { Block *n = b->next; free(b); b = n; }
	free(d);
}

JNode *json_root(JDoc *d) { return d ? d->root : NULL; }

JNode *json_get(const JNode *o, const char *key)
{
	if (!o || o->type != J_OBJ) return NULL;
	for (JNode *c = o->child; c; c = c->next)
		if (c->key && !strcmp(c->key, key)) return c;
	return NULL;
}

const char *json_str(const JNode *o, const char *key)
{
	JNode *n = json_get(o, key);
	return n && (n->type == J_STR || n->type == J_NUM) ? n->str : NULL;
}

double json_num(const JNode *o, const char *key, double def)
{
	JNode *n = json_get(o, key);
	return n && n->type == J_NUM ? n->num : def;
}

bool json_bool(const JNode *o, const char *key)
{
	JNode *n = json_get(o, key);
	return n && n->type == J_TRUE;
}

const char *json_path(const JNode *o, const char *path)
{
	char part[64];
	const JNode *cur = o;
	while (cur && *path) {
		const char *dot = strchr(path, '.');
		size_t l = dot ? (size_t)(dot - path) : strlen(path);
		if (l >= sizeof part) return NULL;
		memcpy(part, path, l);
		part[l] = 0;
		if (!dot) return json_str(cur, part);
		cur = json_get(cur, part);
		path = dot + 1;
	}
	return NULL;
}
