#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define STM_STIMULUS_BASE 0xF8000000UL
#define STM_MAP_SIZE      0x1000  // map 4 KB
#define STM_PORT_OFFSET(n) ((n) * 4)

int main() {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return 1;
    }

    void *map_base = mmap(NULL, STM_MAP_SIZE, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, STM_STIMULUS_BASE);
    if (map_base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    volatile uint32_t *stm_port0 = (volatile uint32_t *)((char *)map_base + STM_PORT_OFFSET(0));
    *stm_port0 = 0x12345678;

    printf("Wrote 0x12345678 to STM port 0\n");

    munmap(map_base, STM_MAP_SIZE);
    close(fd);
    return 0;
}
