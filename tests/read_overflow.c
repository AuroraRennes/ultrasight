#include <unistd.h>
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
    read(0, buf, 32); // raw input, allows null bytes
}

int main() {
    vulnerable();
    return 1;
}


// secure_function address: 0x216768
// python3 -c 'import sys; sys.stdout.buffer.write(b"A"*16 + b"\x68\x67\x21\x00\x00\x00\x00\x00")' > tests/read_overflow_payload.bin
// TRACEE_ARGS="< ../../tests/read_overflow_payload.bin" TRACEE=tests/read_overflow make trace