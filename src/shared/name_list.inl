/* The lists of input and event names a device keeps, "input3, input4": whether
 * one name is in a list, and adding the ones that are not.
 *
 * ⛔⛔ WHOLE NAMES, NEVER SUBSTRINGS (2026-09-14). The lists were searched with
 * strstr, so "input3" was found inside "input30". After the U5s restarted and
 * numbered a GameSir's pad input3 and an Xbox One S pad input30, the GameSir's
 * pad was taken for part of the One S: it vanished from the device list and no
 * stream start bridged it. The same search skipped adding "event3" to a list
 * already holding "event30".
 *
 * Pure: strings in, strings out, so tests/test_name_list.c checks it. */

#ifndef NAME_LIST_INL
#define NAME_LIST_INL

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Names are separated by commas, with any spaces around them. */
static inline int name_list_has(const char *list, const char *name)
{
    if (!list || !name || !name[0]) return 0;
    const size_t len = strlen(name);
    const char *p = list;
    while (*p) {
        while (*p == ',' || *p == ' ') ++p;
        const char *start = p;
        while (*p && *p != ',') ++p;
        const char *end = p;
        while (end > start && end[-1] == ' ') --end;
        if ((size_t)(end - start) == len && strncmp(start, name, len) == 0) return 1;
    }
    return 0;
}

/* Adds each name in `names` -- one name, or a list of them -- that `list` does
 * not already hold, in the order they come. */
static inline void name_list_add(char *list, size_t list_len, const char *names)
{
    if (!list || list_len == 0 || !names) return;
    const char *p = names;
    while (*p) {
        while (*p == ',' || *p == ' ') ++p;
        const char *start = p;
        while (*p && *p != ',') ++p;
        const char *end = p;
        while (end > start && end[-1] == ' ') --end;
        size_t len = (size_t)(end - start);
        if (len == 0) continue;
        char one[64];
        if (len >= sizeof(one)) len = sizeof(one) - 1;
        memcpy(one, start, len);
        one[len] = '\0';
        if (name_list_has(list, one)) continue;
        const size_t used = strlen(list);
        /* ⓘ Only a whole name goes in: a cut one would be a different name. */
        if (used + (used ? 2 : 0) + len + 1 > list_len) return;
        snprintf(list + used, list_len - used, "%s%s", used ? ", " : "", one);
    }
}

#endif /* NAME_LIST_INL */
