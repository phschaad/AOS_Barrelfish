#include <stdio.h>
#include <stdlib.h>

#include <aos/aos.h>
#include <aos/aos_rpc_shared.h>
#include <aos/generic_threadsafe_queue.h>

#undef HERE
//#define HERE debug_printf("Here: %s %s %u\n",__FILE__, __func__, __LINE__)
#define HERE

//todo: make properly platform independent and stuff

static errval_t actual_sending(struct lmp_chan * chan, struct capref cap, int first_byte, size_t payloadcount, uintptr_t* payload) {
    HERE;
    //debug_printf("actual sending dump: chan %p, first byte %d, payloadcount %u, payload %p\n",chan,first_byte,payloadcount,payload);
    switch(payloadcount) {
        case 0:
            return (lmp_chan_send1(chan, LMP_FLAG_SYNC, cap, first_byte));
        case 1:
            return (lmp_chan_send2(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0]));
        case 2:
            return(lmp_chan_send3(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0], payload[1]));
        case 3:
            return(lmp_chan_send4(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0], payload[1], payload[2]));
        case 4:
            return(lmp_chan_send5(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0], payload[1], payload[2], payload[3]));
        case 5:
            return(lmp_chan_send6(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0], payload[1], payload[2], payload[3], payload[4]));
        case 6:
            return(lmp_chan_send7(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0], payload[1], payload[2], payload[3], payload[4], payload[5]));
        case 7:
            return(lmp_chan_send8(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0], payload[1], payload[2], payload[3], payload[4], payload[5], payload[6]));
        case 8:
            return(lmp_chan_send9(chan, LMP_FLAG_SYNC, cap, first_byte, payload[0], payload[1], payload[2], payload[3], payload[4], payload[5], payload[6], payload[7]));
        default:
            USER_PANIC("invalid argcount in actual_sending\n");
    }
    return SYS_ERR_OK;
}

struct send_queue {
    unsigned char type;
    unsigned char id;
    struct lmp_chan* chan;
    struct capref cap;
    size_t index;
    size_t size;
    unsigned int* payload;
    struct event_closure callback_when_done;
};

static int rpc_type_min_id[256];
static int rpc_type_max_id[256];
struct generic_queue_obj rpc_send_queue;

//todo: proper error handling
static errval_t send_loop(void * args) {
    HERE;
    struct send_queue *sq = (struct send_queue *) args;
    struct capref cap;
    size_t remaining = (sq->size-sq->index);
    int first_byte = (sq->type << 24) + (sq->id << 16) + sq->size;
    if(sq->index == 0) {
        cap = sq->cap;
    }
    else {
        cap = NULL_CAP;
    }
    errval_t err;
    //debug_printf("send_loop chan %p, endpoint %p, cnode something %d\n",sq->chan,sq->chan->endpoint,(int)sq->chan->remote_cap.cnode.cnode);
    err = actual_sending(sq->chan,cap,first_byte,remaining > 8 ? 8 : remaining,&sq->payload[sq->index]);
    HERE;
    if (err_is_fail(err)){
        debug_printf("we had an error: %s\n",err_getstring(err));
        // Reregister if failed.
        CHECK(lmp_chan_register_send(sq->chan, get_default_waitset(),
                                     MKCLOSURE((void *) send_loop,
                                               args)));
        HERE;
        CHECK(event_dispatch(get_default_waitset()));
    }else{
        if(remaining > 8) {
            //we still have data to send, so we adjust and resend
            HERE;
            sq->index += 8;
            CHECK(lmp_chan_register_send(sq->chan, get_default_waitset(),
                                         MKCLOSURE((void *) send_loop,
                                                   args)));
            HERE;
            CHECK(event_dispatch(get_default_waitset()));
        }
        else{
            HERE;
            if(sq->callback_when_done.handler != NULL)
                sq->callback_when_done.handler(sq->callback_when_done.arg);
            bool done = true;
            HERE;
            synchronized(rpc_send_queue.thread_mutex) {
                assert(rpc_send_queue.fst->data == args);
                struct generic_queue *temp = rpc_send_queue.fst; //dequeue the one we just got done with and delete it
                HERE;
                rpc_send_queue.fst = rpc_send_queue.fst->next;
                if(rpc_type_min_id[sq->type] == sq->id) {
                    //todo: make proper
                    rpc_type_min_id[sq->type]++;
                }
                HERE;
                free(temp->data);
                    HERE;
                free(temp);
                    HERE;
                if(rpc_send_queue.fst != NULL) {//more to be ran
                    done = false;

                }else{
                    rpc_send_queue.last = NULL;
                }
            }
            if(!done) {
                CHECK(lmp_chan_register_send(((struct send_queue*)rpc_send_queue.fst->data)->chan, get_default_waitset(),
                                             MKCLOSURE((void *) send_loop,
                                                       rpc_send_queue.fst->data)));
                HERE;
                CHECK(event_dispatch(get_default_waitset()));
            }
            HERE;
            return SYS_ERR_OK;
        }
    }
    HERE;
    return SYS_ERR_OK;
}

errval_t send(struct lmp_chan * chan, struct capref cap, unsigned char type, size_t payloadsize, uintptr_t* payload, struct event_closure callback_when_done, int* id) {
    //obtain fresh ID for type
    //then enqueue message into sending loop
    assert(payloadsize < 65536); //otherwise we can't encode the size in 16 bits
    HERE;
    bool need_to_start = false;
    synchronized(rpc_send_queue.thread_mutex) {
        unsigned char min_id = rpc_type_min_id[type];
        unsigned char max_id = rpc_type_max_id[type];
        if(max_id+1 % 256 == min_id)
            USER_PANIC("we ran out of message slots\n"); //todo: turn this into a proper error msg that gets returned and is expected to be handled by the caller
        rpc_type_max_id[type] = max_id+1 % 256;
        struct send_queue* sq = malloc(sizeof(struct send_queue));
            HERE;
        *id = max_id;
            HERE;
        sq->type = type;
        sq->id = max_id;
        sq->index = 0;
        sq->size = payloadsize;
        sq->payload = payload;
        sq->cap = cap;
        sq->chan = chan;
        sq->callback_when_done = callback_when_done;
        HERE;
        struct generic_queue *new = malloc(sizeof(struct generic_queue));
        HERE;

        new->data = (void*)sq;
        new->next = NULL;

        if (rpc_send_queue.last == NULL) {
            assert(rpc_send_queue.fst == NULL);
            need_to_start = true;
            rpc_send_queue.fst = new;
            rpc_send_queue.last = new;
        } else {
            rpc_send_queue.last->next = new;
            rpc_send_queue.last = new;
        }
            HERE;
    }
    if(need_to_start) {
        HERE;
        CHECK(lmp_chan_register_send(((struct send_queue*)rpc_send_queue.fst->data)->chan, get_default_waitset(),
                                     MKCLOSURE((void *) send_loop,
                                               rpc_send_queue.fst->data)));
        HERE;
        CHECK(event_dispatch(get_default_waitset()));
        HERE;
    }
    return SYS_ERR_OK;
}

static struct recv_list *rpc_recv_lookup(struct recv_list *head, unsigned char type, unsigned char id) {
    HERE;
    while(head!= NULL) {
        if(head->type == type && head->id == id)
            return head;
        head = head->next;
    }
    return NULL;
}

static void rpc_recv_list_remove(struct recv_list **rl_head, struct recv_list* rl) {
    assert(rl != NULL);
    HERE;
    struct recv_list *prev =NULL;
    struct recv_list *cur = *rl_head;
    while(true) {
        if(rl == cur)
            break;
        prev = cur;
        cur = cur->next;
    }
    if(prev == NULL) {
        *rl_head = cur->next;
    }else {
        prev->next = cur->next;
    }
}


struct send_cleanup_struct {
    void *data;
    struct event_closure callback;
};
static void send_cleanup(void * data){
    HERE;
    struct send_cleanup_struct* scs = (struct send_cleanup_struct*)data;
    free(scs->data);
    if(scs->callback.handler != NULL)
        scs->callback.handler(scs->callback.arg);
    free(data);
}

// this function persists the payload during the call and initiates the after-call cleanup
errval_t persist_send_cleanup_wrapper(struct lmp_chan * chan, struct capref cap, unsigned char type, size_t payloadsize, void* payload, struct event_closure callback_when_done, int* id) {
    HERE;
    debug_printf("pscw 1\n");
    size_t trailing = (payloadsize % 4 != 0 ? 4 - (payloadsize %4) : 0);
    size_t payloadsize2 = (payloadsize + trailing) /4;
    uintptr_t* payload2 = malloc(payloadsize2);
    memcpy(payload2,payload,payloadsize);
    if(trailing != 0)
        memset(&((char*)payload2)[payloadsize],0,trailing);
    struct send_cleanup_struct* scs = malloc(sizeof(struct send_cleanup_struct));
    scs->data = payload2;
    scs->callback = callback_when_done;
    struct event_closure callback2 = MKCLOSURE(send_cleanup,scs);
    debug_printf("pscw 2\n");
    return send(chan,cap,type,payloadsize2,payload2,callback2,id);
}

// conveniance function for creating responses
errval_t send_response(struct recv_list *rl, struct lmp_chan *chan, struct capref cap, size_t payloadsize, void* payload){
    // ACKs should not generate ACKs
    assert((rl->type & 0x1) == 0);
    HERE;
    // add the id to the data
    // round to 32 bit
    // ( we are assuming here that the id is smaller than an int)
    size_t payloadsize2 = payloadsize + 1;
    uintptr_t * payload2 = malloc(payloadsize2);
    if(payloadsize2 > 1)
        memcpy(&payload2[1],payload,payloadsize);
    payload2[0] = (int) rl->id;
    struct send_cleanup_struct* scs = malloc(sizeof(struct send_cleanup_struct));
    scs->data = payload2;
    scs->callback = NULL_EVENT_CLOSURE;
    struct event_closure callback = MKCLOSURE(send_cleanup,scs);
    int ignored_id;
    return send(chan,cap,rl->type + 1,payloadsize2,payload2,callback,&ignored_id);

    return persist_send_cleanup_wrapper(chan, cap, rl->type + 1, payloadsize2, payload2, callback,&ignored_id);
}

//todo: error handling
void recv_handling(void* args) {
    debug_printf("received a msg...\n");
    HERE;

    struct recv_chan *rc = (struct recv_chan *) args;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    struct capref cap;

    lmp_chan_recv(rc->chan, &msg, &cap);
//    if(cap != NULL_CAP) //todo: figure out if this if check would be a good idea or not when properly implemented
//    -> you mean, to ensure that ram RPCs send a cap, for example? not all rpcs need to send caps.
//    we should consider freeing the slot again when not used anymore.
    lmp_chan_alloc_recv_slot(rc->chan);
    lmp_chan_register_recv(
            rc->chan, get_default_waitset(),
            MKCLOSURE(recv_handling, args));

    assert(msg.buf.msglen > 0);
//    DBG(VERBOSE, "Handling RPC receive event (type %d)\n", msg.words[0]);
    unsigned char type = msg.words[0] >> 24;
    unsigned char id = (msg.words[0] >> 16) & 0xFF;
    size_t size = msg.words[0] & 0xFFFF;

    if(size < 9) //fast path for small messages
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
    }else {
        struct recv_list *rl = rpc_recv_lookup(rc->rpc_recv_list, type, id);
        if (rl == NULL) {
            rl = malloc(sizeof(struct recv_list));
            rl->payload = malloc(size*4);
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
        memcpy(&rl->payload[rl->index],&msg.words[1],count*4);
        rl->index+=count;
        assert(rl->index <= rl->size);
        if(rl->index == rl->size) {
            //done transferring this msg, we can now do whatever we should do with this
            rpc_recv_list_remove(&rc->rpc_recv_list,rl);
            rc->recv_deal_with_msg(rl);
            free(rl);
        }
    }
}

errval_t init_rpc_client(void (*recv_deal_with_msg)(struct recv_list *), struct lmp_chan* chan, struct capref dest) {
    thread_mutex_init(&rpc_send_queue.thread_mutex);
    // Allocate lmp channel structure.
    // Create local endpoint.
    // Set remote endpoint to dest's endpoint.
    CHECK(lmp_chan_accept(chan, DEFAULT_LMP_BUF_WORDS, dest));
    lmp_chan_alloc_recv_slot(chan);
    struct recv_chan *rc = malloc(sizeof(struct recv_chan));
    rc->chan = chan;
    rc->recv_deal_with_msg = recv_deal_with_msg;
    rc->rpc_recv_list = NULL;

    lmp_chan_register_recv(
            rc->chan, get_default_waitset(),
            MKCLOSURE(recv_handling, rc));
    return SYS_ERR_OK;
}

errval_t init_rpc_server(void (*recv_deal_with_msg)(struct recv_list *), struct lmp_chan* chan) {
    thread_mutex_init(&rpc_send_queue.thread_mutex);

    CHECK(lmp_chan_accept(chan, DEFAULT_LMP_BUF_WORDS, NULL_CAP));
    CHECK(lmp_chan_alloc_recv_slot(chan));
    CHECK(cap_copy(cap_initep, chan->local_cap));
    struct recv_chan *rc = malloc(sizeof(struct recv_chan));
    rc->chan = chan;
    rc->recv_deal_with_msg = recv_deal_with_msg;
    rc->rpc_recv_list = NULL;

    lmp_chan_register_recv(
            rc->chan, get_default_waitset(),
            MKCLOSURE(recv_handling, rc));
    return SYS_ERR_OK;
}