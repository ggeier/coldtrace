/*
 * Copyright (C) 2025 Huawei Technologies Co., Ltd.
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <coldtrace/config.h>
#include <coldtrace/thread.h>
#include <coldtrace/writer.h>
#include <dice/events/pthread.h>
#include <dice/log.h>
#include <dice/mempool.h>
#include <dice/module.h>
#include <dice/pubsub.h>
#include <dice/self.h>
#include <stdint.h>
#include <string.h>
#include <vsync/atomic.h>

struct coldtrace_stack {
    uint64_t *data;
    uint32_t size;
    uint32_t capacity;
};

struct coldtrace_thread {
    struct coldtrace_writer writer;
    bool initd;
    uint32_t stack_bottom;
    struct coldtrace_stack stack;
    uint64_t created_thread_idx;
};

static struct coldtrace_thread tls_key_;

static void
coldtrace_stack_init_(struct coldtrace_stack *stack)
{
    stack->data     = NULL;
    stack->size     = 0;
    stack->capacity = 0;
}

static void
coldtrace_stack_fini_(struct coldtrace_stack *stack)
{
    if (stack->data != NULL) {
        mempool_free(stack->data);
    }
    coldtrace_stack_init_(stack);
}

static void
coldtrace_stack_reserve_(struct coldtrace_stack *stack, uint32_t needed)
{
    if (needed <= stack->capacity) {
        return;
    }

    uint32_t new_capacity = stack->capacity == 0 ? 16 : stack->capacity;
    while (new_capacity < needed) {
        if (new_capacity > UINT32_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }

    uint64_t *new_data =
        mempool_alloc((size_t)new_capacity * sizeof(*new_data));
    if (new_data == NULL) {
        log_fatal("error: Could not allocate thread stack");
    }

    if (stack->data != NULL) {
        memcpy(new_data, stack->data, (size_t)stack->size * sizeof(*new_data));
        mempool_free(stack->data);
    }

    stack->data     = new_data;
    stack->capacity = new_capacity;
}

static void
coldtrace_stack_push_(struct coldtrace_stack *stack, void *caller)
{
    if (stack->size == UINT32_MAX) {
        log_fatal("error: Thread stack is full");
    }
    coldtrace_stack_reserve_(stack, stack->size + 1);
    stack->data[stack->size] = (uint64_t)(uintptr_t)caller;
    stack->size++;
}

static void
coldtrace_stack_pop_(struct coldtrace_stack *stack)
{
    assert(stack->size > 0);
    stack->size--;
}

static void
coldtrace_thread_init_(struct coldtrace_thread *th, metadata_t *md)
{
    coldtrace_writer_init(&th->writer, md);
    coldtrace_stack_init_(&th->stack);
    th->stack_bottom       = 0;
    th->created_thread_idx = 0;
    th->initd              = true;
}

static inline struct coldtrace_thread *
get_coldtrace_thread(metadata_t *md)
{
    struct coldtrace_thread *th = SELF_TLS(md, &tls_key_);
    if (!th->initd) {
        coldtrace_thread_init_(th, md);
    }
    return th;
}

static inline bool
with_stack_(coldtrace_entry_type type)
{
    switch (type & ~ZERO_FLAG) {
        case COLDTRACE_FREE:
        case COLDTRACE_ALLOC:
        case COLDTRACE_MMAP:
        case COLDTRACE_MUNMAP:
        case COLDTRACE_READ:
        case COLDTRACE_WRITE:
            return true;
        default:
            return false;
    }
}

DICE_HIDE void *
coldtrace_thread_append(struct metadata *md, coldtrace_entry_type type,
                        const void *ptr)
{
    struct coldtrace_thread *th = get_coldtrace_thread(md);
    uint64_t len                = coldtrace_entry_fixed_size(type);
    if (!with_stack_(type)) {
        struct coldtrace_entry_header *entry =
            (struct coldtrace_entry_header *)coldtrace_writer_reserve(
                &th->writer, len);
        *entry = coldtrace_entry_init(type, ptr);
        return entry;
    }

    struct coldtrace_stack *stack = &th->stack;
    uint32_t stack_bot            = th->stack_bottom;
    uint32_t stack_top            = stack->size;
    size_t stack_size = (size_t)(stack_top - stack_bot) * sizeof(uint64_t);
    void *e           = coldtrace_writer_reserve(&th->writer, len + stack_size);
    if (e == NULL)
        log_fatal("error: Could not reserve entry in writer");

    struct coldtrace_entry_header *entry = (struct coldtrace_entry_header *)e;
    *entry                               = coldtrace_entry_init(type, ptr);

    char *buf = (char *)e + len - sizeof(struct coldtrace_stack_diff);
    struct coldtrace_stack_diff *s = (struct coldtrace_stack_diff *)buf;
    s->depth                       = stack_top;
    s->popped                      = stack_bot;
    if (stack_size != 0) {
        memcpy(s->diff, stack->data + stack_bot, stack_size);
    }

    th->stack_bottom = stack_top;
    return e;
}

DICE_HIDE void
coldtrace_thread_init(struct metadata *md)
{
    struct coldtrace_thread *th = SELF_TLS(md, &tls_key_);
    if (!th->initd) {
        coldtrace_thread_init_(th, md);
    } else {
        coldtrace_writer_init(&th->writer, md);
    }
}

DICE_HIDE void
coldtrace_thread_fini(struct metadata *md)
{
    struct coldtrace_thread *th = SELF_TLS(md, &tls_key_);
    if (!th->initd)
        return;
    coldtrace_writer_fini(&th->writer);
    coldtrace_stack_fini_(&th->stack);
    th->stack_bottom = 0;
    th->initd        = false;
}

DICE_HIDE void
coldtrace_thread_set_create_idx(struct metadata *md, uint64_t idx)
{
    struct coldtrace_thread *th = get_coldtrace_thread(md);
    th->created_thread_idx      = idx;
}

DICE_HIDE uint64_t
coldtrace_thread_get_create_idx(struct metadata *md)
{
    struct coldtrace_thread *th = get_coldtrace_thread(md);
    return th->created_thread_idx;
}

DICE_HIDE void
coldtrace_thread_stack_push(struct metadata *md, void *caller)
{
    struct coldtrace_thread *th = get_coldtrace_thread(md);
    coldtrace_stack_push_(&th->stack, caller);
}

DICE_HIDE void
coldtrace_thread_stack_pop(struct metadata *md)
{
    struct coldtrace_thread *th = get_coldtrace_thread(md);
    if (th->stack.size != 0) {
        coldtrace_stack_pop_(&th->stack);
        if (th->stack_bottom > th->stack.size)
            th->stack_bottom = th->stack.size;
    }
}

__attribute__((weak)) void
coldtrace_main_thread_fini()
{
}
