/**
 * Red Ninja: End of Honor - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       Red Ninja: End of Honor
 *   Title ID:    0x56550027 (USA/Europe), XDK 5849
 *   Base addr:   0x00010000
 *   Entry point: 0x001F2EE2
 *   Code size:   ~??? KB (.text)
 *   Sections:    ?? (list them)
 *   Kernel imports: ??
 */

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include <io.h>
#include <shellapi.h>
#include "iso_setup.h"

/* ── Launcher helpers (Sims BO) ────────────────────────────── */

static char g_exe_dir[MAX_PATH];

static void set_default_env(const char *name, const char *value)
{
    if (!getenv(name)) {
        char kv[512];
        snprintf(kv, sizeof(kv), "%s=%s", name, value);
        _putenv(kv);
        SetEnvironmentVariableA(name, value);
    }
}

/* settings.ini beside the exe: one NAME=VALUE per line, '#' or ';' comments.
 * Values there override the built-in defaults below; real environment
 * variables override both. */
static void load_settings_ini(void)
{
    char path[MAX_PATH], line[512];
    FILE *f;
    snprintf(path, sizeof(path), "%s\\settings.ini", g_exe_dir);
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *eq, *e, *k = line;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '#' || *k == ';' || *k == '[' || !*k) continue;
        eq = strchr(k, '=');
        if (!eq) continue;
        *eq = 0;
        for (e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
        {
            char *v = eq + 1;
            while (*v == ' ' || *v == '\t') v++;
            for (e = v + strlen(v) - 1; e >= v && (*e == '\n' || *e == '\r' || *e == ' '); e--) *e = 0;
            set_default_env(k, v);
        }
    }
    fclose(f);
}

static void apply_title_defaults(void)
{
    /* D3D's EnableOverlay(FALSE) spins until NV_PVIDEO_BUFFER reads 0; the
     * toolkit clears it at vblank only when asked ("Working, please wait"
     * hang otherwise). */
    set_default_env("RECOMP_PVIDEO_VBLANK_CLEAR", "1");
    /* The title statically links the XDK's D3D8, so it talks to the NV2A
     * directly: the pushbuffer has to be executed and vblanks raised. */
    set_default_env("RECOMP_PB_EXEC", "1");
    set_default_env("RECOMP_VBLANK", "paced");
    set_default_env("RECOMP_FB_WINDOW", "1");
    /* Butterflies are drawn through a scanline stencil pattern (half
     * transparent on a television, stripes on a monitor): blend them. */
    set_default_env("RECOMP_GPU_STIPPLE_BLEND", "1");
    /* The logo and menu are drawn nine times over while they move, as an
     * afterimage: draw them once. */
    set_default_env("RECOMP_GPU_SPRITE_ECHO", "0");
    /* The F6 menu: only what applies to this title, in its colour. */
    set_default_env("RECOMP_MENU_HIDE", "move,camera,showfps,reload");
    set_default_env("RECOMP_MENU_THEME", "red");
    set_default_env("RN_WIRE_LOG", "1");      /* until the missing razor wire is understood */
    /* DirectSound is the XDK's own and probes the AC'97 codec. */
    set_default_env("RECOMP_AC97_READY", "1");
    set_default_env("RECOMP_TEX_BILINEAR", "1");
    /* Events the XDK builds inline (no KeInitializeEvent import). */
    set_default_env("RECOMP_TITLE_KEVENTS", "1");
    /* Give freed contiguous memory back: the title frees and reallocates its
     * 1280x960 render targets around every movie, and 64 MB does not hold
     * two sets. */
    if (!getenv("RN_NO_RECLAIM"))
        set_default_env("RECOMP_HEAP_RECLAIM", "1");
    /* D3D's GPU-time wait compares PGRAPH 0x400B10 with the semaphore. */
    set_default_env("RECOMP_PB_LATCH_0310", "1");
    /* Walk the pushbuffer as the GPU does: follow every JUMP from GET to PUT.
     * This title's D3D wraps its ring to the middle as often as to the
     * start, which the PUT-to-PUT segment scan could only guess at. */
    set_default_env("RECOMP_PB_WALK", "dma");
    /* Flips wait for the vblank their swap asked for, as on the console:
     * frame pacing comes from the flip, not from when a GPU callback lands. */
    set_default_env("RECOMP_FLIP_PACING", "1");
    /* Pads: the title's own XAPI drives the USB host controllers. The
     * keyboard doubles as pad 1. */
    set_default_env("RECOMP_USB", "1");
    set_default_env("RECOMP_KEYBOARD", "1");
    /* Contiguous blocks stay above the image: otherwise the USB host
     * controller's descriptors (physical 0x00408000) read back as the game's
     * own .data and no pad ever enumerates. */
    set_default_env("RECOMP_CONTIG_AVOID_IMAGE", "1");
    { extern int rn_cam_no_follow; const char *cf = getenv("RN_CAM_FOLLOW"); rn_cam_no_follow = !(cf && atoi(cf)); }
    /* One guest CPU at DISPATCH level: interrupts and DPCs are delivered at
     * safe points instead of beside whatever the guest is doing. Without it
     * the USB driver's completion DPC raced the host controller model: about
     * half of all runs froze on a logo, crashed in a copy with a negative
     * length, or came up with no pad. */
    set_default_env("RECOMP_DISPATCH_GATE", "1");
    {
        char saves[MAX_PATH];
        snprintf(saves, sizeof(saves), "%s\\saves", g_exe_dir);
        CreateDirectoryA(saves, NULL);
        set_default_env("RECOMP_SAVE_DIR", saves);
    }
}

/* Pull a disc-image path out of the command line (drag-and-drop onto the exe
 * passes it as the first argument). */
static const char *iso_from_cmdline(void)
{
    static char iso[MAX_PATH * 2];
    int argc = 0, i;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return NULL;
    for (i = 1; i < argc; i++) {
        size_t n = wcslen(argv[i]);
        if ((n > 4 && _wcsicmp(argv[i] + n - 4, L".iso") == 0) ||
            (n > 5 && _wcsicmp(argv[i] + n - 5, L".xiso") == 0)) {
            WideCharToMultiByte(CP_ACP, 0, argv[i], -1, iso, sizeof(iso), NULL, NULL);
            LocalFree(argv);
            return iso;
        }
    }
    LocalFree(argv);
    return NULL;
}

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS uint32_t g_mxcsr;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x001F2EE2  /* XBE entry point VA */
#define YOUR_GAME_XBE_PATH      "game\\default.xbe"
#define YOUR_GAME_DIR            "game"

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);
extern int recomp_dispatch_init(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched. The VEH below now reaches apu_hook_handle_mmio
 * through the toolkit's xbox_VehMmioFault.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* Trapped hardware registers (APU, USB OHCI) -> their models.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        if (xbox_VehMmioFault(ep))
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── Title-specific GPU glue ───────────────────────────────── */

/* The title links the XDK's D3D8 statically. Its device lives at a fixed
 * address (the pointer at 0x00231458 -> 0x00231460), and device+0x30 holds
 * the GPU semaphore: the word the NV2A writes with
 * BACK_END_WRITE_SEMAPHORE_RELEASE as it finishes work. D3D's make-space and
 * fence waits poll it. The pushbuffer executor writes those releases only
 * once it has been told where the word is, so a host thread waits for D3D to
 * create its device and then hands the address over. Without this the title
 * spins forever in its first make-space wait (D3D_BlockOnTime, sub_00223890). */
#define SIMS_D3D_DEVICE_PTR_VA  0x00231458u
#define SIMS_D3D_SEMAPHORE_OFF  0x30u

extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);

extern void xbox_DebugDumpThreads(uint32_t code_lo, uint32_t code_hi);

/* Called from the guest's GPU-wait loops (tools/postprocess_gen.py): give the
 * core to the pushbuffer executor, which is what the loop is waiting for. */
/* Multisample type for the title's 480-line modes (tools/postprocess_gen.py).
 * The title asks for 0x2222, 4x supersampling; drawn on the CPU that is four
 * times the work for a softer picture. SIMS_SUPERSAMPLE=1 keeps it. */
uint32_t sims_multisample(uint32_t requested)
{
    const char *e = getenv("SIMS_SUPERSAMPLE");
    if (e && *e == '1')
        return requested;
    return 0x11u;   /* D3DMULTISAMPLE_NONE */
}

/* The scale the title pairs with that choice: 2.0 under supersampling. */
float sims_ss_scale(unsigned site)
{
    const char *keep = getenv("SIMS_SSSCALE_ORIG");   /* hex site list, bring-up */
    char hex[16];
    snprintf(hex, sizeof hex, "%08X", site);
    if (keep && (strstr(keep, hex) || !strcmp(keep, "1"))) return 2.0f;
    /* 0x1293C0 derives something other than the viewport from it; scaled,
     * the title stalls on a black screen after the first movie. */
    if (site == 0x001293C0u) return 2.0f;
    return sims_multisample(0x2222) == 0x2222 ? 2.0f : 1.0f;
}

/* D3D's vblank handler acknowledging the vblank (tools/postprocess_gen.py):
 * PCRTC_INTR_0 and PMC_INTR_0 are write-1-to-clear on hardware, so the ack
 * takes both bits down at once. nv2a = the NV2A register base (0xFD000000). */
void sims_vblank_ack(uint32_t nv2a)
{
    uint8_t *mem = (uint8_t *)g_xbox_mem_offset;
    InterlockedAnd((volatile LONG *)(mem + nv2a + 0x600100u), ~1L);
    InterlockedAnd((volatile LONG *)(mem + nv2a + 0x000100u), ~(LONG)0x01000000L);
}

void sims_spin_yield(void)
{
    if (!SwitchToThread())
        Sleep(0);
}

/* SIMS_THREADDUMP=<seconds>: report every guest thread's latest kernel call
 * and guest call chain at that interval. A bring-up aid for hangs. */
static DWORD WINAPI sims_thread_dumper(LPVOID p)
{
    DWORD ms = (DWORD)(uintptr_t)p;
    for (;;) {
        Sleep(ms);
        xbox_DebugDumpThreads(0x00011000u, 0x002BA000u);
    }
    return 0;
}

/* Bring-up probe (RN_DEBUG_STATE=1): the game's state machine, every 2 s.
 * App singleton at [0x0069ACE8]; +0x4DF8 main state, +0x4DEC boot-load
 * context {step, status}, +0x4E18 resource file object. */
static void rn_debug_state(const uint8_t *mem)
{
    static int enabled = -1;
    static DWORD last;
    DWORD now = GetTickCount();
    uint32_t app, ctx, res;
#define RN32(va) (*(volatile const uint32_t *)(mem + (uint32_t)(va)))
#define RN_OK(p) ((p) >= 0x00010000u && (p) < 0x04000000u)

    if (enabled < 0)
        enabled = getenv("RN_DEBUG_STATE") != NULL;
    if (!enabled || now - last < 2000)
        return;
    last = now;
    app = RN32(0x0069ACE8);
    if (!RN_OK(app))
        return;
    ctx = RN32(app + 0x4DEC);
    res = RN32(app + 0x4E18);
    fprintf(stderr, "[RN] state=%u ctx=%08X", RN32(app + 0x4DF8), ctx);
    if (RN_OK(ctx))
        fprintf(stderr, " {step=%d status=%d %08X %08X}", (int)RN32(ctx),
                (int)RN32(ctx + 4), RN32(ctx + 8), RN32(ctx + 12));
    fprintf(stderr, " res=%08X", res);
    if (RN_OK(res))
        fprintf(stderr, " {+14=%08X +1C=%08X +20=%08X +28=%08X +49C=%08X "
                "+4A0=%u +4A4=%u +4A8=%u}", RN32(res + 0x14), RN32(res + 0x1C),
                RN32(res + 0x20), RN32(res + 0x28), RN32(res + 0x49C),
                RN32(res + 0x4A0), RN32(res + 0x4A4), RN32(res + 0x4A8));
    { extern void rn_fiber_report(void); rn_fiber_report(); }
    fprintf(stderr, " done=%u depth=%u lang=%u\n", RN32(0x00674390),
            RN32(0x0069AF78), RN32(0x002EEBC4));
#undef RN32
#undef RN_OK
}

/* Bring-up probes planted in the generated code by tools/postprocess_gen.py.
 * Each names one place the game gives up on its resource file, which the
 * player sees only as "There's a problem with the disc you're using". */
/* 1 = the camera does not swing round behind the player on its own
 * (tools/postprocess_gen.py, sub_000C0500). RN_CAM_FOLLOW=1 restores it. */
int rn_cam_no_follow = 1;

/* Second jump on demand (tools/postprocess_gen.py, sub_000B4110).
 * RN_DOUBLE_JUMP=0 turns it off. One per jump, and only on a fresh press:
 * rn_dj_frame (called every frame from rn_cam_shape) notes when the button
 * has been let go since the jump began. */
static int s_dj_on = -1, s_dj_released, s_dj_pending;
static uint32_t s_dj_act;
/* Fast run on a toggle: L3 (click the left stick) switches between the jog
 * and the fast run. RN_RUN_TOGGLE=0 restores the original build-up. */
static int s_run_toggle = -1, s_run_fast;
float rn_run_speed(float cur)
{
    if (s_run_toggle < 0) { const char *e = getenv("RN_RUN_TOGGLE"); s_run_toggle = !(e && !atoi(e)); }
    if (!s_run_toggle) return cur;
    return s_run_fast ? 1.6f : 1.0f;
}

/* Costume (F6 menu; tools/postprocess_gen.py, sub_0004E280): which of the
 * player's resource sets a stage loads. 0 as the story has it, 1 the red
 * kimono throughout, 2 the black suit throughout. Read when a stage loads. */
extern int xbox_MenuTitleOption(const char *label, const char *env, const char *const *values, int n, int def);
extern int xbox_MenuTitleValue(int option);
extern ptrdiff_t xbox_GetMemoryOffset(void);
static int s_costume_opt = -1;
static void rn_menu_register(void)
{
    static const char *const costumes[] = { "As in the story", "Red kimono", "Black suit", "Akemi (experimental)" };
    { extern void xbox_MenuPadAction(int game_button, const char *what);
      xbox_MenuPadAction(0, "jump"); xbox_MenuPadAction(1, "crouch");
      xbox_MenuPadAction(14, "fast run on/off"); }
    s_costume_opt = xbox_MenuTitleOption("Costume (next stage load)", "RN_COSTUME", costumes, 4, 0);
}

/* Akemi as the player. res.rbb is an index of zlib-packed entries: RIDX
 * holds (offset, packed size | flags) per entry and EXIX the unpacked size,
 * both kept in memory by the title. A resource set is a run of entries the
 * stage loader reads as one span of the file (sub_00013BA0), so an entry
 * cannot simply be pointed elsewhere. Akemi has a model of her own, the
 * CL01 set (mesh 2221, texture 2222, skeleton 2223), and her bones are a
 * subset of Kurenai's by name. For the swap each of Kurenai's four sets is
 * laid out again, with Akemi's mesh and body texture in place of hers, in a
 * region past the end of the file that only exists here: the index entries
 * of those sets are pointed into it and rn_rbb_read serves the reads from
 * the real file piece by piece. Kurenai's skeleton and motions stay
 * (RN_AKEMI_SKEL=1 swaps the skeleton too, to try it). */
static uint8_t *rn_find_table(const uint32_t *sig, size_t nsig)
{
    uint8_t *base = (uint8_t *)xbox_GetMemoryOffset();
    static const uint32_t span[2][2] = { { 0x00010000u, 0x04000000u }, { 0x80000000u, 0x84000000u } };
    int k;
    for (k = 0; k < 2; k++) {
        uint8_t *p = base + span[k][0], *end = base + span[k][1];
        while (p < end) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(p, &mi, sizeof mi)) break;
            if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                uint8_t *q = (uint8_t *)mi.BaseAddress, *qe = q + mi.RegionSize;
                if (q < p) q = p;
                if (qe > end) qe = end;
                for (; q + nsig * 4 <= qe; q += 4)
                    if (*(uint32_t *)q == sig[0] && !memcmp(q, sig, nsig * 4))
                        return q;
            }
            p = (uint8_t *)mi.BaseAddress + mi.RegionSize;
        }
    }
    return NULL;
}

extern void xbox_SetFileReadHook(int (*hook)(void *handle, uint64_t offset, void *buf, uint32_t len, uint32_t *got));
#define RN_VBASE 0x30000000u            /* the region, as an RIDX offset */
static struct { uint32_t voff, len, src; const uint8_t *mem; } s_vseg[1400];   /* mem: bytes held here, not in the file */
static int s_nvseg;
static uint32_t s_vend, s_rbb_data;     /* region end; file offset of RIDX offset 0 */
static HANDLE s_rbb = INVALID_HANDLE_VALUE, s_rbb_game;
static uint64_t s_rbb_size;

static int rn_rbb_open(void)
{
    char path[MAX_PATH];
    uint32_t o = 8;
    LARGE_INTEGER sz;
    if (s_rbb != INVALID_HANDLE_VALUE) return 1;
    snprintf(path, sizeof path, "%s\\%s\\media\\res.rbb", g_exe_dir, YOUR_GAME_DIR);
    s_rbb = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (s_rbb == INVALID_HANDLE_VALUE || !GetFileSizeEx(s_rbb, &sz)) return 0;
    s_rbb_size = (uint64_t)sz.QuadPart;
    while (o + 8 <= s_rbb_size) {       /* chunks: tag, size, data */
        uint32_t h[2]; DWORD got; OVERLAPPED ov;
        memset(&ov, 0, sizeof ov); ov.Offset = o;
        if (!ReadFile(s_rbb, h, 8, &got, &ov) || got != 8) break;
        if (h[0] == 0x30424252u) { s_rbb_data = o + 8; return 1; }      /* 'RBB0' */
        o += 8 + h[1];
    }
    return 0;
}

static int rn_rbb_read(void *handle, uint64_t offset, void *buf, uint32_t len, uint32_t *got)
{
    uint8_t *out = (uint8_t *)buf;
    uint64_t v0 = (uint64_t)s_rbb_data + RN_VBASE;
    uint32_t pos, done = 0;
    int i;
    if (!s_nvseg || offset < v0 || offset >= v0 + s_vend) return 0;
    if (handle != s_rbb_game) {         /* the title's handle on res.rbb, by size */
        LARGE_INTEGER sz;
        if (!GetFileSizeEx((HANDLE)handle, &sz) || (uint64_t)sz.QuadPart != s_rbb_size) return 0;
        s_rbb_game = (HANDLE)handle;
    }
    pos = (uint32_t)(offset - v0);
    memset(out, 0, len);
    for (i = 0; i < s_nvseg && done < len; i++) {
        uint32_t a = s_vseg[i].voff, e = a + s_vseg[i].len, from, n;
        DWORD rd; OVERLAPPED ov;
        if (e <= pos + done) continue;
        if (a >= pos + len) break;
        if (a > pos + done) done = a - pos;            /* padding between entries */
        from = pos + done - a;
        n = e - (pos + done);
        if (n > len - done) n = len - done;
        if (s_vseg[i].mem) memcpy(out + done, s_vseg[i].mem + from, n);
        else {
            memset(&ov, 0, sizeof ov); ov.Offset = s_rbb_data + s_vseg[i].src + from;
            if (!ReadFile(s_rbb, out + done, n, &rd, &ov)) return 0;
        }
        done += n;
    }
    *got = (pos + len <= s_vend) ? len : s_vend - pos;
    return 1;
}

/* 1: Akemi loads over the black-suit (dark) Kurenai sets. Her mesh is 57 KB
 * larger than the red Kurenai's and 13 KB larger packed, which pushed the
 * set past the buffer the title reads it into: the mines save froze and
 * loading a new area crashed in a corrupted heap. The dark Kurenai is the
 * larger of the two, so over her sets Akemi's comes out smaller than the
 * title expects. She looks the same either way: her own mesh and texture
 * replace whichever Kurenai is there. RN_AKEMI_SET=red to try the red. */
static int rn_akemi_dark(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RN_AKEMI_SET");
        v = !(e && (*e == 'r' || *e == 'R'));
    }
    return v;
}

static void rn_akemi_apply(int on)
{
    /* The first entries of each table, as in the file. */
    static const uint32_t ridx_sig[6] = { 0, 0x80006ECFu, 0x6ED0, 0x800010D8u, 0x7FB0, 0x800027C6u };
    static const uint32_t exix_sig[4] = { 0x23C00, 0x2C00, 0x5680, 0x47480 };
    static const uint16_t sets[4][2] = { { 202, 496 }, { 497, 756 }, { 1028, 1322 }, { 1323, 1578 } };
    const int first_set = rn_akemi_dark() ? 2 : 0;      /* only the pair she is loaded from */
    static const uint16_t swap[3][5] = {       /* Akemi's entry, then Kurenai's in her four sets */
        { 2221, 202, 497, 1028, 1323 },        /* body mesh */
        { 2222, 491, 749, 1317, 1573 },        /* body texture */
        { 2223, 212, 509, 1038, 1333 },        /* skeleton */
    };
    enum { LO = 202, HI = 1578, N = HI - LO + 1 };
    static uint32_t *ridx, *exix;
    static uint32_t keep_r[N][2], keep_x[N], ak_r[3][2], ak_x[3];
    static int saved, applied, skel = -1;
    static uint8_t *mesh;               /* mods\\akemi_mesh.bin: her mesh with the node the player code needs */
    static uint32_t mesh_len;
    int i, j, k, rows;
    if (skel < 0) { const char *e = getenv("RN_AKEMI_SKEL"); skel = e && atoi(e); }
    if (!on && !applied) return;
    if (!ridx) ridx = (uint32_t *)rn_find_table(ridx_sig, 6);
    if (!exix) exix = (uint32_t *)rn_find_table(exix_sig, 4);
    if (!ridx || !exix || !rn_rbb_open()) {
        printf("[RN] Akemi: resource index not found (RIDX %p, EXIX %p, file %d)\n",
               (void *)ridx, (void *)exix, s_rbb != INVALID_HANDLE_VALUE);
        return;
    }
    if (!saved) {
        for (i = 0; i < N; i++) {
            keep_r[i][0] = ridx[(LO + i) * 2]; keep_r[i][1] = ridx[(LO + i) * 2 + 1];
            keep_x[i] = exix[LO + i];
        }
        for (i = 0; i < 3; i++) {
            ak_r[i][0] = ridx[swap[i][0] * 2]; ak_r[i][1] = ridx[swap[i][0] * 2 + 1];
            ak_x[i] = exix[swap[i][0]];
        }
        xbox_SetFileReadHook(rn_rbb_read);
        {
            /* tools/rbb/make_akemi.py writes it; without it she is thrown
             * across the map at levers (no bb_hanger node). */
            char path[MAX_PATH];
            FILE *f;
            snprintf(path, sizeof path, "%s\\mods\\akemi_mesh.bin", g_exe_dir);
            f = fopen(path, "rb");
            if (f) {
                long n;
                fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
                if (n > 0 && n < 0x400000 && (mesh = (uint8_t *)malloc((size_t)n)) != NULL
                        && fread(mesh, 1, (size_t)n, f) == (size_t)n) mesh_len = (uint32_t)n;
                else { free(mesh); mesh = NULL; }
                fclose(f);
            }
            printf("[RN] Akemi: mods\\akemi_mesh.bin %s\n", mesh ? "loaded" : "not found (levers will misplace her)");
        }
        saved = 1;
    }
    if (on == applied) return;
    s_nvseg = 0; s_vend = 0;
    for (i = 0; i < N; i++) {           /* back to the file's own index */
        ridx[(LO + i) * 2] = keep_r[i][0]; ridx[(LO + i) * 2 + 1] = keep_r[i][1];
        exix[LO + i] = keep_x[i];
    }
    if (on) {
        uint32_t v = 0;
        rows = skel ? 3 : 2;
        for (k = first_set; k < first_set + 2; k++) {
            /* The title reads a whole set in one go, into a buffer of its
             * own: a set laid out larger than the file's is what corrupted
             * the heap, so the two are reported here. */
            uint32_t v0 = v, lo = 0xFFFFFFFFu, hi = 0, uold = 0, unew = 0;
            for (i = sets[k][0]; i <= sets[k][1]; i++) {
                uint32_t o = keep_r[i - LO][0], z = keep_r[i - LO][1] & 0x3FFFFFFFu;
                if (o < lo) lo = o;
                if (o + z > hi) hi = o + z;
                uold += keep_x[i - LO];
            }
            for (i = sets[k][0]; i <= sets[k][1]; i++) {
                uint32_t off = keep_r[i - LO][0], sz = keep_r[i - LO][1], us = keep_x[i - LO];
                for (j = 0; j < rows; j++)
                    if (swap[j][k + 1] == i) { off = ak_r[j][0]; sz = ak_r[j][1]; us = ak_x[j]; }
                s_vseg[s_nvseg].mem = NULL;
                if (mesh && swap[0][k + 1] == i) {      /* the patched mesh; unpacked size as in the file */
                    sz = mesh_len | 0x80000000u;
                    s_vseg[s_nvseg].mem = mesh;
                }
                s_vseg[s_nvseg].voff = v; s_vseg[s_nvseg].len = sz & 0x3FFFFFFFu; s_vseg[s_nvseg].src = off;
                s_nvseg++;
                ridx[i * 2] = RN_VBASE + v; ridx[i * 2 + 1] = sz;
                exix[i] = us;
                unew += us;
                v = (v + (sz & 0x3FFFFFFFu) + 15u) & ~15u;
            }
            printf("[RN] Akemi: set %d..%d  span %u -> %u (%+d), unpacked %u -> %u (%+d)%s\n",
                   sets[k][0], sets[k][1], hi - lo, v - v0, (int)(v - v0) - (int)(hi - lo),
                   uold, unew, (int)unew - (int)uold,
                   (v - v0 > hi - lo || unew > uold) ? "  <-- larger than the title expects" : "");
        }
        s_vend = (v + 2047u) & ~2047u;
    }
    applied = on;
    printf("[RN] Akemi: %s (skeleton %s; %d entries, %u KB laid out)\n",
           on ? "her mesh and texture stand in for Kurenai's" : "off",
           on && skel ? "hers" : "Kurenai's", s_nvseg, s_vend >> 10);
}

/* Akemi's proportions. Her mesh is posed by Kurenai's skeleton (pc01_h),
 * whose upper body is longer: the neck stretched. The title uses the
 * skeleton file as loaded -- 224-byte node records from +0xF0, each a name,
 * then rotation, translation and scale twice over -- so the rest
 * translations of the chest, shoulders, arms, neck and head are overwritten
 * with those of her own skeleton (cl01_h). The legs stay Kurenai's: hers are
 * shorter and the motions carry Kurenai's hip height, so she would float. */
static int s_akemi_on;
static uint32_t s_akemi_scan_frames;
void rn_akemi_frame(void)
{
    static const struct { uint8_t node; const char *name; float t[3]; } fix[] = {
        { 13, "bb_mune",    { 13.385821f, 0.000178f, -2.174969f } },
        { 14, "bb_l_kata",  { 17.817791f, -2.800088f, -1.935417f } },
        { 15, "bb_l_ude1",  { 8.719064f, -0.234885f, 0.039115f } },
        { 16, "bb_l_ude2",  { 27.369488f, 0.361123f, 0.063740f } },
        { 19, "bb_l_hand1", { 21.744635f, -0.051452f, -0.285614f } },
        { 20, "bb_l_hand2", { 7.703549f, 0.661790f, -0.081871f } },
        { 21, "bb_r_kata",  { 17.817791f, 2.799891f, -1.935419f } },
        { 22, "bb_r_ude1",  { 8.656052f, 0.232787f, 0.036925f } },
        { 23, "bb_r_ude2",  { 27.369484f, -0.361083f, 0.063702f } },
        { 26, "bb_r_hand1", { 21.744576f, 0.051364f, -0.285555f } },
        { 27, "bb_r_hand2", { 7.710007f, -0.889402f, -0.105948f } },
        { 29, "bb_kubi",    { 19.836206f, 0.000086f, 0.124389f } },
        { 30, "bb_head",    { 3.915399f, -0.187359f, -0.043823f } },
    };
    static uint8_t *found[8];
    static int nfound;
    static uint32_t tick;
    int i, k;
    if (!s_akemi_on) { nfound = 0; return; }
    tick++;
    /* Still the skeleton? (a stage change frees and reloads it) */
    for (i = 0; i < nfound; i++)
        if (memcmp(found[i], "QMO0", 4) || memcmp(found[i] + 0x40, "pc01_h-I", 8)) { nfound = 0; s_akemi_scan_frames = 600; }
    if (!nfound && s_akemi_scan_frames && !(tick % 45)) {
        uint8_t *base = (uint8_t *)xbox_GetMemoryOffset();
        static const uint32_t span[2][2] = { { 0x00010000u, 0x04000000u }, { 0x80000000u, 0x84000000u } };
        int sp;
        s_akemi_scan_frames = s_akemi_scan_frames > 45 ? s_akemi_scan_frames - 45 : 0;
        for (sp = 0; sp < 2 && nfound < 8; sp++) {
            uint8_t *p = base + span[sp][0], *end = base + span[sp][1];
            while (p < end && nfound < 8) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(p, &mi, sizeof mi)) break;
                if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                    uint8_t *q = (uint8_t *)mi.BaseAddress, *qe = q + mi.RegionSize;
                    if (q < p) q = p;
                    if (qe > end) qe = end;
                    for (; q + 0xF0 + 46 * 224 <= qe && nfound < 8; q += 16)
                        if (*(uint32_t *)q == 0x304F4D51u && !memcmp(q + 0x40, "pc01_h-I", 8)
                                && !strcmp((const char *)q + 0xF0 + 29 * 224, "bb_kubi"))
                            found[nfound++] = q;
                }
                p = (uint8_t *)mi.BaseAddress + mi.RegionSize;
            }
        }
        if (nfound) printf("[RN] Akemi: skeleton found (%d cop%s), proportions set\n", nfound, nfound == 1 ? "y" : "ies");
    }
    for (i = 0; i < nfound; i++)
        for (k = 0; k < (int)(sizeof fix / sizeof fix[0]); k++) {
            uint8_t *n = found[i] + 0xF0 + fix[k].node * 224;
            if (strcmp((const char *)n, fix[k].name)) continue;
            memcpy(n + 64 + 4 * 4, fix[k].t, 12);       /* translation */
            memcpy(n + 64 + 16 * 4, fix[k].t, 12);      /* and its second copy */
        }
}

/* The wire effect's materials, as the title found them (RN_WIRE_LOG=1,
 * tools/postprocess_gen.py, sub_00072A90). The razor wire has never been
 * seen drawn in this port, as Kurenai or as Akemi, and its texture has
 * never been uploaded: this says whether the effect is built at all. */
/* Where the razor wire goes missing: 1 started, 2 updated, 3 the update
 * reached the visibility test, 4 handed to the renderer, 5 stopped. The
 * counts go to the log once a second while they change. */
void rn_wire_evt(int what, uint32_t p)
{
    static unsigned n[9];
    static unsigned last[9];
    static DWORD at;
    static int on = -1;
    DWORD now;
    if (on < 0) { const char *e = getenv("RN_WIRE_LOG"); on = e && atoi(e); }
    if (!on || what < 1 || what > 8) return;
    n[what]++;
    if (what == 4 && getenv("RN_WIRE_SHOTS")) {
        /* RN_WIRE_SHOTS: list the draws of the first few frames the wire is
         * in, and photograph them. How the wire looks on screen settles in
         * one glance what a page of numbers argues about. */
        static int shots = 6, shutters = 40, skip, seq;
        extern void xbox_GpuLogNextFrame(void);
        extern int xbox_FramebufferDumpBmp(const char *path);
        if (shots > 0) { shots--; xbox_GpuLogNextFrame(); }
        if (shutters > 0 && ++skip % 2 == 0) {
            char path[64];
            shutters--;
            sprintf(path, "dumps\\wire%03d.bmp", seq++);
            xbox_FramebufferDumpBmp(path);
        }
    }
    now = GetTickCount();
    if (now - at < 1000) return;
    if (!memcmp(n, last, sizeof n)) return;
    at = now;
    memcpy(last, n, sizeof n);
    {
        const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
        uint32_t obj = 0, vis = 0;
        if (mem) {
            memcpy(&obj, mem + 0x69AFCCu, 4);
            if (obj) {
                uint32_t a1 = 0, a2 = 0;
                memcpy(&a1, mem + obj + 0xC8u, 4);
                if (a1) memcpy(&a2, mem + a1 + 0x48u, 4);
                if (a2) vis = mem[a2 + 1];
                obj = a2;
            }
        }
        printf("[RN] wire: started %u, updated %u, reached test %u, drawn %u, stopped %u;"
               " throw: in flight %u, hit %u, timed out %u (mesh %08X visible %u, last %08X)\n",
               n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8], obj, vis, p);
        fflush(stdout);
    }
}

/* The one-shot vertex-array patch (sub_00178170 case 2 -> D3D 0x0022A8E0).
 * Each mesh resource carries a relocation record per submesh naming the
 * pushbuffer offset of its 16-slot SET_VERTEX_DATA_ARRAY_OFFSET block and the
 * attributes' offsets within a vertex; the archive stores page-aligned
 * placeholders there. The engine writes the real addresses into the fixup
 * once, then rewrites the record's type to 4 so it never does it again --
 * which is safe only if the patch reached the pushbuffer that first time.
 * The wire trail's six blocks are still placeholders at draw time, so this
 * logs every record the engine does process. */
void rn_vtxfix(uint32_t rec, uint32_t pbdesc)
{
    static int on = -1, said;
    const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
    uint32_t off = 0, stream = 0, fix = 0, used = 0, size = 0, pbd = 0;
    if (on < 0) { const char *e = getenv("RN_VTXFIX_LOG"); on = e && atoi(e); }
    if (!on || !mem || !rec || said >= 2048) return;
    said++;
    memcpy(&off, mem + rec, 4);
    memcpy(&stream, mem + rec + 8, 4);
    memcpy(&fix, mem + 0x233C18, 4);
    if (fix) { memcpy(&used, mem + fix + 0x10, 4); memcpy(&size, mem + fix + 0x14, 4); }
    if (pbdesc) memcpy(&pbd, mem + pbdesc + 4, 4);
    printf("[RN] vtxfix pb %08X +%04X stream %u fixup %08X used %u of %u\n",
           pbd, off, stream, fix, used, size);
    fflush(stdout);
}

void rn_wire_log(uint32_t st)
{
    static int on = -1, done;
    const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
    static const char *const names[6] = { "wire_ef_ha01", "wire_ef_ha02", "wire_ef_ha03",
                                          "wire_ef_hun01", "wire_ef_hun02", "wire_ef_hun03" };
    int i, found = 0;
    if (on < 0) { const char *e = getenv("RN_WIRE_LOG"); on = e && atoi(e); }
    if (!on || done || !st || !mem) return;
    done = 1;
    printf("[RN] wire effect materials:");
    for (i = 0; i < 6; i++) {
        uint32_t p;
        memcpy(&p, mem + st + 8 + i * 4, 4);
        printf(" %s=%08X", names[i], p);
        found += p != 0;
    }
    printf("  (%d of 6 found)\n", found);
    {   /* the body, weapon and katana materials, looked up the same way */
        static const struct { uint32_t va; const char *name; } g[] = {
            { 0x694B78u, "body_mat1" }, { 0x694B7Cu, "body_mat2" }, { 0x694B80u, "hagata" },
            { 0x694B84u, "hagata_b" }, { 0x694B88u, "hagata_c" }, { 0x694B8Cu, "hundou" },
            { 0x694B90u, "hundou_b" }, { 0x694B94u, "hundou_c" }, { 0x694B98u, "katana" },
        };
        printf("[RN] player materials:");
        for (i = 0; i < (int)(sizeof g / sizeof g[0]); i++) {
            uint32_t p;
            memcpy(&p, mem + g[i].va, 4);
            printf(" %s=%08X", g[i].name, p);
        }
        printf("\n");
    }
    fflush(stdout);
}

uint32_t rn_costume_stage(uint32_t stage)
{
    /* Akemi loads over the black-suit (dark) sets, not the red ones. Her
     * mesh is 57 KB larger than the red Kurenai's and 13 KB larger packed,
     * which pushed the set past the buffer the title reads it into: the
     * mines save froze and loading a new area crashed in a corrupted heap.
     * Over the dark sets, whose Kurenai is the larger of the two, the set
     * comes out smaller than the title expects either way. She looks the
     * same: her own mesh and texture replace whichever Kurenai is there.
     * RN_AKEMI_SET=red goes back to the red sets, to try it. */
    int c = xbox_MenuTitleValue(s_costume_opt);
    uint32_t out = c == 1 ? 0u : c == 2 ? 0x1Eu : c == 3 ? (rn_akemi_dark() ? 0x1Eu : 0u) : stage;
    rn_akemi_apply(c == 3);
    s_akemi_on = (c == 3);
    s_akemi_scan_frames = 1800;
    printf("[RN] costume: stage %u, choice %d -> loads as stage %u\n", stage, c, out);
    return out;
}

/* Crouch straight out of a run. RN_RUN_CROUCH=0 restores stop-then-crouch. */
int rn_run_crouch(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RN_RUN_CROUCH"); on = !(e && !atoi(e)); }
    return on;
}

static void rn_dj_frame(uint32_t act)
{
    static int l3_was;
    unsigned char st[64];
    int down = 0;
    if (s_dj_on < 0) { const char *e = getenv("RN_DOUBLE_JUMP"); s_dj_on = !(e && !atoi(e)); }
    memset(st, 0, sizeof st);
    if (xbox_InputGetState(0, (void *)st) == 0) {
        int l3;
        { extern void xbox_PadRemap(unsigned short *buttons, unsigned char *analog);
          xbox_PadRemap((unsigned short *)(void *)(st + 4), st + 6); }    /* the F6 Controls tab */
        l3 = (st[4] & 0x40) != 0;                  /* wButtons: left thumb click */
        down = st[6] > 30;                             /* bAnalogButtons[0] = A */
        if (l3 && !l3_was) s_run_fast = !s_run_fast;
        l3_was = l3;
    }
    if (act != s_dj_act) { s_dj_act = act; s_dj_released = 0; }
    if (!down) s_dj_released = 1;
    s_dj_pending = 0;
}
void rn_dj_dbg(uint32_t act, uint32_t flags, uint32_t st, uint32_t in, uint32_t mask, uint32_t arg)
{
    static int on = -1, n; static uint32_t l[5];
    if (on < 0) { const char *e = getenv("RN_ACT_LOG"); on = e && atoi(e); }
    if (!on || n > 800) return;
    if (act != 0x415u && act != 0x4C9u && !(in & mask)) return;
    if (l[0] == act && l[1] == flags && l[2] == st && l[3] == (in & mask) && l[4] == arg) return;
    l[0] = act; l[1] = flags; l[2] = st; l[3] = in & mask; l[4] = arg; n++;
    fprintf(stderr, "[DJ] act %03X flags %08X st238 %X in %08X mask %08X arg %08X\n", act, flags, st, in, mask, arg);
    fflush(stderr);
}
int rn_dj_try(uint32_t act)
{
    { static int on = -1; if (on < 0) { const char *e = getenv("RN_ACT_LOG"); on = e && atoi(e); }
      if (on) { fprintf(stderr, "[DJ] reached 419E act %03X released %d\n", act, s_dj_released); fflush(stderr); } }
    if (s_dj_on <= 0 || !s_dj_released) return 0;
    if (act != 0x415u && act != 0x4C9u) return 0;
    s_dj_released = 0;
    s_dj_pending = 1;
    return 1;
}
int rn_dj_take(void)
{
    int p = s_dj_pending;
    s_dj_pending = 0;
    return p;
}

void rn_act_log(int what, uint32_t actor, uint32_t c, uint32_t a0, uint32_t a1, uint32_t ret)
{
    static int on = -1, n;
    static uint32_t last_actor, last_a0;
    if (on < 0) { const char *e = getenv("RN_ACT_LOG"); on = e && atoi(e); }
    if (!on || n > 6000) return;
    if (what == 0 && actor == last_actor && a0 == last_a0) return;
    if (what == 0) { last_actor = actor; last_a0 = a0; }
    n++;
    fprintf(stderr, "[ACT] t=%lu %s actor %08X ecx %X a0 %03X a1 %X ret %08X\n",
            (unsigned long)GetTickCount(), what ? "B2C60" : "set", actor, c, a0, a1, ret);
    fflush(stderr);
}

/* Orbit camera feel. Called from the player-control routine with the right
 * stick as the title is about to integrate it into pitch and yaw (see
 * tools/postprocess_gen.py). Settings, all optional:
 *   RN_CAM_INVERT_X / RN_CAM_INVERT_Y  1 = opposite to the original (default 1)
 *   RN_CAM_SPEED     horizontal speed, percent of the original (default 100)
 *   RN_CAM_SPEED_Y   vertical speed, percent of the original (default 30)
 *   RN_CAM_CURVE     response exponent x100, 100 = linear (default 130)
 *   RN_CAM_SMOOTH_MS ease in/out time constant, 0 = none (default 50) */
void rn_cam_shape(float *px, float *py)
{
    static int init, inv_x = 1, inv_y = 1;
    static float sp_x = 1.0f, sp_y = 0.30f, curve = 1.3f, tau = 0.050f, sx, sy;
    float x = *px, y = *py, m, dt;
    {   /* the player's action id every frame; RN_ACT_LOG prints changes */
        static int alog = -1; static uint32_t la, lf;
        if (alog < 0) { const char *e = getenv("RN_ACT_LOG"); alog = e && atoi(e); }
        {
            const unsigned char *mm = (const unsigned char *)(uintptr_t)g_xbox_mem_offset;
            uint32_t st, id = 0, fl;
            memcpy(&st, mm + 0x01257010u + 0x10u, 4);
            memcpy(&fl, mm + 0x01257010u + 0x234u, 4);
            if (st > 0x10000u && st < 0x04000000u) memcpy(&id, mm + st + 0xC4u, 4);
            rn_dj_frame(id);
            rn_akemi_frame();
            if (alog) {
                /* the title's button masks (jump is the one 0x69CBD8 points at)
                 * and the pad word it tests them against, on any change */
                static uint32_t lm[9]; uint32_t mk[9]; int q, ch = 0;
                for (q = 0; q < 8; q++) memcpy(&mk[q], mm + 0x69AD30u + q * 4, 4);
                { uint32_t pp, p2 = 0, p3 = 0;
                  memcpy(&pp, mm + 0x69AFCCu, 4);
                  if (pp > 0x10000u && pp < 0x04000000u) memcpy(&p2, mm + pp + 0x10Cu, 4);
                  if (p2 > 0x10000u && p2 < 0x04000000u) memcpy(&p3, mm + p2 + 8u, 4);
                  mk[8] = 0;
                  if (p3 > 0x10000u && p3 < 0x04000000u) memcpy(&mk[8], mm + p3 + 0x80Cu, 4); }
                for (q = 0; q < 9; q++) if (mk[q] != lm[q]) ch = 1;
                if (ch) {
                    fprintf(stderr, "[BTN] t=%lu masks %X %X %X %X %X %X %X %X  w80C %08X\n", (unsigned long)GetTickCount(),
                            mk[0], mk[1], mk[2], mk[3], mk[4], mk[5], mk[6], mk[7], mk[8]);
                    memcpy(lm, mk, sizeof lm);
                }
            }
            if (alog && (id != la || fl != lf)) {
                fprintf(stderr, "[PST] t=%lu act %03X flags %08X\n", (unsigned long)GetTickCount(), id, fl);
                fflush(stderr);
                la = id; lf = fl;
            }
        }
    }
    if (!init) {
        const char *e;
        init = 1;
        if ((e = getenv("RN_CAM_INVERT_X"))) inv_x = atoi(e) != 0;
        if ((e = getenv("RN_CAM_INVERT_Y"))) inv_y = atoi(e) != 0;
        if ((e = getenv("RN_CAM_SPEED")) && atoi(e) > 0) sp_x = atoi(e) / 100.0f;
        if ((e = getenv("RN_CAM_SPEED_Y")) && atoi(e) > 0) sp_y = atoi(e) / 100.0f;
        if ((e = getenv("RN_CAM_CURVE")) && atoi(e) > 0) curve = atoi(e) / 100.0f;
        if ((e = getenv("RN_CAM_SMOOTH_MS"))) tau = atoi(e) / 1000.0f;
    }
    if (x != x || y != y) return;
    if (inv_x) x = -x;
    if (inv_y) y = -y;
    m = sqrtf(x * x + y * y);
    if (m > 1e-4f && curve != 1.0f) {
        float t = m > 1.0f ? 1.0f : m;
        float g = powf(t, curve) / t;
        x *= g; y *= g;
    }
    x *= sp_x; y *= sp_y;
    { extern ptrdiff_t xbox_GetMemoryOffset(void); memcpy(&dt, (const char *)(uintptr_t)0x2EEC50 + xbox_GetMemoryOffset(), 4); }
    if (tau > 0.0f && dt > 0.0f && dt < 0.25f) {
        float k = 1.0f - expf(-dt / tau);
        sx += (x - sx) * k; sy += (y - sy) * k;
        if (x == 0.0f && y == 0.0f && sx * sx + sy * sy < 1e-5f) sx = sy = 0.0f;
    } else { sx = x; sy = y; }
    *px = sx; *py = sy;
}

void rn_probe(int id, uint32_t self, uint32_t r_esi, uint32_t r_edi)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    static LONG shown;
    int i;

    if (InterlockedIncrement(&shown) > 40)
        return;
    fprintf(stderr, "[RN-PROBE] site %d this=%08X esi=%08X edi=%08X esp=%08X\n",
            id, self, r_esi, r_edi, g_esp);
    if (id != 3 && self >= 0x10000u && self < 0x04000000u) {
        fprintf(stderr, "  this:");
        for (i = 0; i < 0x98; i += 4)
            fprintf(stderr, " %08X", *(const uint32_t *)(mem + self + i));
        fprintf(stderr, "\n");
        if (id == 1) {
            uint32_t ver = *(const uint32_t *)(mem + self + 0x8C);
            fprintf(stderr, "  stamp in file: \"");
            for (i = 0; i < 20; i++) {
                uint8_t c = mem[self + 0x72 + i];
                fprintf(stderr, c >= 0x20 && c < 0x7F ? "%c" : "\\x%02X", c);
            }
            fprintf(stderr, "\"\n  version string @%08X: \"", ver);
            if (ver >= 0x10000u && ver < 0x04000000u)
                for (i = 0; i < 40 && mem[ver + i]; i++) {
                    uint8_t c = mem[ver + i];
                    fprintf(stderr, c >= 0x20 && c < 0x7F ? "%c" : "\\x%02X", c);
                }
            fprintf(stderr, "\"\n");
        }
    }
    if ((id == 4 || id == 5) && self >= 0x10000u && self < 0x04000000u) {
        /* event {reader, code, index, item}; loader step globals */
        uint32_t item = *(const uint32_t *)(mem + self + 0xC);
        fprintf(stderr, "  event code=%d index=%u item=%08X; loader step=%u sub=%u err=%u\n",
                (int)*(const uint32_t *)(mem + self + 4),
                *(const uint32_t *)(mem + self + 8), item,
                *(const uint32_t *)(mem + 0x0069AFBC),
                *(const uint32_t *)(mem + 0x0069AF94), mem[0x0069AFD0]);
        if ((item >= 0x10000u && item < 0x04000000u) ||
            (item >= 0x80000000u && item < 0x84000000u)) {
            fprintf(stderr, "  item:");
            for (i = 0; i < 48; i++)
                fprintf(stderr, " %02X", mem[item + i]);
            fprintf(stderr, "\n");
        }
    }
    {
        const uint32_t *sp = (const uint32_t *)(mem + g_esp);
        fprintf(stderr, "  guest stack:");
        for (i = 0; i < 48; i++)
            if (sp[i] >= 0x11000u && sp[i] < 0x2BA000u)
                fprintf(stderr, " %08X", sp[i]);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* RN_HANG_DUMP=<seconds>: every N seconds, the host call stack of every
 * thread in the process, as exe+offset (symbolise against the .pdb or with
 * llvm-symbolizer). Generated functions keep their guest names, so this says
 * exactly where each thread sits -- including the timer/DPC thread, which the
 * guest-thread table cannot see. Needs <tlhelp32.h>. */
#include <tlhelp32.h>
static DWORD WINAPI rn_hang_dumper(LPVOID p)
{
    DWORD period = (DWORD)(uintptr_t)p, me = GetCurrentThreadId();
    DWORD pid = GetCurrentProcessId();
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    int round = 0;

    for (;;) {
        HANDLE snap;
        THREADENTRY32 te;
        Sleep(period);
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            continue;
        fprintf(stderr, "[HOSTSTACKS] round %d\n", ++round);
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) do {
            HANDLE th;
            CONTEXT ctx;
            STACKFRAME64 sf;
            int n;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me)
                continue;
            th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                            THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th)
                continue;
            if (SuspendThread(th) == (DWORD)-1) { CloseHandle(th); continue; }
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th, &ctx)) {
                char line[1024];
                int o = snprintf(line, sizeof(line), "  tid %lu:",
                                 (unsigned long)te.th32ThreadID);
                uintptr_t rsp0 = (uintptr_t)ctx.Rsp;   /* StackWalk64 rewrites ctx */
                memset(&sf, 0, sizeof(sf));
                sf.AddrPC.Offset = ctx.Rip;    sf.AddrPC.Mode = AddrModeFlat;
                sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
                sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
                for (n = 0; n < 28 && o < (int)sizeof(line) - 24; n++) {
                    uintptr_t pc;
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(),
                                     th, &sf, &ctx, NULL, SymFunctionTableAccess64,
                                     SymGetModuleBase64, NULL))
                        break;
                    pc = (uintptr_t)sf.AddrPC.Offset;
                    if (!pc)
                        break;
                    if (pc >= base && pc < base + 0x10000000u)
                        o += snprintf(line + o, sizeof(line) - o, " exe+%llX",
                                      (unsigned long long)(pc - base));
                    else
                        o += snprintf(line + o, sizeof(line) - o, " sys");
                }
                /* A thread blocked inside the system unwinds to nothing but
                 * "sys": scan its stack for anything that points into the
                 * exe instead. Over-reports (stale slots), but names the
                 * functions it came through. */
                if (!strstr(line, "exe+")) {
                    MEMORY_BASIC_INFORMATION mbi;
                    const uintptr_t *sp = (const uintptr_t *)rsp0;
                    size_t k, lim = 0;
                    int found = 0;
                    if (VirtualQuery(sp, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT)
                        lim = ((uintptr_t)mbi.BaseAddress + mbi.RegionSize -
                               (uintptr_t)sp) / sizeof(uintptr_t);
                    if (lim > 1500) lim = 1500;
                    o += snprintf(line + o, sizeof(line) - o, " | scan:");
                    for (k = 0; k < lim && found < 14 &&
                                o < (int)sizeof(line) - 24; k++) {
                        uintptr_t v = sp[k];
                        if (v >= base + 0x1000 && v < base + 0x1200000u) {
                            o += snprintf(line + o, sizeof(line) - o, " exe+%llX",
                                          (unsigned long long)(v - base));
                            found++;
                        }
                    }
                }
                /* stderr is not touched while a thread is suspended holding
                 * its lock: the line is printed after the resume. */
                ResumeThread(th);
                fprintf(stderr, "%s\n", line);
            } else {
                ResumeThread(th);
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
        CloseHandle(snap);
        fflush(stderr);
    }
}

/* Bring-up: creating rn_dump.txt beside the exe (lines "va len") writes each
 * guest range to dumps\\rn_dump_<va>.bin, then deletes rn_dump.txt. */
static void rn_dump_poll(void)
{
    FILE *f = fopen("rn_dump.txt", "r");
    char line[128];
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        unsigned int va = 0, len = 0;
        if (sscanf(line, "%x %x", &va, &len) == 2 && len && len <= 0x400000u &&
            ((va >= 0x10000u && va + len <= 0x04000000u) ||
             (va >= 0x80000000u && va + len <= 0x84000000u))) {
            char name[64];
            FILE *o;
            CreateDirectoryA("dumps", NULL);
            snprintf(name, sizeof(name), "dumps\\rn_dump_%08X.bin", va);
            o = fopen(name, "wb");
            if (o) {
                fwrite((const uint8_t *)(uintptr_t)g_xbox_mem_offset + va, 1, len, o);
                fclose(o);
            }
        }
    }
    fclose(f);
    DeleteFileA("rn_dump.txt");
    fprintf(stderr, "[RN] memory dump written\n");
}

static DWORD WINAPI rn_debug_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        rn_debug_state((const uint8_t *)(uintptr_t)g_xbox_mem_offset);
        rn_dump_poll();
        Sleep(250);
    }
}

/* Unattended testing (testloop.bat): creating rn_quit.txt beside the exe ends
 * the run, so a script can stop the game, swap the exe and start it again. */
/* 60 frames a second (F6 menu, Frame rate). The toolkit's switch doubles the
 * vertical blank rate, and the title, which waits two blanks a frame, then
 * runs twice as many frames. Its own clock is a step per frame in sixtieths
 * of a second -- 0x2EEC50: 2.0, or 2.4 on a 50 Hz console -- beside the
 * frame rate that goes with it (0x69AFAC: 30 or 25); movement, timers and
 * animation are scaled by the step. Halving the step and doubling the rate
 * keeps the game at its own speed instead of twice as fast. */
extern int xbox_GfxFps60(void);
static void rn_fps_apply(void)
{
    static float base_step, base_fps;
    uint8_t *mem = (uint8_t *)g_xbox_mem_offset;
    float step, fps, want_step, want_fps;
    int on = xbox_GfxFps60();
    if (!mem) return;
    memcpy(&step, mem + 0x2EEC50u, 4);
    memcpy(&fps, mem + 0x69AFACu, 4);
    if (base_step == 0.0f) {
        if (!((step == 2.0f && fps == 30.0f) || (step == 2.4f && fps == 25.0f))) return;   /* not set up yet */
        base_step = step; base_fps = fps;
    }
    want_step = on ? base_step * 0.5f : base_step;
    want_fps = on ? base_fps * 2.0f : base_fps;
    if (step != want_step || fps != want_fps) {
        memcpy(mem + 0x2EEC50u, &want_step, 4);
        memcpy(mem + 0x69AFACu, &want_fps, 4);
        fprintf(stderr, "[RN] frame rate %.0f: step %.2f sixtieths a frame\n", want_fps, want_step);
    }
}

static DWORD WINAPI rn_quit_watch(LPVOID unused)
{
    (void)unused;
    DeleteFileA("rn_quit.txt");
    for (;;) {
        rn_fps_apply();
        if (GetFileAttributesA("rn_quit.txt") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("rn_quit.txt");
            fprintf(stderr, "[RN] rn_quit.txt seen: exiting\n");
            fflush(stderr);
            ExitProcess(0);
        }
        Sleep(100);
    }
}

static DWORD WINAPI sims_semaphore_watch(LPVOID unused)
{
    int i;
    DWORD t0 = GetTickCount();
    (void)unused;
    for (i = 0; i < 120000; i++) {          /* up to ~10 minutes */
        /* the first 2 s spin: a release executed before the hand-over is lost */
        uint8_t *mem = (uint8_t *)g_xbox_mem_offset;
        uint32_t dev = *(volatile uint32_t *)(mem + SIMS_D3D_DEVICE_PTR_VA);
        if (dev >= 0x00010000u && dev < 0x04000000u) {
            uint32_t sem = *(volatile uint32_t *)(mem + dev + SIMS_D3D_SEMAPHORE_OFF);
            if (sem) {
                nv2a_pb_set_semaphore_target(sem);
                /* D3D's "GPU far behind" sleep (software method 5). */
                {
                    extern void xbox_GpuSetWakeEvent(uint32_t event_va);
                    xbox_GpuSetWakeEvent(dev + 0x1DCCu);
                }
                fprintf(stderr, "[D3D] D3D device at 0x%08X, GPU semaphore at 0x%08X\n",
                        dev, sem);
                return 0;
            }
        }
        if (i < 2000000 && GetTickCount() - t0 < 2000) { YieldProcessor(); i--; }
        else Sleep(5);
    }
    fprintf(stderr, "[D3D] D3D device never appeared; GPU semaphore not registered\n");
    return 0;
}

/* ── WinMain ───────────────────────────────────────────────── */


#ifdef SIMS_TLS_DIAG
/* Bring-up: what the loader set up for this thread's static TLS, written to
 * tls_diag.txt next to the exe. Reads nothing it has not VirtualQuery'd. */
extern unsigned int _tls_index;
extern void (*g_layout_probe)(const char *where);
static __thread volatile unsigned int s_tls_probe = 0x5A5A1234u;
static void tls_probe(const char *where)
{
    FILE *f = fopen("tls_diag.txt", "a");
    void **arr = (void **)__readgsqword(0x58);
    void *blk = NULL;
    MEMORY_BASIC_INFORMATION ma, mb;
    char exe[MAX_PATH];
    if (!f) return;
    GetModuleFileNameA(NULL, exe, sizeof exe);
    memset(&ma, 0, sizeof ma); memset(&mb, 0, sizeof mb);
    if (arr) VirtualQuery(arr, &ma, sizeof ma);
    if (arr && ma.State == MEM_COMMIT) blk = arr[_tls_index];
    if (blk) VirtualQuery(blk, &mb, sizeof mb);
    fprintf(f, "[%s] %s: image %p tls_index %u array %p (state %lx) block %p (state %lx prot %lx) var@%p\n",
            strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe, where,
            (void *)GetModuleHandleA(NULL), _tls_index, (void *)arr,
            (unsigned long)ma.State, blk, (unsigned long)mb.State,
            (unsigned long)mb.Protect, (void *)&s_tls_probe);
    {
        /* The directory as the loader saw it. */
        unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        IMAGE_TLS_DIRECTORY64 *td = (IMAGE_TLS_DIRECTORY64 *)(base + dd->VirtualAddress);
        fprintf(f, "    dir: start %llx end %llx index@%llx (=%p here) callbacks %llx zerofill %lx chars %lx\n",
                (unsigned long long)td->StartAddressOfRawData, (unsigned long long)td->EndAddressOfRawData,
                (unsigned long long)td->AddressOfIndex, (void *)&_tls_index,
                (unsigned long long)td->AddressOfCallBacks, (unsigned long)td->SizeOfZeroFill,
                (unsigned long)td->Characteristics);
    }
    if (blk && mb.State == MEM_COMMIT) {
        unsigned int v = s_tls_probe;
        s_tls_probe = v + 1;
        fprintf(f, "    probe read %08X, wrote %08X ok\n", v, s_tls_probe);
    } else {
        fprintf(f, "    block not usable\n");
    }
    fclose(f);
}
#endif

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

#ifdef SIMS_TLS_DIAG
    {
        char d[MAX_PATH], *sl;
        GetModuleFileNameA(NULL, d, sizeof d);
        if ((sl = strrchr(d, '\\')) != NULL) { *sl = 0; SetCurrentDirectoryA(d); }
        tls_probe("winmain-start");
        g_layout_probe = tls_probe;
    }
#endif
    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Run from the exe's folder whatever the shortcut's working dir is. */
    {
        char *slash;
        GetModuleFileNameA(NULL, g_exe_dir, sizeof(g_exe_dir));
        slash = strrchr(g_exe_dir, '\\');
        if (slash) *slash = 0;
        SetCurrentDirectoryA(g_exe_dir);
    }

    /* Launched by double-click (no console): keep a log for bug reports. */
    /* Always, console or not: the log file is what gets read afterwards. */
    {
        /* A GUI process starts with no valid stdout/stderr, so both are
         * reopened onto the file (in append mode, so the two streams'
         * writes interleave instead of overwriting each other). */
        FILE *t = fopen("redninja.log", "w");
        if (t) fclose(t);
        freopen("redninja.log", "a", stdout);
        freopen("redninja.log", "a", stderr);
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }

    load_settings_ini();
    apply_title_defaults();
    rn_menu_register();

    /* First run: unpack the player's own disc image into game\\. */
    {
        char game_dir[MAX_PATH];
        snprintf(game_dir, sizeof(game_dir), "%s\\%s", g_exe_dir, YOUR_GAME_DIR);
        if (!iso_setup_ensure_game(g_exe_dir, game_dir, iso_from_cmdline()))
            return 1;
    }

    printf("=== Red Ninja: End of Honor - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    /* Step 1: Load XBE */
    if (!load_xbe(YOUR_GAME_XBE_PATH, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load game\\default.xbe.\n"
                    "Delete the 'game' folder and start again to re-run setup.",
                    "Red Ninja: End of Honor", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
#ifdef SIMS_TLS_DIAG
    tls_probe("before-layout");
#endif
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Red Ninja: End of Honor", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* USB host controllers (pads), after the guest memory exists. */
    {
        extern void xbox_OhciInit(void);
        xbox_OhciInit();
    }

    /* Draw on the host GPU (Direct3D 11). SIMS_GPU=0 / RECOMP_GPU=0 keeps the
     * software rasteriser; so does a machine without a D3D11 device. */
    {
        extern int nv2a_d3d11_init(void);
        const char *g = getenv("SIMS_GPU");
        if (g && *g == '0')
            set_default_env("RECOMP_GPU", "0");
        if (!nv2a_d3d11_init())
            printf("Drawing with the software rasteriser.\n");
    }
    {
        extern void xbox_MemWatchStart(void);
        xbox_MemWatchStart();                 /* RECOMP_MEMWATCH=1: debugging */
        { extern void rn_prof_start(void); rn_prof_start(); }
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(YOUR_GAME_DIR, NULL);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    CloseHandle(CreateThread(NULL, 0, sims_semaphore_watch, NULL, 0, NULL));
    if (getenv("RN_HANG_DUMP")) {
        DWORD secs = (DWORD)atoi(getenv("RN_HANG_DUMP"));
        if (secs < 1) secs = 10;
        CloseHandle(CreateThread(NULL, 0, rn_hang_dumper,
                                 (LPVOID)(uintptr_t)(secs * 1000), 0, NULL));
    }
    CloseHandle(CreateThread(NULL, 0, rn_quit_watch, NULL, 0, NULL));
    if (getenv("RN_DEBUG_STATE"))
        CloseHandle(CreateThread(NULL, 0, rn_debug_thread, NULL, 0, NULL));
    if (getenv("SIMS_THREADDUMP")) {
        DWORD secs = (DWORD)atoi(getenv("SIMS_THREADDUMP"));
        if (secs < 1) secs = 10;
        CloseHandle(CreateThread(NULL, 0, sims_thread_dumper,
                                 (LPVOID)(uintptr_t)(secs * 1000), 0, NULL));
    }

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}

