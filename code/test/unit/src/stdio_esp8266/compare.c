// host check of arch/esp8266/stdio_esp8266.c (vsnprintf): every case against the C library of the host
//   gcc -O1 -ffp-contract=off -Ihost -o compare compare.c && ./compare      (WSL; host/esp_libc.h is empty)
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define vsnprintf own_vsnprintf
#define snprintf own_snprintf
#define vsprintf own_vsprintf
#define sprintf own_sprintf
#define vprintf own_vprintf
#define printf own_printf
#define putchar own_putchar
#define puts own_puts
#define __assert_func own_assert_func
static int ets_vprintf(const char* f, va_list a) { (void)f; (void)a; return 0; }
static void ets_putc(char c) { (void)c; }
static int ets_printf(const char* f, ...) { (void)f; return 0; }

#include "../../../../espurna/arch/esp8266/stdio_esp8266.c"
#undef vsnprintf
#undef snprintf
#undef printf

static int failed;

static void check(const char* format, ...) {
    char own[256];
    char ref[256];
    va_list a, b;
    va_start(a, format);
    va_copy(b, a);
    const int n1 = own_vsnprintf(own, sizeof(own), format, a);
    const int n2 = vsnprintf(ref, sizeof(ref), format, b);
    va_end(a);
    va_end(b);
    if (n1 != n2 || strcmp(own, ref)) {
        ++failed;
        printf("FAIL %-12s own [%s] %d  ref [%s] %d\n", format, own, n1, ref, n2);
    }
}

int main(void) {
    check("%d %i %u", -42, 7, 3000000000u);
    check("%5d|%-5d|%05d|%+d|% d", 42, 42, 42, 42, 42);
    check("%.3d|%.0d|%8.3d", 7, 0, -7);
    check("%x %X %#x %#o %o %08x", 255, 255, 255, 8, 0, 0xbeef);
    check("%hhu %hhd %hu %hd", 300, 200, 70000, 40000);
    check("%lu %ld %lx", 4000000000ul, -5l, 0xdeadbeeful);
    check("%llu %lld %llx", 18446744073709551615ull, -9223372036854775807ll, 0x123456789abcull);
    check("%zu %zd %td %jd", (size_t)12345, (ssize_t)-3, (ptrdiff_t)-9, (intmax_t)-77);
    check("%c%c|%3c|%-3c|", 'a', 'b', 'x', 'y');
    check("%s|%10s|%-10s|%.3s|%*s|%-*.*s|", "abc", "abc", "abc", "abcdef", 6, "ab", 6, 2, "abcd");
    check("%%|%5%|");
    check("%f %f %f %f", 0.0, 1.5, -2.25, 123456.789);
    check("%.2f %.0f %.1f %8.3f %-8.2f| %+.2f %08.2f", 3.14159, 2.5, 0.05, 3.14159, 1.0, 1.0, -1.5);
    check("%.3f %.1f %.2f", 0.0005, 9.96, 99.995);
    check("%e %E %.2e %.0e", 12345.678, 0.000123, 1.0, 5e10);
    check("%g %g %g %g %g", 0.0, 100000.0, 1000000.0, 0.0001, 0.00001);
    check("%g %.3g %G %g %#g", 3.14159, 2.0, 1e-10, 123.456, 1.0);
    check("%.10g %g", 1.0 / 3.0, 9.9999999);
    check("%f %f %F", 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0 > 0 ? 1.0 : (1.0 / 0.0));
    check("%5.1f|%-7.2e|", 21.345, 0.5);
    check("%p", (void*)0x3fff1234);
    check("relay%zu\t{Prov=%.*s Status=%.*s}", (size_t)3, 4, "gpio", 2, "ONxx");
    check("[%06lu] %s", 12345ul, "x");

    char small[4];
    const int n = own_snprintf(small, sizeof(small), "%s", "abcdef");
    if (n != 6 || strcmp(small, "abc")) { ++failed; printf("FAIL truncation [%s] %d\n", small, n); }
    if (own_snprintf(NULL, 0, "%d", 12345) != 5) { ++failed; printf("FAIL size query\n"); }

    printf("%s, %d failed\n", failed ? "FAILED" : "OK", failed);
    return failed != 0;
}
