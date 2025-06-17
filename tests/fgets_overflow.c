#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

void secure_function() {
    exit(0);
}

__attribute__((noinline)) void force_frame() {
    __asm__ __volatile__("");
}

__attribute__((noinline)) void vulnerable() {
    force_frame();

    char buf[8];
    fgets(buf, 256, stdin); // buffer overflow via oversized input
}

int main() {
    vulnerable();
    return 1;
}


// secure_function: 0x21bbd8

// Run with:
// python3 -c 'import sys; sys.stdout.buffer.write(b"A"*16 + b"\xd8\xbb\x21\x00\x00\x00\x00\x00")' > tests/fgets_overflow_payload.bin
// TRACEE_ARGS="< ../../tests/fgets_overflow_payload.bin" TRACEE=tests/fgets_overflow make trace