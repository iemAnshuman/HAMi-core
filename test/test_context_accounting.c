#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include "../src/cuda/context_accounting.h"

#define CONTEXT_BYTES 436207616UL

static void test_nested_lifetime(void) {
    primary_context_accounting_t state = {0};
    size_t bytes = 0;

    assert(primary_context_record_retain(&state, CONTEXT_BYTES, &bytes) == 0);
    assert(bytes == CONTEXT_BYTES);
    assert(state.retain_count == 1);

    assert(primary_context_record_retain(&state, CONTEXT_BYTES, &bytes) == 0);
    assert(bytes == 0);
    assert(state.retain_count == 2);

    assert(primary_context_record_release(&state, &bytes) == 0);
    assert(bytes == 0);
    assert(state.retain_count == 1);

    assert(primary_context_record_retain(&state, CONTEXT_BYTES, &bytes) == 0);
    assert(bytes == 0);
    assert(state.retain_count == 2);

    assert(primary_context_record_release(&state, &bytes) == 0);
    assert(bytes == 0);
    assert(primary_context_record_release(&state, &bytes) == 0);
    assert(bytes == CONTEXT_BYTES);
    assert(state.retain_count == 0);
}

static void test_probe_is_not_charged(void) {
    primary_context_accounting_t state = {0};
    size_t bytes = 99;

    assert(primary_context_record_retain(&state, 0, &bytes) == 0);
    assert(bytes == 0);
    assert(primary_context_record_release(&state, &bytes) == 0);
    assert(bytes == 0);

    assert(primary_context_record_retain(&state, CONTEXT_BYTES, &bytes) == 0);
    assert(bytes == CONTEXT_BYTES);
}

static void test_failed_add_is_not_removed(void) {
    primary_context_accounting_t state = {0};
    size_t bytes = 0;

    assert(primary_context_record_retain(&state, CONTEXT_BYTES, &bytes) == 0);
    assert(bytes == CONTEXT_BYTES);
    primary_context_cancel_charge(&state);
    assert(primary_context_record_release(&state, &bytes) == 0);
    assert(bytes == 0);
}

static void test_rejects_unbalanced_release(void) {
    primary_context_accounting_t state = {0};
    size_t bytes = 0;

    errno = 0;
    assert(primary_context_record_release(&state, &bytes) == -1);
    assert(errno == EINVAL);
}

int main(void) {
    test_nested_lifetime();
    test_probe_is_not_charged();
    test_failed_add_is_not_removed();
    test_rejects_unbalanced_release();
    puts("context accounting tests passed");
    return 0;
}
