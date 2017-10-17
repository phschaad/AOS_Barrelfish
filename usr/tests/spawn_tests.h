#include "mem_alloc.h"
#include <mm/mm.h>
#include <aos/paging.h>

static int spawn_hello5(void)
{
    TEST_PRINT_INFO("Start the hello process 5 times");

    errval_t err;

    for(int i = 0; i<10; ++i){
        struct spawninfo* si = (struct spawninfo*) malloc(sizeof(struct spawninfo));
        err = spawn_load_by_name("hello", si);
        free(si);
        if (err_is_fail(err)) {
            TEST_PRINT_FAIL();
        }
    }
    TEST_PRINT_SUCCESS();
}