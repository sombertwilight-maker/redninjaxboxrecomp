/* Diagnostic-only call profiler (built with -DRECOMP_CALL_PROFILE).
 * Counts guest functions called while a gamepad button is held (the "hot"
 * set) against everything called the rest of the time (the baseline), and
 * writes the functions only the hot window ran to prof_N.txt on release.
 * Also records what one Go Here call runs (prof_gohere.txt). */
#ifdef RECOMP_CALL_PROFILE
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "input/xinput_xbox.h"

#define HSZ (1u << 18)
static uint32_t base_k[HSZ], hot_k[HSZ], gh_k[HSZ];
static uint32_t base_c[HSZ], hot_c[HSZ], gh_c[HSZ];
static volatile int s_hot, s_gh;
volatile int g_prof_gohere;

static void add(uint32_t *k, uint32_t *c, uint32_t va)
{
    uint32_t h = (va * 2654435761u) >> 14;
    for (int i = 0; i < 64; i++, h = (h + 1) & (HSZ - 1)) {
        if (k[h] == va) { c[h]++; return; }
        if (k[h] == 0) { k[h] = va; c[h] = 1; return; }
    }
}
static uint32_t get(uint32_t *k, uint32_t *c, uint32_t va)
{
    uint32_t h = (va * 2654435761u) >> 14;
    for (int i = 0; i < 64; i++, h = (h + 1) & (HSZ - 1)) {
        if (k[h] == va) return c[h];
        if (k[h] == 0) return 0;
    }
    return 0;
}

void recomp_call_profile_hit(uint32_t va)
{
    if (g_prof_gohere) add(gh_k, gh_c, va);
    if (s_hot) add(hot_k, hot_c, va); else add(base_k, base_c, va);
}

static void dump(const char *name, uint32_t *k, uint32_t *c, int only_new)
{
    FILE *f = fopen(name, "w");
    if (!f) return;
    for (uint32_t i = 0; i < HSZ; i++)
        if (k[i] && (!only_new || get(base_k, base_c, k[i]) == 0))
            fprintf(f, "%08X %u base %u\n", k[i], c[i], get(base_k, base_c, k[i]));
    fclose(f);
}

void sims_prof_gohere_done(void)
{
    static int n;
    char name[64];
    snprintf(name, sizeof name, "prof_gohere_%d.txt", n++);
    dump(name, gh_k, gh_c, 0);
    memset(gh_k, 0, sizeof gh_k); memset(gh_c, 0, sizeof gh_c);
}

/* Called from the input filter for port 0. */
void sims_prof_input(XBOX_INPUT_STATE *st)
{
    static int held, n;
    static DWORD released;
    static int mask = -1;
    int down;
    if (mask < 0) {
        const char *e = getenv("SIMS_PROF_BUTTON");    /* analog index: 0 A, 1 B, 2 X, 3 Y, 4 Black, 5 White */
        mask = e ? atoi(e) : 1;
    }
    down = st->Gamepad.bAnalogButtons[mask & 7] > 30;
    if (down) { held = 1; s_hot = 1; return; }
    if (held) { held = 0; released = GetTickCount(); }
    if (s_hot && GetTickCount() - released > 1500) {
        char name[64];
        s_hot = 0;
        snprintf(name, sizeof name, "prof_%d.txt", n++);
        dump(name, hot_k, hot_c, 1);
        memset(hot_k, 0, sizeof hot_k); memset(hot_c, 0, sizeof hot_c);
    }
}
#endif

#ifdef RECOMP_CALL_PROFILE
/* Red Ninja: file-driven windows. dumps\prof_start.txt opens a window,
 * dumps\prof_stop.txt closes it and writes every function called inside it
 * (with its count) to dumps\prof_N.txt. Diff two windows to find what one
 * action runs that another does not. */
static DWORD WINAPI rn_prof_thread(LPVOID u)
{
    int n = 0;
    (void)u;
    for (;;) {
        Sleep(20);
        {
            /* dumps\snapram.txt: copy guest RAM 0x01000000..0x01600000 (the
             * game objects) to dumps\ram_N.bin. dumps\peek.txt (hex guest
             * addresses, one per line): while a window is open, log each
             * address as float and int every 20 ms to dumps\peeklog.txt. */
            extern ptrdiff_t g_xbox_mem_offset;
            static int sn; static uint32_t pk[16]; static int npk; static FILE *pl;
            const unsigned char *m = (const unsigned char *)(uintptr_t)g_xbox_mem_offset;
            if (GetFileAttributesA("dumps\\snapram.txt") != INVALID_FILE_ATTRIBUTES) {
                char nm[64]; FILE *f;
                DeleteFileA("dumps\\snapram.txt");
                snprintf(nm, sizeof nm, "dumps\\ram_%d.bin", sn++);
                f = fopen(nm, "wb");
                if (f) { fwrite(m + 0x01000000u, 1, 0x00600000u, f); fclose(f); }
            }
            if (s_hot && !pl) {
                FILE *f = fopen("dumps\\peek.txt", "r");
                npk = 0;
                if (f) { unsigned a; while (npk < 16 && fscanf(f, "%x", &a) == 1) pk[npk++] = a; fclose(f); }
                if (npk) pl = fopen("dumps\\peeklog.txt", "a");
                if (pl) fprintf(pl, "--- window %d\n", n);
            }
            if (s_hot && pl) {
                int q;
                fprintf(pl, "%lu", (unsigned long)GetTickCount());
                for (q = 0; q < npk; q++) {
                    float fv; int iv;
                    memcpy(&fv, m + pk[q], 4); memcpy(&iv, m + pk[q], 4);
                    fprintf(pl, " %.3f/%d", fv, iv);
                }
                fprintf(pl, "\n");
            }
            if (!s_hot && pl) { fclose(pl); pl = NULL; }
        }
        {
            /* Jump episodes: every press of A opens a window (or extends
             * the open one); 1.8 s after the last press the functions called
             * are written to dumps\ep_N.txt with the number of presses. */
            extern DWORD xbox_InputGetState(DWORD, XBOX_INPUT_STATE *);
            static int was, ep, presses, epn; static DWORD last_press;
            XBOX_INPUT_STATE st;
            int down = xbox_InputGetState(0, &st) == 0 && st.Gamepad.bAnalogButtons[0] > 30;
            if (down && !was) {
                if (!ep) {
                    memset(hot_k, 0, sizeof hot_k); memset(hot_c, 0, sizeof hot_c);
                    ep = 1; presses = 0; s_hot = 1;
                }
                presses++; last_press = GetTickCount();
            }
            was = down;
            if (ep && GetTickCount() - last_press > 1800) {
                char name[64]; FILE *f;
                ep = 0; s_hot = 0;
                snprintf(name, sizeof name, "dumps\\ep_%d.txt", epn++);
                dump(name, hot_k, hot_c, 0);
                f = fopen(name, "a"); if (f) { fprintf(f, "presses %d\n", presses); fclose(f); }
            }
        }
        if (!s_hot && GetFileAttributesA("dumps\\prof_start.txt") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("dumps\\prof_start.txt");
            memset(hot_k, 0, sizeof hot_k); memset(hot_c, 0, sizeof hot_c);
            s_hot = 1;
        }
        if (s_hot && GetFileAttributesA("dumps\\prof_stop.txt") != INVALID_FILE_ATTRIBUTES) {
            char name[64];
            DeleteFileA("dumps\\prof_stop.txt");
            s_hot = 0;
            snprintf(name, sizeof name, "dumps\\prof_%d.txt", n++);
            dump(name, hot_k, hot_c, 0);
        }
    }
    return 0;
}
void rn_prof_start(void)
{
    CloseHandle(CreateThread(NULL, 0, rn_prof_thread, NULL, 0, NULL));
}
#else
void rn_prof_start(void) {}
#endif
