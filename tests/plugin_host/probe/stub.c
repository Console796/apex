#include <stdint.h>
struct probe { uint32_t size; int (*add)(int, int); int (*times)(int, int); };
static int add(int a, int b) { return a + b; }
static int times(int a, int b) { return a * b; }
static const struct probe table = { sizeof(table), add, times };
__attribute__((visibility("default"))) const struct probe* probe_entry(void) { return &table; }
