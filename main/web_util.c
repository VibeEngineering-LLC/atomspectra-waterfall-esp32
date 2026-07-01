#include "web_util.h"
#include <stdio.h>

// P3-9: значения из прибора (serial) уходят в XML/N42-разметку → экранировать &<>"'.
void web_xml_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 6 < cap; p++) {
        switch (*p) {
        case '&':  o += snprintf(out + o, cap - o, "&amp;");  break;
        case '<':  o += snprintf(out + o, cap - o, "&lt;");   break;
        case '>':  o += snprintf(out + o, cap - o, "&gt;");   break;
        case '"':  o += snprintf(out + o, cap - o, "&quot;"); break;
        case '\'': o += snprintf(out + o, cap - o, "&apos;"); break;
        default:   out[o++] = *p; break;
        }
    }
    out[o] = '\0';
}
