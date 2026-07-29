#ifndef SRC_CUDA_CONTEXT_ACCOUNTING_H_
#define SRC_CUDA_CONTEXT_ACCOUNTING_H_

#include <stddef.h>

typedef struct {
    unsigned int retain_count;
    size_t charged_bytes;
} primary_context_accounting_t;

/*
 * Record a successful retain or release. The returned byte count is the
 * accounting delta for that transition: nonzero only on 0 -> 1 or 1 -> 0.
 */
int primary_context_record_retain(primary_context_accounting_t *state,
                                  size_t context_bytes,
                                  size_t *bytes_to_add);
int primary_context_record_release(primary_context_accounting_t *state,
                                   size_t *bytes_to_remove);

/* Undo only the accounting marker when adding the bytes itself failed. */
void primary_context_cancel_charge(primary_context_accounting_t *state);

#endif  // SRC_CUDA_CONTEXT_ACCOUNTING_H_
