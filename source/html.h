#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// Lightweight HTML renderer: no CSS or JavaScript, just readable text with headings,
// lists, quotes, tables and tappable links. Built once (layout), drawn every frame.
typedef struct Doc Doc;

Doc        *html_layout(const char *html, float width, float scale);
void        html_free(Doc *d);
float       html_height(const Doc *d);
// Draws the part of the document between doc_y and doc_y + view_h at screen (ox, oy).
void        html_draw(const Doc *d, float doc_y, float view_h, float ox, float oy, int focus_link);
int         html_hit(const Doc *d, float x, float y);          // doc coords -> link or -1
int         html_nlinks(const Doc *d);
const char *html_link(const Doc *d, int i);
float       html_link_y(const Doc *d, int i);                  // top of the link's first line
// Next link whose top is >= doc_y (dir 1) or the previous one (dir -1); -1 if none.
int         html_link_near(const Doc *d, int from, int dir, float doc_y, float view_h);

// Plain text of an HTML fragment (tags removed, entities decoded, spaces collapsed).
void        html_to_text(const char *html, char *out, size_t n);
