#include "fslib.h"
#include <string.h>
#include <stdlib.h>

int FSLIB_get_date_into(uint16_t data, int type, Date* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));

    if (type == 1) { /* date */
        out->year   = (uint16_t)(((data >> 9) & 0x7F) + 1980);
        out->mounth = (uint16_t)((data >> 5) & 0x0F);
        out->day    = (uint16_t)(data & 0x1F);
        return 1;
    }
    if (type == 2) { /* time */
        out->hour   = (uint16_t)((data >> 11) & 0x1F);
        out->minute = (uint16_t)((data >> 5) & 0x3F);
        out->second = (uint16_t)((data & 0x1F) * 2);
        return 1;
    }
    return 0;
}

Date* FSLIB_get_date(uint16_t data, int type) {
    Date* date = (Date*)malloc(sizeof(Date));
    if (!date) return NULL;
    if (!FSLIB_get_date_into(data, type, date)) {
        free(date);
        return NULL;
    }
    return date;
}


static const char* last_sep(const char* s) {
    const char* a = strrchr(s, '\\');
    const char* b = strrchr(s, '/');
    if (!a) return b;
    if (!b) return a;
    return (a > b) ? a : b;
}

int FSLIB_change_path_into(const char* currentPath, const char* content, char* out, size_t outsz) {
    if (!out || outsz == 0) return 0;
    out[0] = '\0';

    if (!currentPath) currentPath = "";

    if (!content || content[0] == '\0') {
        const char* sep = last_sep(currentPath);
        if (!sep) return 0;
        size_t parentLen = (size_t)(sep - currentPath);
        if (parentLen + 1 > outsz) return 0;
        memcpy(out, currentPath, parentLen);
        out[parentLen] = '\0';
        return 1;
    }

    size_t curLen = strlen(currentPath);
    size_t addSep = (curLen > 0 && currentPath[curLen - 1] != '\\' && currentPath[curLen - 1] != '/') ? 1 : 0;
    size_t need = curLen + addSep + strlen(content) + 1;
    if (need > outsz) return 0;

    memcpy(out, currentPath, curLen);
    size_t pos = curLen;
    if (addSep) out[pos++] = '\\';
    strcpy(out + pos, content);
    return 1;
}

char* FSLIB_change_path(const char* currentPath, const char* content) {
    if (!currentPath) currentPath = "";
    size_t cap = strlen(currentPath) + (content ? strlen(content) : 0) + 4;
    char* s = (char*)malloc(cap);
    if (!s) return NULL;
    if (!FSLIB_change_path_into(currentPath, content, s, cap)) {
        free(s);
        return NULL;
    }
    return s;
}
