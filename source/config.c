#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

Config g_cfg;

static void trim(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
	size_t i = 0;
	while (s[i] == ' ' || s[i] == '\t' || (u8)s[i] == 0xEF || (u8)s[i] == 0xBB || (u8)s[i] == 0xBF) i++;   // BOM from Notepad
	if (i) memmove(s, s + i, n - i + 1);
}

// Accepts "einstein.instructure.com", "https://einstein.instructure.com/profile/settings"...
static void clean_domain(char *d)
{
	char *p = strstr(d, "://");
	if (p) memmove(d, p + 3, strlen(p + 3) + 1);
	p = strchr(d, '/');
	if (p) *p = 0;
	trim(d);
}

void config_load(void)
{
	memset(&g_cfg, 0, sizeof g_cfg);
	snprintf(g_cfg.domain, sizeof g_cfg.domain, "einstein.instructure.com");
	g_cfg.tz = -180;
	g_cfg.text_size = 1;
	mkdir("sdmc:/3ds", 0777);
	mkdir(APP_DIR, 0777);

	FILE *f = fopen(APP_DIR "/config.ini", "r");
	if (f) {
		char line[300];
		while (fgets(line, sizeof line, f)) {
			trim(line);
			char *eq = strchr(line, '=');
			if (!eq) continue;
			*eq = 0;
			if (!strcmp(line, "domain") && eq[1]) { snprintf(g_cfg.domain, sizeof g_cfg.domain, "%s", eq + 1); clean_domain(g_cfg.domain); }
			else if (!strcmp(line, "tz")) g_cfg.tz = atoi(eq + 1);
			else if (!strcmp(line, "text_size")) g_cfg.text_size = atoi(eq + 1);
		}
		fclose(f);
	}
	if (g_cfg.text_size < 0 || g_cfg.text_size > 2) g_cfg.text_size = 1;

	f = fopen(APP_DIR "/token.txt", "r");
	if (f) {
		char t[300] = "";
		if (fgets(t, sizeof t, f)) { trim(t); snprintf(g_cfg.token, sizeof g_cfg.token, "%s", t); }
		fclose(f);
	}
}

void config_save(void)
{
	FILE *f = fopen(APP_DIR "/config.ini", "w");
	if (!f) return;
	fprintf(f, "domain=%s\ntz=%d\ntext_size=%d\n", g_cfg.domain, g_cfg.tz, g_cfg.text_size);
	fclose(f);
}

bool config_save_token(const char *tok)
{
	char t[300];
	snprintf(t, sizeof t, "%s", tok);
	trim(t);
	FILE *f = fopen(APP_DIR "/token.txt", "w");
	if (!f) return false;
	fprintf(f, "%s\n", t);
	fclose(f);
	snprintf(g_cfg.token, sizeof g_cfg.token, "%s", t);
	return true;
}
