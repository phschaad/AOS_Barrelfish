/**
 * \file
 * \brief Implementation of AOS rpc-like messaging
 */

/*
 * Copyright (c) 2013 - 2016, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Universitaetstr. 6, CH-8092 Zurich. Attn: Systems Group.
 */

#include <aos/aos_rpc.h>

//#undef DEBUG_LEVEL
//#define DEBUG_LEVEL 100

#define send_handler_core(name, asserts, sendline)                            \
    static errval_t name(uintptr_t *args)                                     \
    {                                                                         \
        DBG(VERBOSE, #name"\n")                                               \
        asserts;                                                              \
        struct aos_rpc *rpc = (struct aos_rpc *) args[0];                     \
        errval_t err;                                                         \
        err = sendline;                                                       \
        if (err_is_fail(err)) {                                               \
            /* Reregister if failed. */                                       \
            CHECK(lmp_chan_register_send(&rpc->chan, get_default_waitset(),   \
                                         MKCLOSURE((void *) putchar_send_handler,\
                                                   args)));                   \
        }                                                                     \
        return SYS_ERR_OK;                                                    \
    }

#define send_handler_0(name, type)                                            \
    send_handler_core(name, , lmp_chan_send1(&rpc->chan, LMP_FLAG_SYNC,       \
                                           rpc->chan. local_cap, type));
#define send_handler_1(name, type, arg)                                       \
    send_handler_core(name, , lmp_chan_send2(&rpc->chan, LMP_FLAG_SYNC,       \
                                             rpc->chan.local_cap, type, arg));
#define send_handler_8(name, type, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8) \
    send_handler_core(name, , lmp_chan_send9(&rpc->chan, LMP_FLAG_SYNC,       \
                                             rpc->chan.local_cap, type, arg1, \
                                             arg2, arg3, arg4, arg5, arg6,    \
                                             arg7,arg8));

#define impl(send_handler, receive_handler, recvslot)                         \
    thread_mutex_lock(&chan->mutex);                                          \
    if (recvslot == true) CHECK(lmp_chan_alloc_recv_slot(&chan->chan));       \
    CHECK(lmp_chan_register_send(&chan->chan, ws, MKCLOSURE((void *) send_handler, \
                                                            sendargs)));      \
    CHECK(lmp_chan_register_recv(&chan->chan, ws, MKCLOSURE((void *) receive_handler, \
                                                            sendargs)));      \
    CHECK(event_dispatch(ws));                                                \
    CHECK(event_dispatch(ws));                                                \
    thread_mutex_unlock(&chan->mutex);

#define recv_handler_fleshy_bits(name,capindex,type)                          \
    DBG(VERBOSE,#name"\n");                                                   \
    struct aos_rpc *rpc = (struct aos_rpc *) args[0];                         \
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;                              \
    errval_t err;                                                             \
    if (capindex != -1) {                                                     \
        struct capref *retcap = (struct capref *) args[capindex];             \
        err = lmp_chan_recv(&rpc->chan, &msg, retcap);                        \
    } else {                                                                  \
        err = lmp_chan_recv(&rpc->chan, &msg, NULL);                          \
    }                                                                         \
    if (err_is_fail(err) && lmp_err_is_transient(err)) {                      \
        DBG(DETAILED,"rereg "#name"\n");                                      \
        CHECK(lmp_chan_register_recv(&rpc->chan, get_default_waitset(),       \
                                     MKCLOSURE((void *) name, args)));        \
        return err;                                                           \
    }                                                                         \
    if (err_is_fail(err)) {                                                   \
        DBG(ERR, #name"'s call to lmp_chan_recv failed non-transiently\n");   \
        return err;                                                           \
    }                                                                         \
    if (msg.words[0] != RPC_ACK_MESSAGE(type)) {                              \
        DBG(ERR, "This is bad, we got msg type (raw, aka shifted to the left): %p\n", \
            msg.words[0]);                                                    \
        /* TODO: handle? */                                                   \
    }

__attribute__((unused))
static errval_t ram_receive_handler(void *args)
{
    uintptr_t *uargs = (uintptr_t *) args;
    struct aos_rpc *rpc = (struct aos_rpc *) uargs[0];
    struct capref *retcap = (struct capref *) uargs[3];
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;

    errval_t err = lmp_chan_recv(&rpc->chan, &msg, retcap);
    // Re-register if failed.
    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        DBG(DETAILED,"rereg ram_receive_handler\n");
        CHECK(lmp_chan_register_recv(&rpc->chan, get_default_waitset(),
                                     MKCLOSURE((void *) ram_receive_handler,
                                               args)));
        return err;
    }
    if (err_is_fail(err)) {
        DBG(ERR, "ram_receive_handler's call to lmp_chan_recv failed non-transiently\n");
        return err;
    }

    assert(msg.buf.msglen >= 2);

    if (msg.words[0] != RPC_ACK_MESSAGE(RPC_TYPE_RAM)) {
        DBG(ERR, "This is bad, we got msg type (raw, aka shifted to the left): %p\n",
            msg.words[0]);
        // TODO: handle?
    }

    if (retcap->cnode.cnode == NULL_CAP.cnode.cnode &&
            retcap->cnode.level == NULL_CAP.cnode.level &&
            retcap->cnode.croot == NULL_CAP.cnode.croot &&
            retcap->slot == NULL_CAP.slot) {
        DBG(ERR, "Got null cap back :(\n");
    }

    size_t *retsize = (size_t *) uargs[4];
    *retsize = msg.words[1];

    return SYS_ERR_OK;
}
/*
static errval_t rpc_receive_handler(void *args)
{
    DBG(DETAILED, "rpc_receive_handler\n");

    struct aos_rpc *rpc = (struct aos_rpc *) args;
    // Get the message from the child.
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    struct capref child_cap;
    errval_t err = lmp_chan_recv(&rpc->chan, &msg, &child_cap);
    if (err_is_fail(err)) {
        return err;
    }
    // do actions depending on the message type
    // Check the message type and handle it accordingly.
    switch (msg.words[0]) {
        case RPC_ACK_MESSAGE(RPC_TYPE_HANDSHAKE):
            DBG(DETAILED, "ACK Received (handshake) \n");
            rpc->init = true;
            break;
        case RPC_ACK_MESSAGE(RPC_TYPE_PUTCHAR):
            DBG(DETAILED, "ACK Received (putchar) \n");
            break;
        case RPC_ACK_MESSAGE(RPC_TYPE_RAM):
            DBG(WARN, "RAM RPC received, but with standard handler\n"
                "This is not intended. Expect badness.\n");
            // TODO: Handle?
            break;
        case RPC_ACK_MESSAGE(RPC_TYPE_NUMBER):
            DBG(DETAILED, "ACK Received (number)\n");
            break;
        default:
            debug_printf("got message type %d", msg.words[0]);
            printf("%zu\n", msg.words[0]);
            DBG(WARN, "Unable to handle RPC-receipt, expect badness!\n");
            assert(!"NOT IMPLEMENTED");
    }

    return SYS_ERR_OK;

}
*/

errval_t aos_rpc_send_number(struct aos_rpc *chan, uintptr_t val)
{
    DBG(VERBOSE, "rpc_send_number\n");

    uintptr_t *sendargs =malloc(1* sizeof(uintptr_t));

    sendargs[0] = (uintptr_t) val;
    int unused_id;
    CHECK(send(&chan->chan,NULL_CAP,RPC_MESSAGE(RPC_TYPE_NUMBER),1,sendargs,MKCLOSURE(free,sendargs),&unused_id));

    return SYS_ERR_OK;
}

static errval_t string_send_handler(uintptr_t *args)
{
    DBG(DETAILED, "string_send_handler (%d)\n", args[8]);

    struct aos_rpc *rpc = (struct aos_rpc *) args[0];

    errval_t err;
    err = lmp_chan_send9(&rpc->chan, LMP_FLAG_SYNC, NULL_CAP,
                         RPC_MESSAGE((uint32_t) args[8]), rpc->id, args[1],
                         args[2], args[3], args[4], args[5], args[6], args[7]);
    if (err_is_fail(err)){
        // Reregister if failed.
        CHECK(lmp_chan_register_send(&rpc->chan, get_default_waitset(),
                                     MKCLOSURE((void *) string_send_handler,
                                               args)));
        CHECK(event_dispatch(get_default_waitset()));
    }

    return SYS_ERR_OK;
}

errval_t aos_rpc_send_string(struct aos_rpc *rpc, const char *string)
{
    int unused_id;
    return persist_send_cleanup_wrapper(&rpc->chan,NULL_CAP,RPC_MESSAGE(RPC_TYPE_STRING),strlen(string),(void*)string,NULL_EVENT_CLOSURE,&unused_id);

    thread_mutex_lock(&rpc->mutex);
    assert(rpc != NULL);
    // We probably have to split the string into smaller pieces.
    // Reminder: string is null terminated ('\0')
    // We can send a max of 9 messages where we need one to identify the 
    // message type so we can send 8 32 bit/4 char chunks at once.

    struct waitset *ws = get_default_waitset();

    uint32_t args[9];
    args[0] = (uint32_t) rpc;
    args[8] = (uint32_t) RPC_TYPE_STRING;

    // Get the length of the string (including the terminating 0).
    uint32_t len = strlen(string) + 1;

    uint8_t count = 0; // We init with 0 to be able to add 1 at the beginning.

    // We send the size of the string in the first message.
    args[++count] = len;
    for (uint32_t i=0; i<=len;++i){
        // We go through the array and pack 4 of them together.
        if (i%4==0){
            // Increase count.
            // The first of 4 should reset the value.
            args[++count] = string[i];
        } else {
            // Shift and add the other 3 values.
            args[count] += string[i] << ((i%4)*8);
        }
        // Send if full.
        if (count == 7 && i%4==3){
            errval_t err;
            do {
                // Check if sender is currently busy.
                err = lmp_chan_register_send(&rpc->chan, ws,
                                 MKCLOSURE((void *) string_send_handler,
                                           args));
                CHECK(event_dispatch(ws));
            } while (err == LIB_ERR_CHAN_ALREADY_REGISTERED);
            args[8] = (uint32_t) RPC_TYPE_STRING_DATA;

            // Reset counter.
            count = 0;
        }
    }

    // Send the final chunk if needed.
    if (len%32 != 0){
        errval_t err;
        do {
            // Check if sender is currently busy.
            err = lmp_chan_register_send(&rpc->chan, ws,
                             MKCLOSURE((void *) string_send_handler,
                                       args));
            CHECK(event_dispatch(ws));
        } while (err == LIB_ERR_CHAN_ALREADY_REGISTERED);
    }
    thread_mutex_unlock(&rpc->mutex);

    return SYS_ERR_OK;
}

static errval_t ram_send_handler(uintptr_t *args)
{
    DBG(DETAILED, "rpc_ram_send_handler\n");

    struct aos_rpc *rpc = (struct aos_rpc *) args[0];
    size_t size = *((size_t *) args[1]);
    size_t align = *((size_t *) args[2]);

    DBG(DETAILED, "The size is %zu and alignment is %zu\n", size, align);
    //dirty hacks
    unsigned int first_byte = (RPC_MESSAGE(RPC_TYPE_RAM) << 24) + (0 << 16) + 2;
    //end dirty hacks
    errval_t err;
    do {
        DBG(DETAILED, "calling lmp_chan_send3 in rpc_ram_send_handler\n");
        // Check if sender is currently busy
        // TODO: could we implement some kind of buffer for this?
        err = lmp_chan_send3(&rpc->chan, LMP_FLAG_SYNC, NULL_CAP,
                             first_byte, size, align);
    } while (err == LIB_ERR_CHAN_ALREADY_REGISTERED);
    if(!err_is_ok(err))
        debug_printf("tried to send ram request, ran into issue: %s\n",err_getstring(err));

    return SYS_ERR_OK;
}
//ugly hack until we have a proper system for associating sends with recvs
static struct capref ram_cap;
static size_t ram_size;
//end ugly hack until we have a proper system for associating sends with recvs

errval_t aos_rpc_get_ram_cap(struct aos_rpc *chan, size_t size, size_t align,
                             struct capref *retcap, size_t *ret_size)
{
    thread_mutex_lock_nested(&chan->mutex);
    DBG(-1, "rpc_get_ram_cap\n");

    struct waitset *ws = get_default_waitset();

    uintptr_t sendargs[5];
    DBG(VERBOSE, "rpc_get_ram_cap 2\n");

    sendargs[0] = (uintptr_t) chan;
    sendargs[1] = (uintptr_t) &size;
    sendargs[2] = (uintptr_t) &align;
    sendargs[3] = (uintptr_t) retcap;
    sendargs[4] = (uintptr_t) ret_size;
    DBG(VERBOSE, "rpc_get_ram_cap 3\n");

//    CHECK(lmp_chan_alloc_recv_slot(&chan->chan));
    DBG(-1, "rpc_get_ram_cap 4\n");
    ram_size = 0;

    errval_t err = lmp_chan_register_send(&chan->chan, ws,
                                 MKCLOSURE((void *) ram_send_handler,
                                           sendargs));
    while(err == LIB_ERR_CHAN_ALREADY_REGISTERED) {
        CHECK(event_dispatch(ws));
        err = lmp_chan_register_send(&chan->chan, ws,
                                     MKCLOSURE((void *) ram_send_handler,
                                               sendargs));
    }
    DBG(-1, "rpc_get_ram_cap 5\n");
    CHECK(err);


    DBG(-1, "rpc_get_ram_cap 6\n");


/*    DBG(-1,"get_ram_cap event dispatch 1\n");
    CHECK(event_dispatch(ws));
    DBG(-1,"get_ram_cap event dispatch 2\n");
    CHECK(event_dispatch(ws));
    DBG(-1,"get_ram_cap event dispatch done\n");*/
    while(ram_size == 0) event_dispatch(ws);
//        DBG(ERR,"we did not receive ram!\n");
    *retcap = ram_cap;
    *ret_size = ram_size;
    ram_size = 0;

    thread_mutex_unlock(&chan->mutex);
    return SYS_ERR_OK;
}

errval_t aos_rpc_serial_getchar(struct aos_rpc *chan, char *retc)
{
    // TODO implement functionality to request a character from
    // the serial driver.
    return SYS_ERR_OK;
}

static errval_t putchar_send_handler(uintptr_t *args)
{
    assert((char *) args[1] != NULL);
    DBG(VERBOSE, "putchar_send_handler\n");

    struct aos_rpc *rpc = (struct aos_rpc *) args[0];

    errval_t err;
    err = lmp_chan_send2(&rpc->chan, LMP_FLAG_SYNC, rpc->chan.local_cap,
                         RPC_MESSAGE(RPC_TYPE_PUTCHAR), *((char *)args[1]));

    if (err_is_fail(err)){
        // Reregister if failed.
        CHECK(lmp_chan_register_send(&rpc->chan, get_default_waitset(),
                                     MKCLOSURE((void *) putchar_send_handler,
                                               args)));
    }

    return SYS_ERR_OK;
}

errval_t aos_rpc_serial_putchar(struct aos_rpc *rpc, char c)
{
    assert(rpc != NULL);

    struct waitset *ws = get_default_waitset();

    uintptr_t sendargs[2];
    sendargs[0] = (uintptr_t) rpc;
    sendargs[1] = (uintptr_t) &c;

    errval_t err;
    do {
        // check if sender is currently busy
        // TODO: could we implement some kind of buffer for this?
        err = lmp_chan_register_send(&rpc->chan, ws,
                                 MKCLOSURE((void *) putchar_send_handler,
                                           sendargs));
        CHECK(event_dispatch(ws));
    } while (err == LIB_ERR_CHAN_ALREADY_REGISTERED);

    return SYS_ERR_OK;
}

send_handler_0(process_get_pids_send_handler,
               RPC_MESSAGE(RPC_TYPE_PROCESS_GET_PIDS))
send_handler_1(process_get_name_send_handler,
               RPC_MESSAGE(RPC_TYPE_PROCESS_GET_NAME), args[1])

static errval_t kill_me_send_handler(void *args)
{
    DBG(VERBOSE, "kill_me_send_handler\n");

    uintptr_t *uargs = (uintptr_t *) args;

    struct aos_rpc *rpc = (struct aos_rpc *) uargs[0];
    struct capref *cap = (struct capref *) uargs[1];

    errval_t err;
    err = lmp_chan_send1(&rpc->chan, LMP_FLAG_SYNC, *cap,
                         RPC_MESSAGE(RPC_TYPE_PROCESS_KILL_ME));
    if (err_is_fail(err)){
        // Reregister if failed.
        CHECK(lmp_chan_register_send(&rpc->chan, get_default_waitset(),
                                     MKCLOSURE((void *) kill_me_send_handler,
                                               args)));
    }
    thread_mutex_unlock(&rpc->mutex);

    return SYS_ERR_OK;
}

errval_t aos_rpc_kill_me(struct aos_rpc *chan, struct capref disp)
{
    DBG(VERBOSE, "aos_rpc_kill_me\n");
    thread_mutex_lock(&chan->mutex);

    struct waitset *ws = get_default_waitset();

    uintptr_t sendargs[2];

    sendargs[0] = (uintptr_t) chan;
    sendargs[1] = (uintptr_t) &disp;

    CHECK(lmp_chan_register_send(&chan->chan, ws,
                                 MKCLOSURE((void *) kill_me_send_handler,
                                           sendargs)));

    CHECK(event_dispatch(ws));

    return SYS_ERR_OK;
}

static errval_t process_spawn_receive_handler(uintptr_t* args) {
    recv_handler_fleshy_bits(process_spawn_receive_handler,
                             -1, RPC_TYPE_PROCESS_SPAWN);
    args[8] = msg.words[1];
    return SYS_ERR_OK;
}

send_handler_8(process_spawn_send_handler, RPC_MESSAGE(RPC_TYPE_PROCESS_SPAWN),
               args[1], args[2], args[3], args[4], args[5], args[6], args[7],
               args[8]);

errval_t aos_rpc_process_spawn(struct aos_rpc *chan, char *name,
                               coreid_t core, domainid_t *newpid)
{
    return SYS_ERR_OK; //todo: implement this;
    struct waitset *ws = get_default_waitset();

    uintptr_t sendargs[9]; // 1+1

    sendargs[0] = (uintptr_t) chan;

    size_t totalcount = strlen(name);
    sendargs[1] = (uintptr_t)totalcount;

    if (totalcount > 6 * 4)
        return LRPC_ERR_WRONG_WORDCOUNT;

    for (int i = 0; i < totalcount; i++) {
        sendargs[(i >> 2) + 2] = (i % 4 ? sendargs[(i >> 2) + 2] : 0) +
            (name[i] << (8 * (i % 4)));
    }

    impl(process_spawn_send_handler, process_spawn_receive_handler, true);
    *newpid = (domainid_t) sendargs[8];

    return SYS_ERR_OK;
}

struct pgnrp_buf {
    size_t cur;
    size_t totalcount;
    char* name;
};

static errval_t process_get_name_receive_handler(uintptr_t *args) {
    recv_handler_fleshy_bits(process_get_name_receive_handler, -1,
                             RPC_TYPE_PROCESS_GET_NAME);

    int totalcount = (int) msg.words[1];
    char* temp = malloc(sizeof(char) * (totalcount + 1));

    if (totalcount > 4 * 6)
        return LRPC_ERR_WRONG_WORDCOUNT;

    for (int i = 0; i < totalcount; i++) {
        char *c = (char*) &(msg.words[(i >> 2) + 2]);
        temp[i] = c[i % 4];
    }

    args[2] = (uintptr_t) temp;
    return SYS_ERR_OK;
}

errval_t aos_rpc_process_get_name(struct aos_rpc *chan, domainid_t pid,
                                  char **name)
{
    struct waitset *ws = get_default_waitset();
    uintptr_t sendargs[3]; // 1 to send, 1 to receive
    sendargs[0] = (uintptr_t) chan;
    sendargs[1] = (uintptr_t) pid;

    impl(process_get_name_send_handler, process_get_name_receive_handler,
         true);

    *name = (char*) sendargs[2];

    return SYS_ERR_OK;
}

static errval_t process_get_pids_receive_handler(uintptr_t *args) {
    recv_handler_fleshy_bits(process_get_pids_receive_handler, -1,
                             RPC_TYPE_PROCESS_GET_PIDS);
    return LIB_ERR_NOT_IMPLEMENTED;
}

errval_t aos_rpc_process_get_all_pids(struct aos_rpc *chan,
                                      domainid_t **pids, size_t *pid_count)
{
    struct waitset *ws = get_default_waitset();
    uintptr_t sendargs[3]; // 0+2
    sendargs[0] = (uintptr_t) chan;

    impl(process_get_pids_send_handler, process_get_pids_receive_handler,
         true);

    *pid_count = (size_t) sendargs[1];
    *pids = (domainid_t *) sendargs[2];

    return LIB_ERR_NOT_IMPLEMENTED;
}

errval_t aos_rpc_get_device_cap(struct aos_rpc *rpc,
                                lpaddr_t paddr, size_t bytes,
                                struct capref *frame)
{
    return LIB_ERR_NOT_IMPLEMENTED;
}


/*
static errval_t handshake_send_handler(void* args)
{
    DBG(DETAILED, "handshake_send_handler\n");
    struct aos_rpc *rpc = (struct aos_rpc *) args;

    errval_t err;
    err = lmp_chan_send1(&rpc->chan, LMP_FLAG_SYNC, rpc->chan.local_cap,
                         RPC_MESSAGE(RPC_TYPE_HANDSHAKE));
    if (err_is_fail(err)){
        // Reregister if failed.
        CHECK(lmp_chan_register_send(&rpc->chan, get_default_waitset(),
                                     MKCLOSURE((void *) handshake_send_handler,
                                               args)));
    }
    return SYS_ERR_OK;
}
*/
static void aos_rpc_recv_handler(struct recv_list* data) {
    // do actions depending on the message type
    // Check the message type and handle it accordingly.
    switch (data->type) {
        case RPC_ACK_MESSAGE(RPC_TYPE_HANDSHAKE):
            DBG(-1, "ACK Received (handshake) \n");
            data->chan->remote_cap = data->cap;
            get_init_rpc()->init = true;
            break;
        case RPC_ACK_MESSAGE(RPC_TYPE_PUTCHAR):
            DBG(DETAILED, "ACK Received (putchar) \n");
            break;
        case RPC_ACK_MESSAGE(RPC_TYPE_RAM):
//            DBG(WARN, "RAM RPC received, but with standard handler\n"
//                    "This is not intended. Expect badness.\n");
            // TODO: Handle?
            ram_cap = data->cap;
            ram_size = data->size;
            break;
        case RPC_ACK_MESSAGE(RPC_TYPE_NUMBER):
            DBG(DETAILED, "ACK Received (number)\n");
            break;
        case RPC_ACK_MESSAGE(RPC_TYPE_STRING):
            DBG(DETAILED, "ACK Receied (string)\n");
            break;
        default:
            debug_printf("got message type %d\n", data->type);
            DBG(WARN, "Unable to handle RPC-receipt, expect badness!\n");
            //assert(!"NOT IMPLEMENTED");
    }
}

unsigned int id = 1337;
errval_t aos_rpc_init(struct aos_rpc *rpc)
{
    assert(rpc != NULL);
    rpc->init = false;
    CHECK(init_rpc_client(aos_rpc_recv_handler,&rpc->chan, cap_initep));
    // Send local ep to init and wait for init to acknowledge receiving
    // the endpoint.
    // Set send handler.
    struct waitset* ws = get_default_waitset();
    int unused_id;
    CHECK(send(&rpc->chan,rpc->chan.local_cap,RPC_MESSAGE(RPC_TYPE_HANDSHAKE),0,NULL,NULL_EVENT_CLOSURE,&unused_id));
    rpc->id = id;
    id++;
    set_init_rpc(rpc);
    // Wait for ACK.
    while (!rpc->init) {
        CHECK(event_dispatch(ws));
    }
    debug_printf("yes\n");

    return SYS_ERR_OK;
}

/**
 * \brief Returns the RPC channel to init.
 */
struct aos_rpc *aos_rpc_get_init_channel(void)
{
    //TODO check if was created
    return get_init_rpc();
}

/**
 * \brief Returns the channel to the memory server
 */
struct aos_rpc *aos_rpc_get_memory_channel(void)
{
    // TODO check if we want to create a separate mem server
    return get_init_rpc();
}

/**
 * \brief Returns the channel to the process manager
 */
struct aos_rpc *aos_rpc_get_process_channel(void)
{
    // TODO check if we want to create a separate process server
    return get_init_rpc();
}

/**
 * \brief Returns the channel to the serial console
 */
struct aos_rpc *aos_rpc_get_serial_channel(void)
{
    // TODO check if we want to create a separate serial server
    return get_init_rpc();
}
