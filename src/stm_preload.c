#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

/**
 * This code is expected to be compiled as a shared library and passed through
 * LD_PRELOAD. It (1) opens the /dev/mem file descriptor and maps the STM region
 * in user-space and (2) store the pointer to that region in the thread local
 * storage (tls).
 */

#define STM_STIMULUS_BASE 0xF8000000UL
#define STM_MAP_SIZE 0x1000

__thread void* stm_region_ptr = NULL;

/* __attribute__((constructor)) tells GCC/Clang to run this function
 * automatically before main() */
__attribute__((constructor)) void preload_stm_region(void)
{
  /* Open /dev/mem to access the STM address */
  int fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (fd < 0) {
    fprintf(stderr, "[stm_preload] could not open /dev/mem\n");
    return;
  }

  /* Map the STM region in userspace */
  void* mapped_region = mmap(NULL, STM_MAP_SIZE, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, STM_STIMULUS_BASE);
  if (mapped_region == MAP_FAILED) {
    fprintf(stderr, "[stm_preload] could not mmap /dev/mem\n");
    close(fd);
    mapped_region = NULL;
    return;
  }

  /* Close the /dev/mem file descriptor */
  close(fd);

  /* Store the virtual address in the TLS */
  stm_region_ptr = mapped_region;
  fprintf(stderr, "[stm_preload] STM region mapped at %p, TLS set\n",
          mapped_region);

  /* Load the TLS pointer into x28 */
  __asm__ __volatile__("mov x28, %0" ::"r"(stm_region_ptr));
}

/* __attribute__((constructor)) tells GCC/Clang to run this function
 * automatically after main() */
__attribute__((destructor)) void unload_stm_region(void)
{
  if (stm_region_ptr != NULL) {
    munmap(stm_region_ptr, STM_MAP_SIZE);
    fprintf(stderr, "[stm_preload] STM region unmapped\n");
  }
}
