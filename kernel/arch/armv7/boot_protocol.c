/**
 * \file
 * \brief Implementation of the BF ARMv7 boot protocol.
 */

/*
 * Copyright (c) 2016, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Universitaetstr. 6, CH-8092 Zurich. Attn: Systems Group.
 */

#include <kernel.h>

#include <assert.h>
#include <boot_protocol.h>
#include <cp15.h>
#include <paging_kernel_arch.h>
#include <platform.h>
#include <startup_arch.h>

/*
 * \brief Boot an ARM APP core
 *
 * \param core_id   MPID of the core to try booting
 * \param entry     Entry address for new kernel in the destination
 *                  architecture's lvaddr_t
 *
 * \returns Zero on successful boot, non-zero (error code) on failure
 */
errval_t platform_boot_aps(hwid_t target, genpaddr_t gen_entry, genpaddr_t context)
{

    assert(paging_mmu_enabled());

    /* XXX - we're abusing the gen_entry pointer here.  Change the interface
     * to make this arch-specific. */
    lpaddr_t new_core_data_ptr= (lpaddr_t)gen_entry;
    printf("I\n");

    /* This mailbox is in the boot driver's BSS. */
    struct armv7_boot_record *br=
        (struct armv7_boot_record *)core_data->target_bootrecs;

    /* Acquire the lock on the boot record. */
    spinlock_acquire(&br->lock);
    printf("II\n");

    /* Pass the core data pointer. */
    br->core_data= new_core_data_ptr;

    /* Clear the completion notification. */
    br->done= 0;

    /* Flag which core should boot. */
    /* XXX - this will only work for single-cluster systems, whose MPID fits
     * entirely within the low 8 bits.  Make core IDs bigger! */
    br->target_mpid= target;

    /* The boot driver will read this value with its MMU and caches disabled,
     * so we need to make sure it's visible. */
    printf("III\n");
    dmb(); isb();
    printf("IV\n");
    clean_invalidate_to_poc(&br->core_data);
    printf("V\n");
    clean_invalidate_to_poc(&br->done);
    printf("VI\n");
    clean_invalidate_to_poc(&br->target_mpid);
    printf("VII\n");

    /* We need to ensure that the clean has finished before we wake them. */
    dmb(); isb();
    printf("VIII\n");

    /* Wake all sleeping cores. */
    sev();
    printf("IX\n");

    /* The target core will let us know that it's exited the boot driver by
     * setting done to one *with its MMU, and hence coherency, enabled*. */
    volatile uint32_t *mailbox= &br->done;
    while(!*mailbox) {
        wfe();
        printf("x");
    }
    printf("\n");

    /* Release the lock on the boot record. */
    spinlock_release(&br->lock);
    printf("X\n");

    return 0;
}

void
platform_notify_bsp(uint32_t *mailbox) {
    assert(paging_mmu_enabled());

    /* Set the flag to one (this is br->done, above). */
    *mailbox= 1;

    /* Make sure that the write has completed. */
    dmb(); isb();

    /* Wake the booting core. */
    sev();
}
