/* k93spell.c - minimal English spellcheck (clean-room).
 * Dictionary is /usr/share/dict/words (copied from the build host when
 * present) or /etc/k93/words.txt. With no dictionary every word is accepted,
 * so the feature degrades to a no-op rather than flagging everything. */
#include "k93util.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char *dict;            /* lowercased, NUL-terminated, \n separated */
static int loaded;

static void load_dict(void)
{
    if (loaded) return;
    loaded = 1;
    const char *paths[] = { "/usr/share/dict/words", "/etc/k93/words.txt" };
    for (int i = 0; i < 2; i++) {
        size_t l = 0;
        char *c = k93_read_file(paths[i], &l);
        if (!c) continue;
        dict = k93_xmalloc(l + 1);
        for (size_t j = 0; j < l; j++) dict[j] = (char)tolower((unsigned char)c[j]);
        dict[l] = 0;
        free(c);
        return;
    }
}

int k93_spell_known(const char *word)
{
    if (!word || !*word) return 1;
    load_dict();
    if (!dict) return 1;                       /* no dictionary: accept all */
    char buf[64];
    size_t n = 0;
    for (; word[n] && n < sizeof buf - 1; n++) buf[n] = (char)tolower((unsigned char)word[n]);
    buf[n] = 0;
    if (!n) return 1;
    for (char *p = dict; *p; ) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        if (!strcmp(p, buf)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* Collect unknown alphabetic words from text (space separated) into out.
 * Returns the count found. Words shorter than 2 letters are ignored. */
int k93_spell_unknown(const char *text, char *out, size_t cap)
{
    out[0] = 0;
    if (!text) return 0;
    size_t o = 0;
    int count = 0;
    const char *p = text;
    while (*p) {
        while (*p && !isalpha((unsigned char)*p)) p++;
        if (!*p) break;
        char w[64];
        size_t n = 0;
        while (isalpha((unsigned char)*p) && n < sizeof w - 1) w[n++] = *p++;
        w[n] = 0;
        if (n >= 2 && !k93_spell_known(w) && o + n + 2 < cap) {
            if (count) out[o++] = ' ';
            memcpy(out + o, w, n);
            o += n;
            out[o] = 0;
            count++;
        }
    }
    out[o] = 0;
    return count;
}
