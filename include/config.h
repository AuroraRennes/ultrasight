#ifndef CS_TRACE_CONFIG_H
#define CS_TRACE_CONFIG_H

#include <sys/types.h>

#include "csregistration.h"

#include "utils.h"

/* Where the tracee's address range is enforced:
 *  - PL:   the ETM traces all of EL0 for the tracee's CID, and the decoder and
 *          edge_extractor drop what lies outside the tracee text. Filtering in
 *          the ETM turns every call into an untraced library into a TRACE_ON
 *          burst, which overflows its FIFO
 *  - ETM:  the ETM's address comparators only trace the tracee text
 *  - NONE: all of EL0 for the tracee's CID, no filter anywhere */
typedef enum { ADDR_FILTER_PL, ADDR_FILTER_ETM, ADDR_FILTER_NONE } addr_filter_t;
extern addr_filter_t addr_filter;

/* Where branch broadcast applies when it is on (TRCBBCTLR over the tracee
 * text, on the second address comparator pair):
 *  - ALL: everywhere the ETM traces
 *  - OUT: outside the tracee text only (libraries), atoms inside it
 *  - IN:  inside the tracee text only, atoms in the libraries */
typedef enum { BB_FILTER_ALL, BB_FILTER_OUT, BB_FILTER_IN } bb_filter_t;
extern bb_filter_t bb_filter;

void cs_etb_flush_and_wait_stop(struct cs_devices_t *devices);
void cs_tpiu_flush_and_wait_stop(struct cs_devices_t *devices);
int init_etm(cs_device_t dev);
void show_etm_config(cs_device_t etm);
int configure_trace(const struct board *board, struct cs_devices_t *devices,
                    struct map_info *range, int range_count, pid_t pid);
int enable_trace(const struct board *board, struct cs_devices_t *devices);
int disable_trace(const struct board *board, struct cs_devices_t *devices);
int enable_trace_sinks_only(const struct board *board, struct cs_devices_t *devices);
int disable_trace_sinks_only(struct cs_devices_t *devices);
int reconfigure_cid(const struct board *board, struct cs_devices_t *devices, pid_t pid);
int set_etm_bb_mode(const struct board *board, struct cs_devices_t *devices, int bb_mode);

#endif /* CS_TRACE_CONFIG_H */