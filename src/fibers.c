/*
 * Fibers.
 *
 * Red Ninja runs its level loader and its script tasks on XAPI fibers
 * (CreateFiber 0x00212D58, DeleteFiber 0x00212DE4, SwitchToFiber 0x00212DF7,
 * about 700 call sites). SwitchToFiber saves the callee-saved registers on the
 * current stack, loads another fiber's stack pointer, pops that fiber's
 * registers and returns -- into whatever called SwitchToFiber on *that*
 * fiber.
 *
 * A lifted function cannot do that: its `ret` is a C return, to its own C
 * caller. Lifted as it stands, the guest stack pointer changed and the host
 * call stack did not, so the caller went on running with another fiber's
 * stack; the first casualty was the level loader, which the player saw as
 * "There's a problem with the disc you're using".
 *
 * So every guest fiber gets a host fiber, and SwitchToFiber switches both.
 * The guest side is done exactly as the original does it (same pushes, same
 * pops, same fields), so anything that inspects a fiber's stack still finds
 * what it expects.
 *
 * tools/postprocess_gen.py renames the generated sub_00212DF7 out of the way
 * and plants the create/delete hooks.
 */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_funcs.h"
#include <windows.h>
#include <stdio.h>

#define TLS_INDEX_VA      0x006702A0u   /* _tls_index */
#define TLS_CUR_FIBER     0x10u         /* XAPI: current fiber, in the TLS block */
#define FIBER_ESP         0x0Cu         /* guest fiber: saved stack pointer */
#define HOST_STACK_COMMIT (64u * 1024u)
#define HOST_STACK_RESERVE (2u * 1024u * 1024u)

#define MAX_FIBERS 1024
typedef struct { uint32_t guest; void *host; } FiberSlot;
static FiberSlot s_fibers[MAX_FIBERS];
static SRWLOCK s_lock = SRWLOCK_INIT;
static volatile LONG s_switches, s_starts, s_returns;
static volatile LONG s_to[MAX_FIBERS];       /* switches into slot i */

void rn_fiber_report(void)
{
    int i, shown = 0;
    fprintf(stderr, "[FIBER] switches %ld, started %ld, returned %ld:",
            s_switches, s_starts, s_returns);
    AcquireSRWLockShared(&s_lock);
    for (i = 0; i < MAX_FIBERS && shown < 12; i++)
        if (s_fibers[i].guest) {
            fprintf(stderr, " %08X=%ld", s_fibers[i].guest, s_to[i]);
            shown++;
        }
    ReleaseSRWLockShared(&s_lock);
    fprintf(stderr, "\n");
}

static void count_to(uint32_t guest)
{
    int i;
    for (i = 0; i < MAX_FIBERS; i++)
        if (s_fibers[i].guest == guest) { InterlockedIncrement(&s_to[i]); return; }
}

static void *map_get(uint32_t guest)
{
    void *h = NULL;
    int i;
    AcquireSRWLockShared(&s_lock);
    for (i = 0; i < MAX_FIBERS; i++)
        if (s_fibers[i].guest == guest) { h = s_fibers[i].host; break; }
    ReleaseSRWLockShared(&s_lock);
    return h;
}

static void map_set(uint32_t guest, void *host)
{
    int i, free_i = -1;
    AcquireSRWLockExclusive(&s_lock);
    for (i = 0; i < MAX_FIBERS; i++) {
        if (s_fibers[i].guest == guest) { free_i = i; break; }
        if (free_i < 0 && !s_fibers[i].guest) free_i = i;
    }
    if (free_i >= 0) {
        if (s_fibers[free_i].guest != guest) s_to[free_i] = 0;
        s_fibers[free_i].guest = host ? guest : 0;
        s_fibers[free_i].host = host;
    } else {
        fprintf(stderr, "[FIBER] table full\n");
    }
    ReleaseSRWLockExclusive(&s_lock);
}

static uint32_t tls_block(void)
{
    return MEM32(MEM32(XBOX_FS_BASE + 4) + MEM32(TLS_INDEX_VA) * 4);
}

/* The half of SwitchToFiber that runs on the fiber being switched TO: take
 * its stack, make it current, pop what its own switch pushed. Leaves esp at
 * the return address. */
static void guest_resume(uint32_t fiber)
{
    uint32_t tmp, r_ebp;
    esp = MEM32(fiber + FIBER_ESP);
    MEM32(tls_block() + TLS_CUR_FIBER) = fiber;
    POP32(esp, tmp); MEM32(XBOX_FS_BASE) = tmp;
    POP32(esp, ebx);
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, r_ebp);
    g_ebp = r_ebp; g_seh_ebp = r_ebp;
}

/* A fiber that has never run: its stack was laid out by CreateFiber so that
 * the first switch "returns" into BaseFiberStart(start) with a call frame
 * already in place. */
static VOID CALLBACK fiber_entry(LPVOID param)
{
    uint32_t fiber = (uint32_t)(uintptr_t)param, ra;
    recomp_func_t fn;

    InterlockedIncrement(&s_starts);
    guest_resume(fiber);
    ra = MEM32(esp);
    esp += 8;                                   /* ret 4 */
    fn = recomp_lookup(ra);
    if (fn)
        fn();
    else
        fprintf(stderr, "[FIBER] %08X starts at unknown code %08X\n", fiber, ra);
    /* BaseFiberStart does not return (it ends the thread). If it does, there
     * is nowhere to go: a fiber function that returns ends its thread. */
    InterlockedIncrement(&s_returns);
    fprintf(stderr, "[FIBER] %08X returned from its start routine\n", fiber);
    fflush(stderr);
    for (;;)
        Sleep(1000);
}

/* SwitchToFiber(fiber) */
void sub_00212DF7(void)
{
    uint32_t target = MEM32(esp + 4);
    uint32_t cur = MEM32(tls_block() + TLS_CUR_FIBER);
    void *host_cur, *host_tgt;
    static LONG shown;

    /* Guest side, the outgoing half: as the original. */
    PUSH32(esp, g_seh_ebp);
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    PUSH32(esp, ebx);
    PUSH32(esp, MEM32(XBOX_FS_BASE));
    MEM32(cur + FIBER_ESP) = esp;

    if (target == cur) {                        /* to itself: nothing moves */
        guest_resume(cur);
        esp += 8;
        return;
    }

    host_cur = IsThreadAFiber() ? GetCurrentFiber()
                                : ConvertThreadToFiber(NULL);
    if (map_get(cur) != host_cur)
        map_set(cur, host_cur);

    host_tgt = map_get(target);
    if (!host_tgt) {
        host_tgt = CreateFiberEx(HOST_STACK_COMMIT, HOST_STACK_RESERVE, 0,
                                 fiber_entry, (LPVOID)(uintptr_t)target);
        if (!host_tgt) {
            fprintf(stderr, "[FIBER] CreateFiberEx failed (%lu)\n", GetLastError());
            fflush(stderr);
            guest_resume(cur);
            esp += 8;
            return;
        }
        map_set(target, host_tgt);
        if (InterlockedIncrement(&shown) <= 8) {
            fprintf(stderr, "[FIBER] guest fiber %08X -> host fiber %p (from %08X)\n",
                    target, host_tgt, cur);
            fflush(stderr);
        }
    }

    InterlockedIncrement(&s_switches);
    count_to(target);
    SwitchToFiber(host_tgt);

    /* Back: some fiber switched to `cur`. Its outgoing half is done; this is
     * our incoming half. */
    guest_resume(cur);
    esp += 8;                                   /* ret 4 */
}

/* CreateFiber returned `fiber`: a stale host fiber for a recycled address
 * must not be resumed in its place. */
void rn_fiber_created(uint32_t fiber)
{
    void *old;
    if (!fiber)
        return;
    old = map_get(fiber);
    if (old) {
        map_set(fiber, NULL);
        if (old != GetCurrentFiber())
            DeleteFiber(old);
    }
}

/* DeleteFiber(fiber) is about to free the guest stack. */
void rn_fiber_deleted(uint32_t fiber)
{
    void *old = map_get(fiber);
    if (!old)
        return;
    map_set(fiber, NULL);
    if (!IsThreadAFiber() || old != GetCurrentFiber())
        DeleteFiber(old);
}
