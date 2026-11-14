#include "test.h"

int g_checks, g_failed;

int main(void)
{
    test_core();
    test_net_util();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
