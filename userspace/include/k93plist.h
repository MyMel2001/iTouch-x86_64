/* k93plist.h - minimal XML property-list parser (clean-room)
 * Scope: XML plists (dict/array/string/integer/real/true/false/data/date).
 * Ceiling: binary plists (bplist00) are rejected with a clear error; add a
 * bplist reader if a real .ipa needs it. */
#ifndef K93PLIST_H
#define K93PLIST_H
#include <stddef.h>
#include <stdio.h>

typedef enum {
    K93_PLIST_DICT,
    K93_PLIST_ARRAY,
    K93_PLIST_STRING,
    K93_PLIST_INT,
    K93_PLIST_REAL,
    K93_PLIST_BOOL,
    K93_PLIST_DATA,
    K93_PLIST_DATE
} k93_plist_type;

typedef struct k93_plist k93_plist;

k93_plist *k93_plist_parse(const char *xml, size_t len, char *errbuf, size_t errlen);
k93_plist *k93_plist_parse_file(const char *path, char *errbuf, size_t errlen);
void       k93_plist_free(k93_plist *p);

k93_plist_type k93_plist_type_of(const k93_plist *p);
/* dict */
const k93_plist *k93_plist_dict_get(const k93_plist *dict, const char *key);
const char *k93_plist_dict_str(const k93_plist *dict, const char *key, const char *dflt);
long        k93_plist_dict_int(const k93_plist *dict, const char *key, long dflt);
/* array */
int             k93_plist_array_count(const k93_plist *arr);
const k93_plist *k93_plist_array_at(const k93_plist *arr, int idx);
/* scalars */
const char *k93_plist_string(const k93_plist *p);             /* NULL if not a string */
const char *k93_plist_data(const k93_plist *p, size_t *len);  /* raw <data> text (base64), if any */
long        k93_plist_int(const k93_plist *p, long dflt);
int         k93_plist_bool(const k93_plist *p, int dflt);
/* serializer (used to write our own registry plists) */
int k93_plist_write_dict_header(FILE *f, int indent);
int k93_plist_write_string(FILE *f, const char *s);
int k93_plist_write_integer(FILE *f, long v);
int k93_plist_write_bool(FILE *f, int v);
int k93_plist_write_key(FILE *f, const char *k);

/* convenience: print a plist tree (debug/autotest) into malloc'd string */
char *k93_plist_dump(const k93_plist *p, int *len);

#endif
