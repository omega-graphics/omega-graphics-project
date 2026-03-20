#include "codec.h"

int encode(const char *input, char *output, int len) {
    for (int i = 0; i < len; ++i)
        output[i] = input[i] ^ 0x42;
    return len;
}

int decode(const char *input, char *output, int len) {
    for (int i = 0; i < len; ++i)
        output[i] = input[i] ^ 0x42;
    return len;
}
