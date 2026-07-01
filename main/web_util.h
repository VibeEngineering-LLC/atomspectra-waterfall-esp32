#pragma once
#include <stddef.h>

// #DEDUP-1: общие утилиты веб-слоя (ранее дублировались в web_server.c и
// web_waterfall.c одинаковыми static-копиями).

// XML-escape строки из прибора (serial и т.п.), уходящей в N42/XML-разметку:
// заменяет & < > " ' на сущности. Пишет не более cap-1 байт + '\0'; при нехватке
// места усекает на границе символа (запас 6 байт на самую длинную "&quot;").
// in и out не должны перекрываться.
void web_xml_escape(const char *in, char *out, size_t cap);
