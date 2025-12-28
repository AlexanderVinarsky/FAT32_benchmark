#include <dtime.h>

unsigned short DTM_current_time() {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    unsigned short hour = (unsigned short)(tmv.tm_hour & 0x1F);
    unsigned short min  = (unsigned short)(tmv.tm_min  & 0x3F);
    unsigned short sec2 = (unsigned short)((tmv.tm_sec / 2) & 0x1F);

    return (unsigned short)((hour << 11) | (min << 5) | sec2);
}

unsigned short DTM_current_date() {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    int year = tmv.tm_year + 1900;
    if (year < 1980) year = 1980;
    if (year > 2107) year = 2107;

    unsigned short y = (unsigned short)((year - 1980) & 0x7F);
    unsigned short m = (unsigned short)((tmv.tm_mon + 1) & 0x0F);
    unsigned short d = (unsigned short)(tmv.tm_mday & 0x1F);

    return (unsigned short)((y << 9) | (m << 5) | d);
}
