#include <cstdio>
#include <cstring>
#include "codec.h"

int main() {
    const char *msg = "Hello";
    int len = (int)strlen(msg);
    char encoded[16] = {};
    char decoded[16] = {};

    encode(msg, encoded, len);
    decode(encoded, decoded, len);

    printf("Original: %s\n", msg);
    printf("Decoded:  %s\n", decoded);

    return (strcmp(msg, decoded) == 0) ? 0 : 1;
}
