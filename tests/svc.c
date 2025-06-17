int main() {
    __asm__ volatile (
        "mov x8, #93\n"      // syscall number for exit
        "mov x0, #0\n"       // status code 0
        "svc #0\n"           // make syscall
    );
    __builtin_unreachable();
}
