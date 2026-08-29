#include "spectrum_t1.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

float spectrum_temp_from_token(const char *p)
{
    if (p == NULL || *p == '\0')
        return NAN;

    while (*p != '\0' && isspace((unsigned char)*p))
        p++;
    if (*p == '\0')
        return NAN;

    if (p[0] && p[1] && p[2] &&
        tolower((unsigned char)p[0]) == 'o' &&
        tolower((unsigned char)p[1]) == 'f' &&
        tolower((unsigned char)p[2]) == 'f') {
        char next = p[3];
        if (next == '\0' || next == ' ' || next == '\n' || next == '\r')
            return NAN;
    }

    char *end = NULL;
    float v = strtof(p, &end);
    if (end == p)
        return NAN;
    return v;
}

int spectrum_temp_json(char *buf, size_t cap, float t)
{
    int n;

    if (buf == NULL || cap < 2)
        return -1;
    if (isnan(t))
        n = snprintf(buf, cap, "null");
    else
        n = snprintf(buf, cap, "%.1f", (double)t);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return n;
}
