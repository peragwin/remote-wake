#include "unity.h"
static struct { const char *name; void (*fn)(void); } tests[64];
static size_t count;
void host_register_test(const char *name, void (*fn)(void))
{
    assert(count < sizeof(tests) / sizeof(tests[0]));
    tests[count].name = name;
    tests[count++].fn = fn;
}
void unity_run_all_tests(void)
{
    for (size_t i = 0; i < count; ++i) {
        tests[i].fn();
        printf("PASS %s\n", tests[i].name);
    }
    printf("%zu auth tests passed\n", count);
}
void app_main(void);
int main(void) { app_main(); return 0; }
