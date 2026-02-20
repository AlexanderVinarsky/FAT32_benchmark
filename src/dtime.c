#include "dtime.h"
#include <time.h>

static void dtm_localtime(time_t t, struct tm* out) {
#ifdef _WIN32
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}

uint16_t DTM_current_time(void) {
    time_t now = time(NULL);
    struct tm tmv;
    dtm_localtime(now, &tmv);

    uint16_t hour = (uint16_t)(tmv.tm_hour & 0x1F);
    uint16_t min  = (uint16_t)(tmv.tm_min  & 0x3F);
    uint16_t sec2 = (uint16_t)((tmv.tm_sec / 2) & 0x1F);

    return (uint16_t)((hour << 11) | (min << 5) | sec2);
}

uint16_t DTM_current_date(void) {
    time_t now = time(NULL);
    struct tm tmv;
    dtm_localtime(now, &tmv);

    int year = tmv.tm_year + 1900;
    if (year < 1980) year = 1980;
    if (year > 2107) year = 2107;

    uint16_t y = (uint16_t)((year - 1980) & 0x7F);
    uint16_t m = (uint16_t)((tmv.tm_mon + 1) & 0x0F);
    uint16_t d = (uint16_t)(tmv.tm_mday & 0x1F);

    return (uint16_t)((y << 9) | (m << 5) | d);
}
