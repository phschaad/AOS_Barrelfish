/**
 * \file
 * \brief Debugging functions
 */

/*
 * Copyright (c) 2008, 2010, 2011, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#ifndef BARRELFISH_DEBUG_H
#define BARRELFISH_DEBUG_H

#include <sys/cdefs.h>

#include <errors/errno.h>
#include <aos/caddr.h>
#include <stddef.h>
#include <barrelfish_kpi/registers_arch.h>

#define DEBUG_LEVEL WARN
#define RELEASE  0x0
#define ERR      0x1
#define WARN     0x2
#define VERBOSE  0x3
#define DETAILED 0x4

__unused
static char dbg_buff[500];

#define DBG(level, msg...) if(level <= DEBUG_LEVEL){                          \
                               sprintf(dbg_buff, msg);                            \
                               if(level == ERR){                              \
                                   debug_printf("\033[31m%s\033[0m", dbg_buff);   \
                               }else if(level == WARN){                       \
                                   debug_printf("\033[33m%s\033[0m", dbg_buff);   \
                               }else{                                         \
                                   debug_printf(dbg_buff);                        \
                               }                                              \
                           }

#define DBG_LINE DBG(DETAILED, "Function %s, Line %d\n" , __FUNCTION__, __LINE__)

__BEGIN_DECLS

struct capability;
errval_t debug_cap_identify(struct capref cap, struct capability *ret);
errval_t debug_dump_hw_ptables(void);
errval_t debug_cap_trace_ctrl(uintptr_t types, genpaddr_t start_addr, gensize_t size);
void debug_cspace(struct capref root);
void debug_my_cspace(void);
void debug_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int debug_print_cap(char *buf, size_t len, struct capability *cap);
int debug_print_cap_at_capref(char *buf, size_t len, struct capref cap);
int debug_print_capref(char *buf, size_t len, struct capref cap);
int debug_print_cnoderef(char *buf, size_t len, struct cnoderef cnode);

void debug_print_save_area(arch_registers_state_t *state);
void debug_print_fpu_state(arch_registers_fpu_state_t *state);
void debug_dump(arch_registers_state_t *state);
void debug_call_chain(arch_registers_state_t *state);
void debug_return_addresses(void);
void debug_dump_mem_around_addr(lvaddr_t addr);
void debug_dump_mem(lvaddr_t base, lvaddr_t limit, lvaddr_t point);

void debug_err(const char *file, const char *func, int line,
               errval_t err, const char *msg, ...);
void check_err(errval_t err, const char *file, const char *fun, int line,
               const char *msg, ...);
void user_panic_fn(const char *file, const char *func, int line,
                   const char *msg, ...)
    __attribute__((noreturn));

void dump_bootinfo(struct bootinfo *bi, coreid_t my_core_id);

#ifdef NDEBUG
# define DEBUG_ERR(err, msg...) ((void)0)
# define HERE ((void)0)
# define CHECK(fun) fun
# define CHECK_MSG(fun, msg...) fun
#else
# define DEBUG_ERR(err, msg...) debug_err(__FILE__, __func__, __LINE__, err, msg)
# include <aos/dispatch.h>
# define HERE fprintf(stderr, "Disp %.*s.%u: %s, %s, %u\n", \
                        DISP_NAME_LEN, disp_name(), disp_get_core_id(), \
                      __FILE__, __func__, __LINE__)
# define CHECK(fun) check_err(fun, __FILE__, __func__, __LINE__, "")
# define CHECK_MSG(fun, msg...) check_err(fun, __FILE__, __func__, __LINE__, msg)
#endif

/**
 * \brief Prints out a string, errval and then aborts the domain
 */
#define USER_PANIC_ERR(err, msg...) do {               \
    debug_err(__FILE__, __func__, __LINE__, err, msg); \
    abort();                                           \
} while (0)

/**
 * \brief Prints out a string and abort the domain
 */
#define USER_PANIC(msg...)                                 \
    user_panic_fn(__FILE__, __func__, __LINE__, msg);      \

__END_DECLS

#endif //BARRELFISH_DEBUG_H
