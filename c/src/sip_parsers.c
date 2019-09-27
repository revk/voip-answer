#include "sip_parsers.h"
#include <ctype.h>

int read_unsigned (ui8 ** p, ui8 const *const e) {
   int v = 0;
   while (*p < e && isdigit (**p))
      v = v * 10 + (*(*p)++ - '0');
   return v;
}
