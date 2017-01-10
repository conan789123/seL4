/*
 * Copyright 2016, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(D61_GPL)
 */

#ifndef __ARCH_MODE_FASTPATH_FASTPATH_H_
#define __ARCH_MODE_FASTPATH_FASTPATH_H_

#include <util.h>
#include <arch/linker.h>
#include <api/types.h>
#include <api/syscall.h>
#include <plat/machine/hardware.h>

/* seL4 is always in the top of memory, so the high bits of pointers are always 1.
   The autogenerated unpacking code doesn't know that, however, so will try to
   conditionally sign extend (in 64-bit mode), which wastes cycles in the fast
   path. Instead, we can do the unpacking ourselves and explicitly set the high
   bits. */

static inline tcb_t *
endpoint_ptr_get_epQueue_tail_fp(endpoint_t *ep_ptr)
{
    uint64_t ret = ep_ptr->words[0] & 0xfffffffffffcull;
    return unlikely(ret) ? TCB_PTR(ret | PPTR_BASE) : NULL;
}

static inline vspace_root_t *
cap_vtable_cap_get_vspace_root_fp(cap_t vtable_cap)
{
    return PML4E_PTR(vtable_cap.words[1]);
}

static inline word_t
cap_pml4_cap_get_capPML4MappedASID_fp(cap_t vtable_cap)
{
    return (uint32_t)vtable_cap.words[0];
}

static inline void FORCE_INLINE
switchToThread_fp(tcb_t *thread, vspace_root_t *vroot, pde_t stored_hw_asid)
{
    word_t new_vroot = pptr_to_paddr(vroot);
    /* the asid is the 12-bit PCID */
    asid_t asid = (asid_t)(stored_hw_asid.words[0] & 0xfff);
    if (likely(getCurrentCR3().words[0] != cr3_new(new_vroot, asid).words[0])) {
        SMP_COND_STATEMENT(tlb_bitmap_set(vroot, getCurrentCPUIndex());)
        setCurrentVSpaceRoot(new_vroot, asid);
    }

#if CONFIG_MAX_NUM_NODES > 1
    asm volatile("movq %[value], %%gs:%c[offset]"
                 :
                 : [value] "r" (&thread->tcbArch.tcbContext.registers[Error + 1]),
                 [offset] "i" (OFFSETOF(nodeInfo_t, currentThreadUserContext)));
#endif

    NODE_STATE(ksCurThread) = thread;
}

static inline void
thread_state_ptr_set_blockingIPCDiminish_np(thread_state_t *ts_ptr, word_t dim)
{
    ts_ptr->words[1] = (ts_ptr->words[1] & 1) | dim;
}

static inline void
mdb_node_ptr_mset_mdbNext_mdbRevocable_mdbFirstBadged(
    mdb_node_t *node_ptr, word_t mdbNext,
    word_t mdbRevocable, word_t mdbFirstBadged)
{
    node_ptr->words[1] = mdbNext | (mdbRevocable << 1) | mdbFirstBadged;
}

static inline void
mdb_node_ptr_set_mdbPrev_np(mdb_node_t *node_ptr, word_t mdbPrev)
{
    node_ptr->words[0] = mdbPrev;
}

static inline bool_t
isValidVTableRoot_fp(cap_t vspace_root_cap)
{
    /* Check the cap is a pml4_cap, and that it is mapped. The fields are next
       to each other, so they can be read and checked in parallel */
    return (vspace_root_cap.words[0] >> (64 - 6)) == ((cap_pml4_cap << 1) | 0x1);
}

static inline void
fastpath_copy_mrs(word_t length, tcb_t *src, tcb_t *dest)
{
    word_t i;
    register_t reg;

    /* assuming that length < n_msgRegisters */
    for (i = 0; i < length; i ++) {
        /* assuming that the message registers simply increment */
        reg = msgRegisters[0] + i;
        setRegister(dest, reg, getRegister(src, reg));
    }
}

/* This is an accelerated check that msgLength, which appears
   in the bottom of the msgInfo word, is <= 4 and that msgExtraCaps
   which appears above it is zero. We are assuming that n_msgRegisters == 4
   for this check to be useful. By masking out the bottom 3 bits, we are
   really checking that n + 3 <= MASK(3), i.e. n + 3 <= 7 or n <= 4. */
compile_assert (n_msgRegisters_eq_4, n_msgRegisters == 4)
static inline int
fastpath_mi_check(word_t msgInfo)
{
    return ((msgInfo & MASK(seL4_MsgLengthBits + seL4_MsgExtraCapBits))
            + 3) & ~MASK(3);
}

static inline void NORETURN FORCE_INLINE
fastpath_restore(word_t badge, word_t msgInfo, tcb_t *cur_thread)
{
    if (config_set(CONFIG_SYSENTER) && config_set(CONFIG_HARDWARE_DEBUG_API) && ((getRegister(NODE_STATE(ksCurThread), FLAGS) & FLAGS_TF) != 0)) {
        /* If single stepping using sysenter we need to do a return using iret to avoid
         * a race condition in restoring the flags (which enables stepping and interrupts) and
         * calling sysexit. This case is handled in restore_user_context so we just go there
         */
        restore_user_context();
    }
    NODE_UNLOCK;
    c_exit_hook();
    lazyFPURestore(cur_thread);

#ifdef CONFIG_HARDWARE_DEBUG_API
    restore_user_debug_context(cur_thread);
#endif

#if CONFIG_MAX_NUM_NODES > 1
    cpu_id_t cpu = getCurrentCPUIndex();
    swapgs();
#endif
    /* Now that we have swapped back to the user gs we can safely
     * update the GS base. We must *not* use any kernel functions
     * that rely on having a kernel GS though. Most notably uses
     * of NODE_STATE etc cannot be used beyond this point */
    word_t base = getRegister(cur_thread, TLS_BASE);
    x86_write_fs_base(base, SMP_TERNARY(cpu, 0));

    base = cur_thread->tcbIPCBuffer;
    x86_write_gs_base(base, SMP_TERNARY(cpu, 0));

    if (config_set(CONFIG_SYSENTER)) {
        cur_thread->tcbArch.tcbContext.registers[FLAGS] &= ~FLAGS_IF;

        asm volatile (
            "movq %%rcx, %%rsp\n"
            "popq %%rax\n"
            "popq %%rbx\n"
            "popq %%rbp\n"
            "popq %%r12\n"
            "popq %%r13\n"
            "popq %%r14\n"
            // Skip RDX, we need to put NextIP into it
            "addq $8, %%rsp\n"
            "popq %%r10\n"
            "popq %%r8\n"
            "popq %%r9\n"
            "popq %%r15\n"
            // restore RFLAGS
            "popfq\n"
            // reset interrupt bit
            "orq %[IF], -8(%%rsp)\n"
            // Restore NextIP
            "popq %%rdx\n"
            // skip Error
            "addq $8, %%rsp\n"
            // restore RSP
            "popq %%rcx\n"
            // Skip TLS_BASE FaultIP
            "addq $16, %%rsp\n"
            "popq %%r11\n"
            "sti\n"
            "rex.w sysexit\n"
            :
            : "c" (&cur_thread->tcbArch.tcbContext.registers[RAX]),
            "D" (badge),
            "S" (msgInfo),
            [IF] "i" (FLAGS_IF)
            : "memory"
        );
    } else {
        asm volatile(
            // Set our stack pointer to the top of the tcb so we can efficiently pop
            "movq %0, %%rsp\n"
            "popq %%rax\n"
            "popq %%rbx\n"
            "popq %%rbp\n"
            "popq %%r12\n"
            "popq %%r13\n"
            "popq %%r14\n"
            "popq %%rdx\n"
            "popq %%r10\n"
            "popq %%r8\n"
            "popq %%r9\n"
            "popq %%r15\n"
            //restore RFLAGS
            "popq %%r11\n"
            // Restore NextIP
            "popq %%rcx\n"
            // clear RSP to not leak information to the user
            "xor %%rsp, %%rsp\n"
            // More register but we can ignore and are done restoring
            // enable interrupt disabled by sysenter
            "rex.w sysret\n"
            :
            : "r"(&cur_thread->tcbArch.tcbContext.registers[RAX]),
            "D" (badge),
            "S" (msgInfo)
            : "memory"
        );
    }
    UNREACHABLE();
}

#endif /* __ARCH_MODE_FASTPATH_FASTPATH_H_ */
