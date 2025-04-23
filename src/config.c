/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2021 Ricerca Security, Inc. All rights reserved. */

#include "config.h"

#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <limits.h>

#include "csregistration.h"
#include "csregisters.h"

#define SHOW_ETM_CONFIG 0

const bool return_stack = false;
const bool branch_broadcast = false;
const bool cycle_count = false;

extern unsigned long etr_ram_addr;
extern size_t etr_ram_size;
extern int registration_verbose;

/**
* Set the ETB to manual flush and wait for the end of the trace.
*/
void cs_etb_flush_and_wait_stop(struct cs_devices_t *devices)
{
  unsigned int ffcr_val, status_val;
  if (cs_sink_is_enabled(devices->etb)) {
    /* Read the value of the Flush and Format Control Register. */
    ffcr_val = cs_device_read(devices->etb, CS_ETB_FLFMT_CTRL);
    /* Set the manual flush bit and write it back */
    ffcr_val |= CS_ETB_FLFMT_CTRL_FOnMan;
    cs_device_write(devices->etb, CS_ETB_FLFMT_CTRL, ffcr_val);
    /* Wait for the ETB to stop collecting data, i.e. waiting for the FtEmpty
       bit to be set in CS_ETB_STATUS */
    if (cs_device_wait(devices->etb, CS_ETB_STATUS, CS_ETB_STATUS_FtEmpty,
                       CS_REG_WAITBITS_ALL_1, 0, &status_val) != 0) {
      fprintf(stderr,
              "ETB collection not stopped on flush on trigger. STS: 0x%08x\n",
              status_val);
    }
  } else {
    fprintf(stderr, "ETB is not activated, flush cancelled.\n");
  }
}

/**
* Define the address ranges of the ETMv4 by configuring the address comparators.
*/
static void set_etmv4_addr_range(struct map_info *range,
                                 struct _adrcmp *addr_comp,
                                 unsigned int acc_type_ex)
{
  /*  Excluded access type: all exception secure/not secure (0, 1, 2) and a
   * user-defined excluded access type. */
  const unsigned int acc_type =
      CS_ETMV4_ACATR_ExEL0_S | CS_ETMV4_ACATR_ExEL1_S | CS_ETMV4_ACATR_ExEL2_S |
      CS_ETMV4_ACATR_ExEL1_NS | CS_ETMV4_ACATR_ExEL2_NS | acc_type_ex;

  /* NULL check for both */
  if (!range || !addr_comp) {
    fprintf(stderr, "No range or address comparator, ETMv4 address not set.");
    return;
  }

  /* Address comparator setup (0 for start address, 1 for end address):
    - acvr_l, address comparator value (low)
    - acvr_h,               -         (high)
    - acatr_l, address comparator type (low)  */
  addr_comp[0].acvr_l = range->start & 0xFFFFFFFF;
  addr_comp[0].acvr_h = (range->start >> 32) & 0xFFFFFFFF;
  addr_comp[0].acatr_l = acc_type;
  addr_comp[1].acvr_l = range->end & 0xFFFFFFFF;
  addr_comp[1].acvr_h = (range->end >> 32) & 0xFFFFFFFF;
  addr_comp[1].acatr_l = acc_type;
}

/**
* Configure the ETMv4 registers
*/
static int configure_etmv4_addr_range_cid(cs_device_t etm,
                                          struct map_info *range,
                                          int range_count, unsigned long cid)
{
  cs_etmv4_config_t tconfig;
  int error_count;
  size_t cididx;
  size_t addridx;

  /* Initialize an ETM configuration structure */
  cs_etm_config_init_ex(etm, &tconfig);
  /* These flags a*/
  tconfig.flags = CS_ETMC_TRACE_ENABLE | CS_ETMC_CONFIG | CS_ETMC_EVENTSELECT;
  /* Read the configuration from the ETM device hardware. */
  cs_etm_config_get_ex(etm, &tconfig);

  /* Disable VMID (virtual context id) */
  if (tconfig.scv4->idr2.bits.vmidsize > 0) {
    /* VMID trace must be disabled to use context ID trace only. */
    tconfig.configr.bits.vmid = 0;
  }
  /* Setup CID (context id) in the static configuration */
  if (tconfig.scv4->idr2.bits.cidsize > 0 && cid > 0) {
    tconfig.configr.bits.cid = 1; /* Enable */
  } else {
    tconfig.configr.bits.cid = 0; /* Disable */
  }

  /** Configure specific features (if supported by the device):
  *    - return stack: enable a return stack of addresses for faster tracing
  *    - branch broadcast: add address packets before indirect jumps
  *    - cycle count: add cycle counting packets
  */
  if (return_stack) tconfig.configr.bits.rs = 1; /* set the return stack */
  if (branch_broadcast)
    tconfig.configr.bits.bb = 1;                 /* set the branch broadcast */
  if (cycle_count) tconfig.configr.bits.cci = 1; /* set the cycle count */

  /* If a context id is specified, set up the comparator */
  if (cid > 0) {
    cididx = 0;
    /* Low/high part of the comparator id */
    tconfig.cxid_comps[cididx].cidcvr_l = cid & 0xFFFFFFFF;
    tconfig.cxid_comps[cididx].cidcvr_h = (cid >> 32) & 0xFFFFFFFF;
    /* Enable control and setup a mask */
    tconfig.cidcctlr0 &= ~(1 << cididx);
    tconfig.cxid_comps_acc_mask |= (1 << cididx);
    /* Activate the comparator id in the main flags */
    tconfig.flags |= CS_ETMC_CXID_COMP;
  }

  /* Set up address range filtering */
  addridx = 0;
  /* Note: Assumes range[0] is the tracee itself. */
  /* Set the address range for the tracee program */
  set_etmv4_addr_range(&range[0], &tconfig.addr_comps[addridx],
                       cid > 0 ? (cididx << 4) | (0x1 << 2) : 0);
  /* Activates the address comparator mask */
  tconfig.addr_comps_acc_mask |= 0x3 << addridx;
  tconfig.viiectlr |= 1 << (addridx / 2);

  tconfig.flags |= CS_ETMC_ADDR_COMP;

  /* Mark the configuration ready to be written back into the above registers on 'put' */
  cs_etm_config_put_ex(etm, &tconfig);

  /* If needed, show the resulting configuration */
  if (registration_verbose > 0) {
    show_etm_config(etm);
  }

  /* Check for errors */
  error_count = cs_error_count();
  if (error_count > 0) {
    fprintf(stderr, "%u errors reported when configuring ETM\n", error_count);
    return -1;
  }

  return 0;
}

/**
* Printing the ETM config
*/
void show_etm_config(cs_device_t etm)
{
  /* ETMv4 config type */
  assert(cs_device_has_class(etm, CS_DEVCLASS_SOURCE));
  cs_etmv4_config_t *t4config;
  t4config = NULL;

  /* Check the ETM version, bail out if not v4 */
  if (CS_ETMVERSION_MAJOR(cs_etm_get_version(etm)) < CS_ETMVERSION_ETMv4)
    return;

  /* Initialize the ETMv4 config structure */
  cs_etm_config_init_ex(etm, t4config);
  /* Activate all flags */
  t4config->flags = CS_ETMC_ALL;
  /* Get and print the config */
  cs_etm_config_get_ex(etm, t4config);
  cs_etm_config_print_ex(etm, t4config);
}

