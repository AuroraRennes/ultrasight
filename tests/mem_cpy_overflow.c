#include <string.h>
#include <stdlib.h>
#include <stdint.h>

void secure_function() {
    exit(0);
}

 // Force prologue/epilogue emission, storing/restoring LR from stack
__attribute__((noinline)) void force_frame() {
    __asm__ __volatile__("");
}

__attribute__((noinline)) void vulnerable(char *payload) {
    force_frame();

    // 8 bytes buffer + 8 bytes FP + 8 bytes LR
    char buf[8];
    memcpy(buf, payload, 32);
}

int main() {
    // Fill start of payload
    char payload[32];
    memset(payload, 'A', 24);

    // Address of secure_function, find with:
    //  - readlef -s <bin> | grep secure_function
    //  - p secure_function in gdb
    uint64_t addr = 0x00000000002164e8;

    // Add the address to the payload
    memcpy(payload + 24, &addr, 8);

    vulnerable(payload);
    return 1;
}
