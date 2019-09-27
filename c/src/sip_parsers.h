#pragma once

#ifdef __CHAR_UNSIGNED__
#define ui8     char
#else
#define ui8     unsigned char
#endif

int read_unsigned (ui8 ** p, ui8 const * const e);
