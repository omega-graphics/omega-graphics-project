#include "engine.h"
#include <cstdio>

static int initialized = 0;

void engine_init() {
    initialized = 1;
}

int engine_run() {
    if (!initialized)
        return -1;
    printf("Engine running\n");
    return 0;
}
