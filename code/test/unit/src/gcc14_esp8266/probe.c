/* GCC 14.2 (toolchain-xtensa-esp-elf, -mdynconfig=xtensa_esp8266.so) on the lx106 core: the code must use the call0
   ABI (call0/ret, no entry/retw) and no instructions the core lacks (regress-todo.md item 35) */
int probe_mul(int a, int b) { return a * b; }
long long probe_mul64(long long a, long long b) { return a * b; }
int probe_div(int a, int b) { return a / b; }
float probe_float(float a, float b) { return a * b + 1.0f; }
int probe_call(int x) { return probe_mul(x, 3) + probe_div(x, 7); }
