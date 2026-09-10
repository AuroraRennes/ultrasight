// trace_capture.c
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // clock_gettime under -std=c11
#endif

#include "trace_capture.h"
#include "bitmap_dma.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define TRACE_CAPTURE_TIMEOUT_S 1.0

// AXI DMA S2MM status error bits: DMAIntErr, DMASlvErr, DMADecErr
#define STATUS_ERR_MASK             (0x7 << 4)

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Poll `regs[offset] & mask` until it equals `want`, bounded in time */
static int wait_bits(axi_regs_t *regs, uint32_t offset, uint32_t mask,
                     uint32_t want, const char *what)
{
    double deadline = now_s() + TRACE_CAPTURE_TIMEOUT_S;
    while ((axi_regs_read(regs, offset) & mask) != want) {
        if (now_s() > deadline) {
            fprintf(stderr, "[!] Trace capture: timed out waiting for %s\n", what);
            return -1;
        }
    }
    return 0;
}

int trace_capture_open(trace_capture_t *h)
{
    memset(h, 0, sizeof(*h));
    h->udmabuf_fd = -1;

    if (axi_regs_open(&h->capture, TRACE_CAPTURE_BASE, TRACE_CAPTURE_MAP_SIZE) < 0) {
        perror("axi_regs_open trace capture");
        return -1;
    }
    if (axi_regs_open(&h->dma, TRACE_DMA_BASE, DMA_MAP_SIZE) < 0) {
        perror("axi_regs_open trace DMA");
        axi_regs_close(&h->capture);
        return -1;
    }

    const char *udmabuf_name = getenv(TRACE_UDMABUF_ENV);
    if (!udmabuf_name || !*udmabuf_name) udmabuf_name = UDMABUF_TRACE_NAME;
    /* Tolerate a full device path as well as a bare name */
    const char *slash = strrchr(udmabuf_name, '/');
    if (slash) udmabuf_name = slash + 1;

    if (udmabuf_sysfs_info(udmabuf_name, &h->dst_addr, &h->buf_size) < 0)
        goto err_regs;

    /* Whole frames only, and within the DMA's length register */
    h->length = h->buf_size < TRACE_DMA_MAX_LENGTH ? h->buf_size : TRACE_DMA_MAX_LENGTH;
    h->length -= h->length % TRACE_CAPTURE_FRAME_BYTES;
    if (h->length < 2 * TRACE_CAPTURE_FRAME_BYTES) {
        fprintf(stderr, "[!] u-dma-buf '%s' too small for a trace capture\n", udmabuf_name);
        goto err_regs;
    }

    char udmabuf_dev[256];
    snprintf(udmabuf_dev, sizeof(udmabuf_dev), "/dev/%s", udmabuf_name);
    h->udmabuf_fd = open(udmabuf_dev, O_RDWR | O_SYNC);
    if (h->udmabuf_fd < 0) {
        perror("open trace udmabuf");
        goto err_regs;
    }
    h->buf = mmap(NULL, h->buf_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  h->udmabuf_fd, 0);
    if (h->buf == MAP_FAILED) {
        perror("mmap trace udmabuf");
        close(h->udmabuf_fd);
        goto err_regs;
    }
    return 0;

err_regs:
    axi_regs_close(&h->dma);
    axi_regs_close(&h->capture);
    return -1;
}

void trace_capture_close(trace_capture_t *h)
{
    munmap(h->buf, h->buf_size);
    close(h->udmabuf_fd);
    axi_regs_close(&h->dma);
    axi_regs_close(&h->capture);
}

/* Arm the S2MM transfer, then start accepting frames. Call before the ETM is
 * enabled, the TPIU does not wait for anyone */
int trace_capture_arm(trace_capture_t *h)
{
    axi_regs_write(&h->dma, S2MM_CONTROL_REGISTER, RESET_DMA);
    if (wait_bits(&h->dma, S2MM_CONTROL_REGISTER, RESET_DMA, 0, "DMA reset") < 0)
        return -1;
    axi_regs_write(&h->dma, S2MM_CONTROL_REGISTER, ENABLE_ALL_IRQ);
    axi_regs_write(&h->dma, S2MM_DST_ADDRESS_REGISTER, h->dst_addr);
    axi_regs_write(&h->dma, S2MM_CONTROL_REGISTER, RUN_DMA | ENABLE_ALL_IRQ);
    // Writing the length starts the transfer
    axi_regs_write(&h->dma, S2MM_BUFF_LENGTH_REGISTER, h->length);

    axi_regs_write(&h->capture, TRACE_CAPTURE_CTRL,
                   TRACE_CAPTURE_CTRL_RESET | TRACE_CAPTURE_CTRL_START);
    return 0;
}

/* Stop the capture, close the DMA transfer with the TLAST pad and account for
 * every frame. Call after the TPIU was flushed, so the tail is in */
int trace_capture_finish(trace_capture_t *h)
{
    axi_regs_write(&h->capture, TRACE_CAPTURE_CTRL, TRACE_CAPTURE_CTRL_FLUSH);

    uint32_t lo = axi_regs_read(&h->capture, TRACE_CAPTURE_CAPT_LO);
    uint32_t hi = axi_regs_read(&h->capture, TRACE_CAPTURE_CAPT_HI);
    h->frames_captured = ((uint64_t)hi << 32) | lo;
    h->frames_dropped  = axi_regs_read(&h->capture, TRACE_CAPTURE_DROPPED);

    uint64_t expected = (h->frames_captured + 1) * TRACE_CAPTURE_FRAME_BYTES;
    h->truncated = expected > h->length;

    if (h->truncated) {
        /* The DMA stopped at h->length without seeing TLAST, the rest of the
         * trace (and the pad) is stuck upstream. Reset clears it */
        wait_bits(&h->dma, S2MM_STATUS_REGISTER, STATUS_IDLE, STATUS_IDLE, "DMA idle");
        fprintf(stderr, "[!] Trace capture: buffer full, trace truncated to %zu bytes "
                "(%" PRIu64 " frames captured)\n", h->length, h->frames_captured);
        h->trace_bytes = h->length;
        axi_regs_write(&h->dma, S2MM_CONTROL_REGISTER, RESET_DMA);
        return 0;
    }

    if (wait_bits(&h->capture, TRACE_CAPTURE_STATUS, TRACE_CAPTURE_ST_FLUSH_BUSY, 0,
                  "the flush pad (is the trace DMA armed?)") < 0)
        return -1;
    if (wait_bits(&h->dma, S2MM_STATUS_REGISTER, STATUS_IDLE, STATUS_IDLE,
                  "the trace DMA to complete") < 0)
        return -1;

    uint32_t status = axi_regs_read(&h->dma, S2MM_STATUS_REGISTER);
    if (status & STATUS_ERR_MASK) {
        fprintf(stderr, "[!] Trace capture: DMA error, S2MM status 0x%08x\n", status);
        return -1;
    }

    /* On completion the length register holds the bytes actually written */
    uint32_t written = axi_regs_read(&h->dma, S2MM_BUFF_LENGTH_REGISTER);
    if (written != expected) {
        fprintf(stderr, "[!] Trace capture: DMA wrote %u bytes, %" PRIu64
                " frames + pad is %" PRIu64 "\n", written, h->frames_captured, expected);
        return -1;
    }
    h->trace_bytes = written - TRACE_CAPTURE_FRAME_BYTES;

    if (h->frames_dropped)
        fprintf(stderr, "[!] Trace capture: %u frames dropped, the trace has gaps\n",
                h->frames_dropped);
    return 0;
}

/* Write the captured frames, pad excluded, e.g. as a snapshot's cstrace.bin */
int trace_capture_export(const trace_capture_t *h, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror("fopen trace capture export");
        return -1;
    }
    size_t n = fwrite(h->buf, 1, h->trace_bytes, f);
    if (fclose(f) != 0 || n != h->trace_bytes) {
        fprintf(stderr, "[!] Trace capture: short write to %s\n", path);
        return -1;
    }
    return 0;
}
