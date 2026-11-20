// dtostrf() of the layer (String(float), sensors), as avr-libc's: `width` chars at least, right-aligned (negative:
// left-aligned), `prec` digits after the point. Without printf: the .c.impl of ArduinoCore-API went through
// sprintf("%f"), which pulled newlib's _printf_float (~10 KB) into every head for this one function

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "arduino-api/deprecated-avr-comp/avr/dtostrf.h"

char *dtostrf(double val, signed char width, unsigned char prec, char *sout) {
  // 309 digits of DBL_MAX, the point, the sign, the digits after the point
  char out[340];
  size_t pos = sizeof(out);
  out[--pos] = '\0';

  const int negative = signbit(val) ? 1 : 0;
  if (negative) {
    val = -val;
  }

  if (prec > 15) {
    prec = 15;
  }

  if (isnan(val)) {
    pos -= 3;
    memcpy(&out[pos], "nan", 3);
  } else {
    double scaled = val;
    for (unsigned char index = 0; index < prec; ++index) {
      scaled *= 10.0;
    }
    scaled = floor(scaled + 0.5);

    if (isinf(scaled)) {
      pos -= 3;
      memcpy(&out[pos], "inf", 3);
    } else {
      // every digit of the rounded integer, the point before the last `prec` of them; an integer double divides by
      // 10 exactly once its last digit is taken off
      unsigned digits = 0;
      do {
        if (prec && (digits == prec)) {
          out[--pos] = '.';
        }

        const double digit = fmod(scaled, 10.0);
        out[--pos] = (char)('0' + (int)digit);
        scaled = (scaled - digit) / 10.0;
        ++digits;
      } while (((scaled >= 1.0) || (digits <= prec)) && (pos > 2));
    }
  }

  if (negative) {
    out[--pos] = '-';
  }

  const size_t len = sizeof(out) - 1 - pos;
  const size_t room = (size_t)((width < 0) ? -width : width);
  const size_t pad = (room > len) ? (room - len) : 0;

  char *cursor = sout;
  if (width > 0) {
    memset(cursor, ' ', pad);
    cursor += pad;
  }

  memcpy(cursor, &out[pos], len);
  cursor += len;

  if (width < 0) {
    memset(cursor, ' ', pad);
    cursor += pad;
  }

  *cursor = '\0';
  return sout;
}
