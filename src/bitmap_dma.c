// bitmap_dma.c
#include "bitmap_dma.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Read "phys_addr" and "size" for a u-dma-buf device out of sysfs */
int udmabuf_sysfs_info(const char *name, unsigned long *phys_addr,
                       size_t *size)
{
    const char *root = "/sys/class/u-dma-buf";
    char path[256];
    char attr[64];
    struct stat sb;
    int fd;

    snprintf(path, sizeof(path), "%s/%s", root, name);
    if (stat(path, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        fprintf(stderr, "u-dma-buf device '%s' not found\n", name);
        return -1;
    }

    snprintf(path, sizeof(path), "%s/%s/phys_addr", root, name);
    if ((fd = open(path, O_RDONLY)) < 0) {
        perror("open phys_addr");
        return -1;
    }
    memset(attr, 0, sizeof(attr));
    if (read(fd, attr, sizeof(attr) - 1) < 0) {
        perror("read phys_addr");
        close(fd);
        return -1;
    }
    close(fd);
    if (sscanf(attr, "%lx", phys_addr) != 1) return -1;

    snprintf(path, sizeof(path), "%s/%s/size", root, name);
    if ((fd = open(path, O_RDONLY)) < 0) {
        perror("open size");
        return -1;
    }
    memset(attr, 0, sizeof(attr));
    if (read(fd, attr, sizeof(attr) - 1) < 0) {
        perror("read size");
        close(fd);
        return -1;
    }
    close(fd);
    if (sscanf(attr, "%zu", size) != 1) return -1;

    return 0;
}

/* Write a string to a u-dma-buf sysfs attribute */
static int udmabuf_sysfs_write(const char *name, const char *attr,
                               const char *val)
{
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/u-dma-buf/%s/%s", name, attr);

    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return n < 0 ? -1 : 0;
}

/* Configure the driver to sync [0, size) DMA_FROM_DEVICE and return an fd
 * that invalidates that range when written to, or -1 on failure. */
static int udmabuf_sync_open(const char *name, size_t size)
{
    char val[32];
    snprintf(val, sizeof(val), "%zu", size);

    if (udmabuf_sysfs_write(name, "sync_offset", "0") < 0 ||
        udmabuf_sysfs_write(name, "sync_size", val) < 0 ||
        udmabuf_sysfs_write(name, "sync_direction", "2") < 0)  /* DMA_FROM_DEVICE */
        return -1;

    char path[256];
    snprintf(path, sizeof(path), "/sys/class/u-dma-buf/%s/sync_for_cpu", name);
    return open(path, O_WRONLY);
}

int bitmap_dma_open(bitmap_dma_t *h, size_t bitmap_size)
{
    h->buf_size = bitmap_size;

    // Map DMA AXI-Lite control
    if (axi_regs_open(&h->dma, DMA_BASE, DMA_MAP_SIZE) < 0) {
        perror("axi_regs_open DMA");
        return -1;
    }

    // Map bitmap reader AXI-Lite control
    if (axi_regs_open(&h->reader, BITMAP_READER_BASE, BITMAP_READER_MAP_SIZE) < 0) {
        perror("axi_regs_open bitmap reader");
        axi_regs_close(&h->dma);
        return -1;
    }

    /* Everything about the destination buffer comes from its name: the
     * physical address and size from sysfs, the device node from /dev. */
    const char *udmabuf_name = getenv(BITMAP_UDMABUF_ENV);
    if (!udmabuf_name || !*udmabuf_name) udmabuf_name = UDMABUF_FUZZSIGHT_NAME;

    /* Tolerate a full device path as well as a bare name */
    const char *slash = strrchr(udmabuf_name, '/');
    if (slash) udmabuf_name = slash + 1;

    unsigned long dst_addr;
    size_t dst_size;
    if (udmabuf_sysfs_info(udmabuf_name, &dst_addr, &dst_size) < 0) {
        axi_regs_close(&h->dma);
        axi_regs_close(&h->reader);
        return -1;
    }
    if (dst_size < bitmap_size) {
        fprintf(stderr, "[!] u-dma-buf '%s' holds %zu bytes, bitmap needs %zu\n",
                udmabuf_name, dst_size, bitmap_size);
        axi_regs_close(&h->dma);
        axi_regs_close(&h->reader);
        return -1;
    }
    h->dst_addr = dst_addr;

    // Map udmabuf as destination
    char udmabuf_dev[256];
    snprintf(udmabuf_dev, sizeof(udmabuf_dev), "/dev/%s", udmabuf_name);
    /* Map cached and invalidate by hand after each transfer: reading the
     * 64 KiB bitmap through an O_SYNC (uncached) mapping costs ~350 us.
     * ULTRASIGHT_BITMAP_UNCACHED=1 restores the uncached mapping. */
    int uncached = getenv("ULTRASIGHT_BITMAP_UNCACHED") != NULL;
    h->sync_fd = uncached ? -1 : udmabuf_sync_open(udmabuf_name, bitmap_size);
    if (!uncached && h->sync_fd < 0) {
        fprintf(stderr, "[~] bitmap_dma: cannot set up cached mapping, using O_SYNC\n");
        uncached = 1;
    }
    h->udmabuf_fd = open(udmabuf_dev, uncached ? (O_RDWR | O_SYNC) : O_RDWR);
    if (h->udmabuf_fd < 0) {
        perror("open udmabuf");
        axi_regs_close(&h->dma);
        axi_regs_close(&h->reader);
        return -1;
    }

    h->buf = mmap(NULL, bitmap_size, PROT_READ | PROT_WRITE,
                  MAP_SHARED, h->udmabuf_fd, 0);
    if (h->buf == MAP_FAILED) {
        perror("mmap udmabuf");
        close(h->udmabuf_fd);
        axi_regs_close(&h->dma);
        axi_regs_close(&h->reader);
        return -1;
    }

    // Reset and configure S2MM once
    axi_regs_write(&h->dma, S2MM_CONTROL_REGISTER, RESET_DMA);
    axi_regs_write(&h->dma, S2MM_CONTROL_REGISTER, HALT_DMA);
    axi_regs_write(&h->dma, S2MM_CONTROL_REGISTER, ENABLE_ALL_IRQ);
    axi_regs_write(&h->dma, S2MM_DST_ADDRESS_REGISTER, h->dst_addr);
    axi_regs_write(&h->dma, S2MM_CONTROL_REGISTER, RUN_DMA | ENABLE_ALL_IRQ);

    return 0;
}

void bitmap_dma_close(bitmap_dma_t *h)
{
    munmap(h->buf, h->buf_size);
    close(h->udmabuf_fd);
    if (h->sync_fd >= 0) close(h->sync_fd);
    axi_regs_close(&h->dma);
    axi_regs_close(&h->reader);
}

int bitmap_dma_transfer(bitmap_dma_t *h)
{
    // Trigger bitmap reader DMA
    axi_regs_write(&h->reader, BITMAP_READER_CTRL, CTRL_DMA_REQ);

    // // Wait for dma busy
    while (!(axi_regs_read(&h->reader, BITMAP_READER_STATUS) & STATUS_DMA_BUSY))
        ;

    // Arm S2MM by writting the buffer length, triggering the transfer
    axi_regs_write(&h->dma, S2MM_BUFF_LENGTH_REGISTER, h->buf_size);

    // Wait for S2MM to complete
    uint32_t status;
    do {
        status = axi_regs_read(&h->dma, S2MM_STATUS_REGISTER);
    } while (!(status & STATUS_IOC_IRQ) || !(status & STATUS_IDLE));


    // Wait for bitmap reader to confirm done
    while (!(axi_regs_read(&h->reader, BITMAP_READER_STATUS) & STATUS_DMA_DONE))
        ;

    // Rearm S2MM for next transfer
    axi_regs_write(&h->dma, S2MM_DST_ADDRESS_REGISTER, h->dst_addr);
    axi_regs_write(&h->dma, S2MM_CONTROL_REGISTER, RUN_DMA | ENABLE_ALL_IRQ);

    // Invalidate the CPU cache over the bitmap before it is read
    if (h->sync_fd >= 0 && write(h->sync_fd, "1", 1) < 0)
        perror("sync_for_cpu");

    return 0;
}