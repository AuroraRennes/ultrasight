/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KSIGHT_H
#define _KSIGHT_H

#include <linux/types.h>

/* -----------------------
 * Ksight objects
 * -----------------------
 *
 * Ksight monitors the information flows between different
 * types of objects in the kernel. To differenciate them, we
 * use this enumeration based on Laurent Georget's work.
 */

enum ksight_obj_type {
    KS_OBJ_NONE,
    KS_OBJ_FILE,
    KS_OBJ_PIPE,
    KS_OBJ_MEM,
    KS_OBJ_SHM,
    KS_OBJ_MQUEUE_SYSV,
    KS_OBJ_MQUEUE_POSIX,
    KS_OBJ_SOCKET,
};


struct ksight_object {
    uint32_t ksight_obj_type;
    uint32_t pid;        /* valid if OBJ_MEM */
	uint32_t tid;
    uint64_t id;         /* inode, pipe id, shmid, or base address */
    uint64_t size;       /* region size (bytes) */
};


struct ksight_tag_event {
	struct ksight_object src;
	struct ksight_object dst;
	uint64_t timestamp;
};

#endif /* _KSIGHT_H */