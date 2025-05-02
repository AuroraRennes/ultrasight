#include <stdio.h>

int main() {
    void* x28_val;
    __asm__ __volatile__("mov %0, x28" : "=r"(x28_val));
    printf("[tracee] x28: %p\n", x28_val);
    return 0;
}
