// trace_capture.h
#ifndef TRACE_CAPTURE_H
#define TRACE_CAPTURE_H

#include <stdint.h>
#include <stddef.h>
#include "axi_regs.h"

/*
 * PL-side trace capture: the TPIU frames out of frame_generator (synch words
 * stripped) are streamed by a dedicated S2MM AXI DMA into a u-dma-buf. The
 * buffer holds 16-byte aligned formatted frames, the layout an ETR writes, so
 * the dump is a drop-in cstrace.bin for a `format=coresight` OpenCSD snapshot.
 *
 * Sequence: trace_capture_arm() before the ETM is enabled,
 * trace_capture_finish() after the TPIU was flushed, then
 * trace_capture_export(). The capture is lossless iff frames_dropped is 0.
 */

// trace_capture AXI-Lite
#define TRACE_CAPTURE_MAP_SIZE      0x1000
#define TRACE_CAPTURE_CTRL          0x00  // pulse bits, see below
#define TRACE_CAPTURE_STATUS        0x04
#define TRACE_CAPTURE_CAPT_LO       0x08  // frames_captured, reading latches the high word
#define TRACE_CAPTURE_CAPT_HI       0x0C  // frames_captured, latched at the last LO read
#define TRACE_CAPTURE_DROPPED       0x10  // frames_dropped, 32-bit saturating

#define TRACE_CAPTURE_CTRL_START    (1 << 0)  // accept frames
#define TRACE_CAPTURE_CTRL_FLUSH    (1 << 1)  // stop, then emit the TLAST pad frame
#define TRACE_CAPTURE_CTRL_RESET    (1 << 2)  // clear counters and the drop flag

#define TRACE_CAPTURE_ST_ENABLED    (1 << 0)
#define TRACE_CAPTURE_ST_FLUSH_BUSY (1 << 1)  // pad not yet accepted downstream
#define TRACE_CAPTURE_ST_DROPPED    (1 << 2)  // sticky

#define TRACE_CAPTURE_FRAME_BYTES   16
// AXI DMA simple mode, c_sg_length_width = 26
#define TRACE_DMA_MAX_LENGTH        ((1u << 26) - 1)

#ifndef TRACE_CAPTURE_BASE
#pragma message("WARNING: TRACE_CAPTURE_BASE not defined, using placeholder - override for your Vivado project")
#define TRACE_CAPTURE_BASE 0x80023000
#endif

#ifndef TRACE_DMA_BASE
#pragma message("WARNING: TRACE_DMA_BASE not defined, using placeholder - override for your Vivado project")
#define TRACE_DMA_BASE 0x80030000
#endif

/* u-dma-buf backing the capture. In TPIU-only mode the ETR's buffer is idle,
 * so it is the default */
#ifndef UDMABUF_TRACE_NAME
#ifndef UDMABUF_ETR_NAME
#error "Neither UDMABUF_TRACE_NAME nor UDMABUF_ETR_NAME defined: pass -DUDMABUF_TRACE_NAME=\"<name>\""
#endif
#define UDMABUF_TRACE_NAME UDMABUF_ETR_NAME
#endif

#define TRACE_UDMABUF_ENV "ULTRASIGHT_UDMABUF_TRACE"

typedef struct {
    axi_regs_t      capture;
    axi_regs_t      dma;
    int             udmabuf_fd;
    unsigned long   dst_addr;
    void           *buf;
    size_t          buf_size;         // mapped u-dma-buf size
    size_t          length;           // S2MM transfer length armed, bytes
    /* Filled by trace_capture_finish() */
    size_t          trace_bytes;      // frames in the buffer, pad excluded
    uint64_t        frames_captured;
    uint32_t        frames_dropped;
    int             truncated;        // buffer filled before the flush
} trace_capture_t;

int  trace_capture_open(trace_capture_t *h);
void trace_capture_close(trace_capture_t *h);
int  trace_capture_arm(trace_capture_t *h);
int  trace_capture_finish(trace_capture_t *h);
int  trace_capture_export(const trace_capture_t *h, const char *path);

#endif
