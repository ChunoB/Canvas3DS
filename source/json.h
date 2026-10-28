#pragma once
#include <stdbool.h>
#include <stddef.h>

// Small JSON parser. A whole document lives in one arena and is freed at once.
typedef enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JNode JNode;
struct JNode {
	JType  type;
	char  *key;      // member name inside an object (NULL in arrays)
	char  *str;      // string value, or the number as written (ids stay exact)
	double num;
	JNode *child;    // first element/member
	JNode *next;     // next sibling
	int    count;    // number of children
};

typedef struct JDoc JDoc;

JDoc       *json_parse(const char *text, size_t len);   // NULL on error
void        json_free(JDoc *d);
JNode      *json_root(JDoc *d);

JNode      *json_get(const JNode *obj, const char *key);           // member or NULL
const char *json_str(const JNode *obj, const char *key);           // string/number text or NULL
double      json_num(const JNode *obj, const char *key, double def);
bool        json_bool(const JNode *obj, const char *key);
const char *json_path(const JNode *obj, const char *path);         // "a.b.c" -> string
#define     json_each(arr, it) for (JNode *it = (arr) && ((arr)->type == J_ARR || (arr)->type == J_OBJ) ? (arr)->child : NULL; it; it = it->next)
