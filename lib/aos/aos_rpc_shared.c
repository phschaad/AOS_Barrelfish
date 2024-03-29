#include <stdio.h>
#include <stdlib.h>

#include <aos/aos.h>
#include <aos/aos_rpc_shared.h>
#include <aos/generic_threadsafe_queue.h>

// TODO: make properly platform independent and stuff

static errval_t actual_sending(struct lmp_chan *chan, struct capref cap,
                               int first_byte, size_t payloadcount,
                               uintptr_t *payload)
{
    if (!chan) {
        USER_PANIC("no more chan...");
    }
    switch (payloadcount) {
    case 0:
        return lmp_chan_send1(chan, LMP_FLAG_SYNC, cap, first_byte);
    case 1:
        return lmp_chan_send2(chan, LMP_FLAG_SYNC, cap, first_byte,
                              payload[0]);
    case 2:
        return lmp_chan_send3(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0],
                              payload[1]);
    case 3:
        return lmp_chan_send4(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0],
                              payload[1], payload[2]);
    case 4:
        return lmp_chan_send5(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0],
                              payload[1], payload[2], payload[3]);
    case 5:
        return lmp_chan_send6(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0],
                              payload[1], payload[2], payload[3], payload[4]);
    case 6:
        return lmp_chan_send7(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0],
                              payload[1], payload[2], payload[3], payload[4],
                              payload[5]);
    case 7:
        return lmp_chan_send8(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0],
                              payload[1], payload[2], payload[3], payload[4],
                              payload[5], payload[6]);
    case 8:
        return lmp_chan_send9(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0],
                              payload[1], payload[2], payload[3], payload[4],
                              payload[5], payload[6], payload[7]);
    default:
        USER_PANIC("invalid argcount in actual_sending\n");
    }
    return SYS_ERR_OK;
}

struct chan_list {
    struct lmp_chan *chan;
    struct generic_queue_obj rpc_send_queue;
    struct chan_list *next;
};

struct send_queue {
    unsigned char type;
    unsigned char id;
    struct lmp_chan *chan;
    struct capref cap;
    size_t index;
    size_t size;
    unsigned int *payload;
    struct chan_list *parent;
    struct event_closure callback_when_done;
};

struct chan_list* chan_listing;


static int rpc_type_min_id[256];
static int rpc_type_max_id[256];
bool mutex_init = false;
struct thread_mutex chan_list_mutex;

// TODO: proper error handling
// TODO: also make it stop growing the stack by trampolining the event_dispatch stuff properly
static errval_t send_loop(void *args)
{
    struct send_queue *sq = (struct send_queue *) args;
    struct capref cap;
    size_t remaining = (sq->size - sq->index);
    int first_byte = (sq->type << 24) + (sq->id << 16) + sq->size;
    if (sq->index == 0) {
        cap = sq->cap;
    } else {
        cap = NULL_CAP;
    }

    errval_t err =
        actual_sending(sq->chan, cap, first_byte,
                       remaining > 8 ? 8 : remaining, &sq->payload[sq->index]);
    if (err_is_fail(err)) {
        if(!lmp_err_is_transient(err)) {
            debug_printf("send loop error: %s\n", err_getstring(err));
            debug_printf("print cap: slot %u, level %u, cnode %u, croot %u\n", (unsigned int) sq->cap.slot, (
                                 unsigned int) sq->cap.cnode.level, (unsigned int) sq->cap.cnode.cnode,
                         (unsigned int) sq->cap.cnode.croot);
            debug_printf("sq info: raw type %u, id %u, index %u, size %u\n",(unsigned int)sq->type,(unsigned int)sq->id,(unsigned int)sq->index,(unsigned int)sq->size);
            debug_printf("sq chan info: connstate %u, has remote cap %u\n",(unsigned int)sq->chan->connstate,(
            unsigned int)(sq->chan->remote_cap.cnode.croot != 0));
            if(sq->type == 6) {
                debug_printf("sending string, msg is: %s\n",(char*)&(sq->payload[2]));
            }
            assert(!"we don't deal well with non-transient errors yet, please consider fixing - adding a callback to call in case of non-transient send error appears to me to be the most appropriate way to fix this\n");
        }
        else {
            //we tried and failed in a transient way, so we reregister
            CHECK(lmp_chan_register_send(sq->chan, get_default_waitset(),
                                         MKCLOSURE((void *) send_loop, sq)));
                   }
//        debug_printf("hi from hell\n");
//        CHECK(event_dispatch(get_default_waitset()));
    } else {
        if (remaining > 8) {
            // we still have data to send, so we adjust and resend
            sq->index += 8;
            CHECK(lmp_chan_register_send(sq->chan, get_default_waitset(),
                                         MKCLOSURE((void *) send_loop, args)));
//            debug_printf("hi from hell\n");
//            CHECK(event_dispatch(get_default_waitset()));
        } else {
            if (sq->callback_when_done.handler != NULL)
                sq->callback_when_done.handler(sq->callback_when_done.arg);
            bool done = true;
            synchronized(sq->parent->rpc_send_queue.thread_mutex)
            {
                assert(sq->parent->rpc_send_queue.fst->data == args);

                struct generic_queue *temp =
                        sq->parent->rpc_send_queue.fst; // dequeue the one we just got done
                                        // with and delete it
                sq->parent->rpc_send_queue.fst = sq->parent->rpc_send_queue.fst->next;

                if (rpc_type_min_id[sq->type] == sq->id) {
                    DBG(VERBOSE,"cleanup on type %u id %u\n",(
                    unsigned int)sq->type,(unsigned int)sq->id);
                    // TODO: make proper
                    rpc_type_min_id[sq->type] =
                        (rpc_type_min_id[sq->type] + 1) % 256;
                }

                free(temp->data);
                free(temp);

                if (sq->parent->rpc_send_queue.fst != NULL) // more to be ran
                    done = false;
                else
                    sq->parent->rpc_send_queue.last = NULL;

            }
            if (!done) {
                CHECK(lmp_chan_register_send(
                    ((struct send_queue *) sq->parent->rpc_send_queue.fst->data)->chan,
                    get_default_waitset(),
                    MKCLOSURE((void *) send_loop, sq->parent->rpc_send_queue.fst->data)));
//                debug_printf("hi from hell2\n");
//                CHECK(event_dispatch(get_default_waitset()));
            }
            return SYS_ERR_OK;
        }
    }
    return SYS_ERR_OK;
}

// TODO: consider using a bitfield instead of this fairly brittle thing
unsigned char request_fresh_id(unsigned char type)
{
    unsigned char id;
    synchronized(chan_list_mutex)
    {
        unsigned char min_id = rpc_type_min_id[type];
        unsigned char max_id = rpc_type_max_id[type];
        if ((max_id + 1) % 256 == min_id)
            USER_PANIC("we ran out of message slots\n");
        // TODO: turn this into a proper error msg that gets returned and is
        // expected to be handled by the caller.

        rpc_type_max_id[type] = (max_id + 1) % 256;
        id = max_id;
    }
    return id;
}

errval_t send(struct lmp_chan *chan, struct capref cap, unsigned char type,
              size_t payloadsize, uintptr_t *payload,
              struct event_closure callback_when_done, unsigned char id)
{
    // obtain fresh ID for type then enqueue message into sending loop
    assert(payloadsize < 65536); // Needed so we can encode the size in 16 bits
    struct chan_list *chan_entry = NULL;
    bool done = false;
    synchronized(chan_list_mutex) {
            struct chan_list *prev = NULL;
            chan_entry = chan_listing;
            while(!done && chan_entry != NULL) {
                if(chan_entry->chan == chan) {//found chan
                    done = true;
                }else{
                    //disabled that case for now
                    if(false && chan->connstate == LMP_DISCONNECTED) {
                        debug_printf("found disconnected channel, removing it\n");
                        //todo: callback that channel was disconnected
                        if(prev != NULL) {
                            prev->next = chan_entry->next;
                        }else{
                            chan_listing = chan_entry->next;
                        }
                        struct chan_list* temp_cl = chan_entry;
                        chan_entry = chan_entry->next;
                        free(temp_cl);
                    }else {
                        prev = chan_entry;
                        chan_entry = chan_entry->next;
                    }
                }
            }
            if(!done) {
                DBG(VERBOSE,"making new chan list entry\n");
                chan_entry = malloc(sizeof(struct chan_list));
                chan_entry->chan = chan;
                thread_mutex_init(&chan_entry->rpc_send_queue.thread_mutex);
                chan_entry->rpc_send_queue.fst = NULL;
                chan_entry->rpc_send_queue.last = NULL;
                chan_entry->next = chan_listing;
                chan_listing = chan_entry;
            }
        }

    bool need_to_start = false;
    DBG(DETAILED, "Sending message with: raw type %u id %u\n", type, id);
    synchronized(chan_entry->rpc_send_queue.thread_mutex)
    {
        struct send_queue *sq = malloc(sizeof(struct send_queue));

        sq->type = type;
        sq->id = id;
        sq->index = 0;
        sq->size = payloadsize;
        sq->payload = payload;
        sq->cap = cap;
        sq->chan = chan;
        sq->callback_when_done = callback_when_done;
        sq->parent = chan_entry;

        struct generic_queue *new = malloc(sizeof(struct generic_queue));

        new->data = (void *) sq;
        new->next = NULL;

        if (chan_entry->rpc_send_queue.last == NULL) {
            assert(chan_entry->rpc_send_queue.fst == NULL);
            need_to_start = true;
            chan_entry->rpc_send_queue.fst = new;
            chan_entry->rpc_send_queue.last = new;
        } else {
            chan_entry->rpc_send_queue.last->next = new;
            chan_entry->rpc_send_queue.last = new;
        }
    }
    if (need_to_start) {
        CHECK(lmp_chan_register_send(
            ((struct send_queue *) chan_entry->rpc_send_queue.fst->data)->chan,
            get_default_waitset(),
            MKCLOSURE((void *) send_loop, chan_entry->rpc_send_queue.fst->data)));
        CHECK(event_dispatch(get_default_waitset()));
    }
    return SYS_ERR_OK;
}

static struct recv_list *rpc_recv_lookup(struct recv_list *head,
                                         unsigned char type, unsigned char id)
{
    while (head != NULL) {
/*        if(type == 11)
            debug_printf("found type %u with id %u\n",(unsigned int)head->type,(unsigned int)id);
*/        if (head->type == type && head->id == id)
            return head;
        head = head->next;
    }
    return NULL;
}

static void rpc_recv_list_remove(struct recv_list **rl_head,
                                 struct recv_list *rl)
{
    assert(rl != NULL);

    struct recv_list *prev = NULL;
    struct recv_list *cur = *rl_head;
    while (true) {
        if (rl == cur)
            break;
        prev = cur;
        cur = cur->next;
    }

    if (prev == NULL)
        *rl_head = cur->next;
    else
        prev->next = cur->next;
}

struct send_cleanup_struct {
    void *data;
    struct event_closure callback;
};

static void send_cleanup(void *data)
{
    struct send_cleanup_struct *scs = (struct send_cleanup_struct *) data;
    free(scs->data);
    if (scs->callback.handler != NULL)
        scs->callback.handler(scs->callback.arg);
    free(data);
}

// this function persists the payload during the call and initiates the
// after-call cleanup
errval_t persist_send_cleanup_wrapper(struct lmp_chan *chan, struct capref cap,
                                      unsigned char type, size_t payloadsize,
                                      void *payload,
                                      struct event_closure callback_when_done,
                                      unsigned char id)
{
    size_t trailing = (payloadsize % 4 != 0 ? 4 - (payloadsize % 4) : 0);
    size_t payloadsize2 = (payloadsize + trailing) / 4;
    uintptr_t *payload2 = malloc(payloadsize2);

    memcpy(payload2, payload, payloadsize);

    if (trailing != 0)
        memset(&((char *) payload2)[payloadsize], 0, trailing);

    struct send_cleanup_struct *scs =
        malloc(sizeof(struct send_cleanup_struct));

    scs->data = payload2;
    scs->callback = callback_when_done;

    struct event_closure callback2 = MKCLOSURE(send_cleanup, scs);

    return send(chan, cap, type, payloadsize2, payload2, callback2, id);
}

// conveniance function for creating responses
errval_t send_response(struct recv_list *rl, struct lmp_chan *chan,
                       struct capref cap, size_t payloadsize, void *payload)
{
    // ACKs should not generate ACKs
    assert((rl->type & 0x1) == 0);

    // add the id to the data
    // round to 32 bit
    // ( we are assuming here that the id is smaller than an int)
    size_t payloadsize2 = payloadsize + 1;
    uintptr_t *payload2 = malloc(payloadsize2 * sizeof(uintptr_t));

    if (payloadsize2 > 1)
        memcpy(&payload2[1], payload, payloadsize * sizeof(uintptr_t));

    payload2[0] = (int) rl->id;
    struct send_cleanup_struct *scs =
        malloc(sizeof(struct send_cleanup_struct));

    scs->data = payload2;
    scs->callback = NULL_EVENT_CLOSURE;

    struct event_closure callback = MKCLOSURE(send_cleanup, scs);

    return send(chan, cap, rl->type + 1, payloadsize2, payload2, callback,
                request_fresh_id(rl->type + 1));
}
// conveniance function for forwarding messages
errval_t forward_message(struct recv_list *rl, struct lmp_chan *chan,
                       struct capref cap, size_t payloadsize, void *payload)
{
    // add the id to the data
    // round to 32 bit
    // ( we are assuming here that the id is smaller than an int)
    size_t payloadsize2 = payloadsize + 1;
    uintptr_t *payload2 = malloc(payloadsize2 * sizeof(uintptr_t));

    if (payloadsize2 > 1)
        memcpy(&payload2[1], payload, payloadsize * sizeof(uintptr_t));

    payload2[0] = (int) rl->id;
    struct send_cleanup_struct *scs =
        malloc(sizeof(struct send_cleanup_struct));

    scs->data = payload2;
    scs->callback = NULL_EVENT_CLOSURE;

    struct event_closure callback = MKCLOSURE(send_cleanup, scs);

    return send(chan, cap, rl->type, payloadsize2, payload2, callback,
                request_fresh_id(rl->type));
}

static int refill_nono = 0;

// TODO: error handling
void recv_handling(void *args)
{
    free(malloc(100));
    refill_nono++;
    struct recv_chan *rc = (struct recv_chan *) args;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    struct capref cap;

    lmp_chan_recv(rc->chan, &msg, &cap);
    if (!capref_is_null(cap)) {
        //we logically only need to realloc, if we received a cap
        lmp_chan_alloc_recv_slot(rc->chan);
    }
    CHECK(lmp_chan_register_recv(rc->chan, get_default_waitset(),
                           MKCLOSURE(recv_handling, args)));
    slot_alloc_refill_preallocated_slots_conditional(refill_nono);

    assert(msg.buf.msglen > 0);

    unsigned char type = msg.words[0] >> 24;
    unsigned char id = (msg.words[0] >> 16) & 0xFF;
    size_t size = msg.words[0] & 0xFFFF;
    DBG(DETAILED, "Received message with: type 0x%x %s id %u and size %u\n", type >> 1, type & 1 ? "ACK" : "", id, size);

    //debug_printf("receive with size: %d\n", size);
    if (size < 9) // fast path for small messages
    {
        struct recv_list rl;
        rl.payload = &msg.words[1];
        rl.size = size;
        rl.id = id;
        rl.type = type;
        rl.cap = cap;
        rl.index = size;
        rl.next = NULL;
        rl.chan = rc->chan;
        rc->recv_deal_with_msg(&rl);
    } else {
        //todo: give it its own mutex instead of using the send_queue's mutex
        synchronized(chan_list_mutex) {
            struct recv_list *rl = rpc_recv_lookup(rc->rpc_recv_list, type, id);
            if (rl == NULL) {
                rl = malloc(sizeof(struct recv_list));
                rl->payload = malloc(size * 4);
                rl->size = size;
                rl->id = id;
                rl->type = type;
                rl->cap = cap;
                rl->index = 0;
                rl->chan = rc->chan;
                rl->next = rc->rpc_recv_list;
                rc->rpc_recv_list = rl;
            }
            size_t rem = rl->size - rl->index;
            size_t count = (rem > 8 ? 8 : rem);
            memcpy(&rl->payload[rl->index], &msg.words[1], count * 4);
            rl->index += count;
            assert(rl->index <= rl->size);
            if (rl->index == rl->size) {
                // done transferring this msg, we can now do whatever we should do
                // with this
                rpc_recv_list_remove(&rc->rpc_recv_list, rl);
                DBG(VERBOSE, "call long msg handler\n");
                rc->recv_deal_with_msg(rl);
                free(rl->payload);
                free(rl);
            }
        }
    }
    refill_nono--;
}

errval_t init_rpc_client(void (*recv_deal_with_msg)(struct recv_list *),
                         struct lmp_chan *chan, struct capref dest)
{
    if(!mutex_init) {
        thread_mutex_init(&chan_list_mutex);
        mutex_init = true;
    }
    // Allocate lmp channel structure.
    // Create local endpoint.
    // Set remote endpoint to dest's endpoint.
    CHECK(lmp_chan_accept(chan, DEFAULT_LMP_BUF_WORDS, dest));
    lmp_chan_alloc_recv_slot(chan);
    struct recv_chan *rc = malloc(sizeof(struct recv_chan));
    rc->chan = chan;
    rc->recv_deal_with_msg = recv_deal_with_msg;
    rc->rpc_recv_list = NULL;

    lmp_chan_register_recv(rc->chan, get_default_waitset(),
                           MKCLOSURE(recv_handling, rc));
    return SYS_ERR_OK;
}

errval_t init_rpc_server(void (*recv_deal_with_msg)(struct recv_list *),
                         struct lmp_chan *chan)
{
    if(!mutex_init) {
        thread_mutex_init(&chan_list_mutex);
        mutex_init = true;
    }
    CHECK(lmp_chan_accept(chan, DEFAULT_LMP_BUF_WORDS, NULL_CAP));
    CHECK(lmp_chan_alloc_recv_slot(chan));
    struct recv_chan *rc = malloc(sizeof(struct recv_chan));
    rc->chan = chan;
    rc->recv_deal_with_msg = recv_deal_with_msg;
    rc->rpc_recv_list = NULL;

    lmp_chan_register_recv(rc->chan, get_default_waitset(),
                           MKCLOSURE(recv_handling, rc));
    return SYS_ERR_OK;
}

void convert_charptr_to_uintptr_with_padding_and_copy(const char *in,
                                                      size_t charsize,
                                                      uintptr_t **out,
                                                      size_t *outsize)
{
    size_t trailing = (charsize % 4 != 0 ? 4 - (charsize % 4) : 0);
    *outsize = (charsize + trailing) / 4;
    *out = malloc((*outsize) * sizeof(uintptr_t));

    memcpy(*out, in, charsize);

    if (trailing != 0)
        memset(&((char *) *out)[charsize], 0, trailing);
}
