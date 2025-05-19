#include <stdio.h>

int main() {
    void* x26_val;
    __asm__ __volatile__("mov %0, x26" : "=r"(x26_val));
#ifdef __GLIBC__
    printf("[tracee] x26: %p\n", x26_val);
#endif
    return 0;
}
