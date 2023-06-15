#include <errno.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <mach/mach.h>


#include <ruby/ruby.h>
#include <ruby/thread.h>

int __ulock_wait(uint32_t op, void *addr, uint64_t val, uint32_t timeout_us);
int __ulock_wake(uint32_t op, void *addr, uint64_t val);
const uint32_t UL_COMPARE_AND_WAIT_SHARED = 3;


static VALUE mProcessShared;
static VALUE cMemorySegment;
static VALUE cMonitor;

static ID id_size;

// ******* ProcessShared::MemorySegment ********

struct memory_segment_s {
    char *mem;
    size_t sz;
    size_t alloc_index;
};

static void memory_segment_gc_mark(void *ptr) {

}

static void memory_segment_gc_free(void *ptr) {
    struct memory_segment_s *memory_segment = (struct memory_segment_s *)ptr;
    if (memory_segment->mem != MAP_FAILED) {
        (void)munmap(memory_segment->mem, memory_segment->sz);
    }
    ruby_xfree(memory_segment);
}

static size_t memory_segment_gc_memsize(const void *ptr) {
    return 0;
}

static void memory_segment_gc_compact(void *ptr) {

}

static const rb_data_type_t memory_segment_rbtype = {
    .wrap_struct_name = "ProcessShared::MemorySegment",
    .function = {
        .dmark = memory_segment_gc_mark,
        .dfree = memory_segment_gc_free,
        .dsize = memory_segment_gc_memsize,
        .dcompact = memory_segment_gc_compact,
        .reserved = { 0 },
    },
    .parent = NULL,
    .data = NULL,
    .flags = RUBY_TYPED_WB_PROTECTED,
};

static VALUE memory_segment_alloc(VALUE klass) {
    struct memory_segment_s *memory_segment;
    VALUE obj = TypedData_Make_Struct(klass, struct memory_segment_s, &memory_segment_rbtype, memory_segment);
    memory_segment->mem = MAP_FAILED;
    memory_segment->sz = 0;
    memory_segment->alloc_index = 0;
    return obj;
};

static VALUE memory_segment_initialize(int argc, VALUE *argv, VALUE self) {
    struct memory_segment_s *memory_segment;
    TypedData_Get_Struct(self, struct memory_segment_s, &memory_segment_rbtype, memory_segment);

    // Parse keyword arguments
    VALUE kwargs_hash = Qnil;
    rb_scan_args(argc, argv, "00:", &kwargs_hash);
    ID kwarg_keys[1] = { id_size };
    VALUE kwarg_values[1];
    rb_get_kwargs(kwargs_hash, kwarg_keys, 0, 1, kwarg_values);

    if (kwarg_values[0] == Qundef) {
        // default size
        kwarg_values[0] = RB_INT2NUM(4096);
    }

    // Actually allocate the shared memory block
    memory_segment->sz = RB_NUM2SIZE(kwarg_values[0]);
    memory_segment->mem = (char *)mmap(NULL, memory_segment->sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory_segment->mem == MAP_FAILED) {
        rb_sys_fail("mmap(2)");
    }

    return Qnil;
}

static char *memory_segment_bump_allocate(struct memory_segment_s *memory_segment, size_t bytes, size_t align) {
    size_t padding = 0;
    if (memory_segment->alloc_index % align != 0) {
        padding = align - (memory_segment->alloc_index % align);
    }
    size_t alloc_size_required = bytes + padding;
    if (memory_segment->alloc_index + alloc_size_required > memory_segment->sz) {
        rb_raise(rb_eRangeError, "Segment ran out of memory to satisfy request for %ld bytes", bytes);
    }
    memory_segment->alloc_index += padding;
    char *r = memory_segment->mem + memory_segment->alloc_index;
    memory_segment->alloc_index += bytes;
    return r;
}

// ******* ProcessShared::Monitor ********

struct monitor_s {
    VALUE memory_segment_v;
    _Atomic uint64_t *ulock_word;
};

static const uint64_t PS_LOCK_WORD_HELD = 0x1u;
static const uint64_t PS_LOCK_WORD_WAITING = 0x2u;
// static const uint64_t PS_LOCK_WORD_WAKEALL = 0x4u;
// static const uint64_t PS_LOCK_WORD_PID = 0xFFFFFFFF00000000u;

/*
 * The layout of the ulock_word is as follows, from lowest-order to highest-order bit:
 * Bit 0: Set if the lock is held by somebody, 0 otherwise (PS_LOCK_WORD_HELD)
 * Bit 1: Set if there is somebody waiting for the lock, 0 otherwise (PS_LOCK_WORD_WAITING)
 * Bit 2: This is never actually set in the word, but we might call a futex wake with it set
 *        to force wake up _all_ lock waiters, and make them e.g. check their cancelation status.
 * Bits 3-31: All set to zero
 * Bits 32-63: The process ID of the process that holds the lock
 *
 * Only the first 32-bits of the futex are actually waited on in the kernel, but all 64 bits are
 * atomically set in userspace.
*/

static void monitor_gc_mark(void *ptr) {
    struct monitor_s *monitor = (struct monitor_s *)ptr;
    rb_gc_mark_movable(monitor->memory_segment_v);
}

static void monitor_gc_free(void *ptr) {
    struct monitor_s *monitor = (struct monitor_s *)ptr;
    ruby_xfree(monitor);
}

static size_t monitor_gc_memsize(const void *ptr) {
    return 0;
}

static void monitor_gc_compact(void *ptr) {
    struct monitor_s *monitor = (struct monitor_s *)ptr;
    monitor->memory_segment_v = rb_gc_location(monitor->memory_segment_v);
}

static const rb_data_type_t monitor_rbtype = {
    .wrap_struct_name = "ProcessShared::Monitor",
    .function = {
        .dmark = monitor_gc_mark,
        .dfree = monitor_gc_free,
        .dsize = monitor_gc_memsize,
        .dcompact = monitor_gc_compact,
        .reserved = { 0 },
    },
    .parent = NULL,
    .data = NULL,
    .flags = RUBY_TYPED_WB_PROTECTED,
};

static VALUE monitor_alloc(VALUE klass) {
    struct monitor_s *monitor;
    VALUE obj = TypedData_Make_Struct(klass, struct monitor_s, &monitor_rbtype, monitor);
    RB_OBJ_WRITE(obj, &monitor->memory_segment_v, Qnil);
    return obj;
};

static VALUE monitor_initialize(int argc, VALUE *argv, VALUE self) {
    struct monitor_s *monitor;
    TypedData_Get_Struct(self, struct monitor_s, &monitor_rbtype, monitor);

    // Parse arguments
    VALUE memory_segment_v = Qnil;
    rb_scan_args(argc, argv, "10", &memory_segment_v);
    RB_OBJ_WRITE(self, &monitor->memory_segment_v, memory_segment_v);

    struct memory_segment_s *memory_segment;
    TypedData_Get_Struct(memory_segment_v, struct memory_segment_s, &memory_segment_rbtype, memory_segment);

    monitor->ulock_word = (_Atomic uint64_t *)memory_segment_bump_allocate(memory_segment, sizeof(uint64_t), alignof(uint64_t));

    return Qnil;
}

struct do_futex_wait_nogvl_args {
    struct monitor_s *monitor;
    uint64_t old_lock_val;
    int ret;
    int ret_errno;
};

static void *do_futex_wait_nogvl(void *ctx) {
    struct do_futex_wait_nogvl_args *args = (struct do_futex_wait_nogvl_args *)ctx;
    args->ret_errno = 0;
    args->ret = __ulock_wait(UL_COMPARE_AND_WAIT_SHARED, args->monitor->ulock_word, args->old_lock_val & 0xFFFFFFFF, 0);
    if (args->ret == -1) {
        args->ret_errno = errno;
    }
    return NULL;
}

static VALUE monitor_lock(VALUE self) {
    struct monitor_s *monitor;
    TypedData_Get_Struct(self, struct monitor_s, &monitor_rbtype, monitor);
    pid_t pid = getpid();

    /*
     * I think this is a fairly faithful interpretation of
     * https://github.com/eliben/code-for-blog/blob/master/2018/futex-basics/mutex-using-futex.cpp
     * but adapted for multi-process & some stuff we need in the Ruby VM.
     */

    /*
    VALUE thread_id_num = rb_funcall(rb_thread_current(), rb_intern("native_thread_id"), 0);
    int thread_id = RB_NUM2INT(thread_id_num);
    */

    // Uncontended case - can we flat-out acquire the mutex?
    uint64_t new_lock_val = (PS_LOCK_WORD_HELD) | (((uint64_t)pid) << 32);
    uint64_t old_lock_val = 0;
    bool did_acquire = atomic_compare_exchange_strong(monitor->ulock_word, &old_lock_val, new_lock_val);
    while (!did_acquire) {
        // Lock might be contended.
        bool needs_sleep = false;
        if (old_lock_val & PS_LOCK_WORD_WAITING) {
            // Someone has already marked the mutex as contended. We need to sleep
            needs_sleep = true;
        } else if (old_lock_val & PS_LOCK_WORD_HELD) {
            // Looks like we might be the first to mark the mutex as contended.
            new_lock_val = old_lock_val | PS_LOCK_WORD_WAITING;
            bool did_swap = atomic_compare_exchange_strong(monitor->ulock_word, &old_lock_val, new_lock_val);
            if (did_swap && (old_lock_val | PS_LOCK_WORD_HELD)) {
                // Lock is still currently held - need to sleep
                needs_sleep = true;
            }
        }
        if (needs_sleep) {
            struct do_futex_wait_nogvl_args futex_wait_args = {
                .monitor = monitor,
                .old_lock_val = old_lock_val,
            };
            rb_thread_call_without_gvl(do_futex_wait_nogvl, &futex_wait_args, RUBY_UBF_IO, 0);
            if (futex_wait_args.ret == -1) {
                rb_raise(rb_eStandardError, "futex wait failed (ret %d errno %d)", futex_wait_args.ret, futex_wait_args.ret_errno);
            }
        }

        // See if we can acquire it now - we set the mutex to still-contended because we don't actually
        // _know_ if there are no more waiters.
        old_lock_val = 0;
        new_lock_val = (PS_LOCK_WORD_HELD | PS_LOCK_WORD_WAITING) | (((uint64_t)pid) << 32);
        did_acquire = atomic_compare_exchange_strong(monitor->ulock_word, &old_lock_val, new_lock_val);
    }

    // Lock is held here.
    return Qnil;
}

static VALUE monitor_unlock(VALUE self) {
    struct monitor_s *monitor;
    TypedData_Get_Struct(self, struct monitor_s, &monitor_rbtype, monitor);

    uint64_t new_lock_val = 0;
    uint64_t old_lock_val = atomic_exchange(monitor->ulock_word, new_lock_val);
    if (old_lock_val & PS_LOCK_WORD_WAITING) {
        // Someone (might) care about being woken up here.
        // Also make sure we keep trying to wake even if we get interrupted.
        bool done_waking = false;
        while (!done_waking) {
            int r = __ulock_wake(UL_COMPARE_AND_WAIT_SHARED, monitor->ulock_word, 0);
            if (r == -1) {
                switch (errno) {
                case ENOENT:
                    // That's OK - just means there is nobody left to wake up.
                    done_waking = true;
                    break;
                case EINTR:
                    // Interrupted by a signal, retry.
                    break;
                default:
                    // oh-oh.
                    rb_raise(rb_eStandardError, "futex wake failed (ret %d errno %d)", r, errno);
                    __builtin_unreachable();
                }
            } else {
                // Wake succeeded.
                done_waking = true;
            }
        }
    }

    // Lock is now not held here.

    return Qnil;
}

static VALUE monitor_force_unlock_of(VALUE self, VALUE pid) {
    // old_value = atomic_load(thing)
    // if (old_value >> 32) == pid
    //     atomic_compare_exchange(old_value, 0)
    //     wake someone up.
    return Qnil;
}

void Init_process_shared_ext(void) {
    rb_ext_ractor_safe(true);

    id_size = rb_intern("size");

    mProcessShared = rb_define_module("ProcessShared");
    rb_global_variable(&mProcessShared);
    cMemorySegment = rb_define_class_under(mProcessShared, "MemorySegment", rb_cObject);
    rb_global_variable(&cMemorySegment);
    cMonitor = rb_define_class_under(mProcessShared, "Monitor", rb_cObject);
    rb_global_variable(&cMonitor);

    rb_define_alloc_func(cMemorySegment, memory_segment_alloc);
    rb_define_method(cMemorySegment, "initialize", memory_segment_initialize, -1);

    rb_define_alloc_func(cMonitor, monitor_alloc);
    rb_define_method(cMonitor, "initialize", monitor_initialize, -1);
    rb_define_method(cMonitor, "lock", monitor_lock, 0);
    rb_define_method(cMonitor, "unlock", monitor_unlock, 0);
    rb_define_method(cMonitor, "force_unlock_of", monitor_force_unlock_of, 1);
}
