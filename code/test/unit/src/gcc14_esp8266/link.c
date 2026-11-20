/* GCC 14.2 for lx106 against the newlib / libgcc of the 8.4 toolchain (interim of regress-todo.md item 35): the newlib
   headers and libc_nano of one version (struct _reent), the 64-bit and float helpers from libgcc */
#include <stdio.h>
#include <string.h>
int probe_link(char* out, size_t size, long long a, long long b, float f) {
    return snprintf(out, size, "%lld %d", a / b, (int) (f * 3.0f)) + (int) strlen(out);
}
void _start(void) {
    char buffer[32];
    probe_link(buffer, sizeof(buffer), 100, 7, 1.5f);
}
