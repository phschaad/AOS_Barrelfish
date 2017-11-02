/**
 * \file
 * \brief Slot allocator wrapper
 *
 * Warning: slot_alloc_init calls vregion_map which calls vspace_add_vregion.
 * vspace_add_vregion uses malloc to increase it's slab.
 * Since malloc depends upon slot_alloc_init being called successfully,
 * vspace_add_vregion should have enough initial slab space to not use malloc.
 */

/*
 * Copyright (c) 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <aos/aos.h>
#include <aos/core_state.h>
#include <aos/caddr.h>
#include "internal.h"


/**
 * \brief Returns the default slot allocator for the caller
 */
struct slot_allocator *get_default_slot_allocator(void)
{
    struct slot_alloc_state *state = get_slot_alloc_state();
    return (struct slot_allocator*)(&state->defca);
}

/**
 * \brief Default slot allocator
 *
 * \param ret Pointer to the cap to return the allocated slot in
 *
 * Allocates one slot from the default allocator
 */
static int slot_alloc_rec_depth = 0;

static int usedslots = 31;
static struct capref capbuffer[5];
static bool refilling = false;

static void sloc_alloc_refill_preallocated_with_cap(struct capref *cap) {
    for(int i = 0; i < 5; i++) {
        if(usedslots & (1 << i)) {
            capbuffer[i] = *cap;
            usedslots -= (1 << i);
            return;
        }
    }
    DBG(ERR,"we tried to hand a slot back to the prefilled buffer despite that one being full. This is bad as we just lost a cap into the infinite void\n");
}

static void sloc_alloc_refill_preallocated_slots(void) {
    refilling = true;
    debug_printf("we are refilling slots\n");
    for(int i = 0; i < 5; i++) {
        if(usedslots & (1 << i)) {
            if(slot_alloc(&capbuffer[i]) != SYS_ERR_OK) {
                DBG(ERR,
                    "we failed to refill in lmp_chan_recv_slot_refill_preallocated_slots, things are going to be bad\n");
            } else
                usedslots -= (1 << i);
        }
    }
    refilling = false;
}

static void slot_alloc_use_prefilled_slot(struct capref *slot) {
    assert(!refilling);
    for(int i = 0; i < 5; i++) {
        if(usedslots & (1 << i))
            continue;
        *slot = capbuffer[i];
        usedslots = usedslots | (1 << i);
        return;
    }
    assert(!"lmp_chan_alloc_recv_slot_use_prefilled_slot ran out of buffers. Something is really wrong\n");

}
errval_t slot_alloc(struct capref *ret)
{
    //this is inherently not threadsafe. But hey, problems for later.
    errval_t err;
    if(get_slot_alloc_rec_depth() == 0) {
        slot_alloc_rec_depth++;
        struct slot_allocator *ca = get_default_slot_allocator();
        err = ca->alloc(ca, ret);
        slot_alloc_rec_depth--;
    }else{
        debug_printf("we get into this case!\n");
        slot_alloc_use_prefilled_slot(ret);
        debug_printf("and out again\n");
        err = SYS_ERR_OK;
    }
    if(usedslots != 0 && !refilling && get_slot_alloc_rec_depth() == 0)
        sloc_alloc_refill_preallocated_slots();

    return err;
}

int get_slot_alloc_rec_depth(void) {
    return slot_alloc_rec_depth;
}

/**
 * \brief slot allocator for the root
 *
 * \param ret Pointer to the cap to return the allocated slot in
 *
 * Allocates one slot from the root slot allocator
 */
errval_t slot_alloc_root(struct capref *ret)
{
    struct slot_alloc_state *state = get_slot_alloc_state();
    struct slot_allocator *ca = (struct slot_allocator*)(&state->rootca);
    return ca->alloc(ca, ret);
}

errval_t root_slot_allocator_refill(cn_ram_alloc_func_t myalloc, void *allocst)
{
    errval_t err;

    struct slot_alloc_state *state = get_slot_alloc_state();
    struct single_slot_allocator *sca = &state->rootca;

    cslot_t nslots = sca->a.nslots;
    assert(nslots >= L2_CNODE_SLOTS);

    // Double size of root cnode
    struct capref root_ram, newroot_cap;
    err = myalloc(allocst, nslots * 2 * OBJSIZE_CTE, &root_ram);
    if (err_is_fail(err)) {
        return err_push(err, MM_ERR_SLOT_MM_ALLOC);
    }
    err = slot_alloc(&newroot_cap);
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_SLOT_ALLOC);
    }
    err = cnode_create_from_mem(newroot_cap, root_ram, ObjType_L1CNode,
            NULL, nslots * 2);
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_CNODE_CREATE_FROM_MEM);
    }
    // Delete RAM cap of new CNode
    err = cap_delete(root_ram);
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_CAP_DELETE);
    }

    // Resize rootcn
    err = root_cnode_resize(newroot_cap, root_ram);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "resizing root cnode");
        return err;
    }

    // Delete old Root CNode and free slot
    err = cap_destroy(root_ram);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "deleting old root cnode");
        return err_push(err, LIB_ERR_CAP_DESTROY);
    }

    // update root slot allocator size and our metadata
    return single_slot_alloc_resize(sca, nslots * 2);

    return SYS_ERR_OK;
}

/**
 * \brief Default slot free
 *
 * \param ret The cap to free
 *
 * Frees the passed in slot.
 *
 * \bug During dispatcher initialization and special domains like
 * init and mem_serv free slots which
 * are not allocated by the default allocator.
 * This function detects such cases and ignores the errors.
 * It maybe ignoring errors that must be caught.
 */
errval_t slot_free(struct capref ret)
{
    struct slot_alloc_state *state = get_slot_alloc_state();

    if (cnodecmp(ret.cnode, cnode_base)) { // Detect frees in basecn
        return SYS_ERR_OK;
    }

    if (cnodecmp(ret.cnode, cnode_root)) {
        struct slot_allocator *ca = (struct slot_allocator*)(&state->rootca);
        return ca->free(ca, ret);
    }

    struct slot_allocator *ca = (struct slot_allocator*)(&state->defca);
    errval_t err;
    if(slot_alloc_rec_depth == 0)
        err = ca->free(ca, ret);
    else {
        sloc_alloc_refill_preallocated_with_cap(&ret);
        err = SYS_ERR_OK;
    }
    // XXX: Detect frees in special case of init and mem_serv
    if (err_no(err) == LIB_ERR_SLOT_ALLOC_WRONG_CNODE) {
        return SYS_ERR_OK;
    }
    return err;
}

errval_t slot_alloc_init(void)
{
    errval_t err;

    struct slot_alloc_state *state = get_slot_alloc_state();

    /* Default allocator */
    // While initializing, other domains will call into it. Be careful
    struct capref cap;
    struct cnoderef cnode;
    struct multi_slot_allocator *def = &state->defca;

    // Generic
    thread_mutex_init(&def->a.mutex);

    def->a.alloc = two_level_alloc;
    def->a.free  = two_level_free;
    def->a.space = SLOT_ALLOC_CNODE_SLOTS;
    def->a.nslots = SLOT_ALLOC_CNODE_SLOTS;

    def->head = &state->head;
    def->head->next = NULL;
    def->reserve = &state->reserve;
    def->reserve->next = NULL;

    // Head
    cap.cnode = cnode_root;
    cap.slot  = ROOTCN_SLOT_SLOT_ALLOC1;
    cnode = build_cnoderef(cap, CNODE_TYPE_OTHER);
    err = single_slot_alloc_init_raw(&def->head->a, cap, cnode,
                                     SLOT_ALLOC_CNODE_SLOTS, state->head_buf,
                                     sizeof(state->head_buf));
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_SINGLE_SLOT_ALLOC_INIT_RAW);
    }

    // Reserve
    cap.cnode = cnode_root;
    cap.slot  = ROOTCN_SLOT_SLOT_ALLOC2;
    cnode = build_cnoderef(cap, CNODE_TYPE_OTHER);
    err = single_slot_alloc_init_raw(&def->reserve->a, cap, cnode,
                                     SLOT_ALLOC_CNODE_SLOTS, state->reserve_buf,
                                     sizeof(state->reserve_buf));
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_SINGLE_SLOT_ALLOC_INIT_RAW);
    }

    // Slab
    size_t allocation_unit = sizeof(struct slot_allocator_list) +
                             SINGLE_SLOT_ALLOC_BUFLEN(SLOT_ALLOC_CNODE_SLOTS);
    slab_init(&def->slab, allocation_unit, NULL);

    // Vspace mgmt
    // Warning: necessary to do this in the end as during initialization,
    // libraries can call into slot_alloc.
    // XXX: this should be resizable
    err = paging_region_init(get_current_paging_state(), &def->region,
                             allocation_unit * SLOT_ALLOC_CNODE_SLOTS * 20);
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_VSPACE_MMU_AWARE_INIT);
    }

    /* Root allocator */
    err = single_slot_alloc_init_raw(&state->rootca, cap_root, cnode_root,
                                     L2_CNODE_SLOTS, state->root_buf,
                                     sizeof(state->root_buf));
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_SINGLE_SLOT_ALLOC_INIT_RAW);
    }
    state->rootca.a.space     = L2_CNODE_SLOTS - ROOTCN_FREE_SLOTS;
    state->rootca.head->space = L2_CNODE_SLOTS - ROOTCN_FREE_SLOTS;
    state->rootca.head->slot  = ROOTCN_FREE_SLOTS;

    return SYS_ERR_OK;
}
