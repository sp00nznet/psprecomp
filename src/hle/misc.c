/* psprecomp — the smaller firmware libraries.
 *
 * Kernel_Library, UtilsForUser, StdioForUser, sceSuspendForUser,
 * LoadExecForUser, ModuleMgrForUser, sceCtrl and sceAudio. Individually small,
 * but collectively they are what a game's C runtime needs before main() gets
 * anywhere -- newlib's reentrancy setup alone wants interrupt masking, a
 * clock, and the standard file descriptors.
 */

#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <stdatomic.h>
#endif

/* ---- Kernel_Library ------------------------------------------------------
 * Interrupt masking. The mask belongs to the *thread* that opened the critical
 * section, not to the machine: with several guest threads on one CPU, a global
 * flag would let one thread's suspend silence interrupts inside another
 * thread's unrelated bracket. The scheduler owns that state, because it is
 * also what decides whether an interrupt may be delivered at a park. */

static void hle_CpuSuspendIntr(void) {
    psp_ret((uint32_t)psp_threadman_intr_suspend());   /* the cookie resume expects */
}

static void hle_CpuResumeIntr(void) {
    psp_threadman_intr_resume(psp_arg(0));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- UtilsForUser -------------------------------------------------------- */

/* Cache maintenance. There is no cache to write back -- the recompiled code
 * and the GE share one flat backing store -- so these are genuinely no-ops
 * rather than unimplemented. */
static void hle_CacheOp(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_LibcTime(void) {
    time_t t = time(NULL);
    uint32_t out = psp_arg(0);
    if (out) psp_write32(out, (uint32_t)t);
    psp_ret((uint32_t)t);
}

static void hle_LibcClock(void) {
    /* Microseconds since start. A game that uses this for frame pacing needs
     * it to advance, so it is derived from the host clock rather than being a
     * constant -- a frozen clock makes a game either spin or run at infinite
     * speed, both of which look like a hang. */
    psp_ret((uint32_t)((uint64_t)clock() * 1000000ull / CLOCKS_PER_SEC));
}

static void hle_LibcGettimeofday(void) {
    uint32_t tv = psp_arg(0);
    if (tv) {
        time_t t = time(NULL);
        psp_write32(tv, (uint32_t)t);
        psp_write32(tv + 4, 0);
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* General-purpose I/O pins, wired to the debug board. Nothing is connected. */
static void hle_GetGPI(void) { psp_ret(0); }

/* ---- StdioForUser -------------------------------------------------------- */
/* These return the file descriptors, which sceIoWrite then recognises. */
static void hle_Stdin(void)  { psp_ret(0); }
static void hle_Stdout(void) { psp_ret(1); }
static void hle_Stderr(void) { psp_ret(2); }

/* ---- sceSuspendForUser --------------------------------------------------- */
/* Power management around suspend. Nothing suspends here. */
static void hle_ok(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- LoadExecForUser ----------------------------------------------------- */

static int g_exit_requested;
int psp_exit_requested(void) { return g_exit_requested; }

static void hle_ExitGame(void) {
    /* A game calling this is finished. Recording it rather than terminating
     * the process lets the host report what happened and dump its counters. */
    g_exit_requested = 1;
    fprintf(stderr, "psprecomp: the game called sceKernelExitGame\n");
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RegisterExitCallback(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- ModuleMgrForUser ---------------------------------------------------- */
/* A game-sharing microgame is self-contained and does not load further
 * modules, so these report a plausible identity rather than doing anything.
 * A title that genuinely loads PRXs at run time will need real ones. */
/* These report the id of the one loaded module.
 *
 * A previous note here recorded the opposite -- that returning an id was a
 * "plausible lie" and an error was the truthful answer -- on the strength of a
 * test showing byte-identical output either way. **That test was run while
 * `jal` never assigned `$ra`**, so every non-leaf function in the program was
 * returning through a stale register. A falsification obtained under broken
 * codegen is not a falsification.
 *
 * Re-run after that fix, the two answers differ clearly: returning an id makes
 * the game's own "libc:_getmodreent: no reent structure" diagnostic disappear
 * and drops bad memory accesses from 3 to 0.
 *
 * And an id is the *truthful* answer. The question is "which module owns this
 * address", a self-contained microgame is exactly one module, and the host has
 * loaded it. Reporting 1 states that; reporting UNKNOWN_MODULE denies a module
 * that demonstrably exists. */
#define SCE_KERNEL_ERROR_UNKNOWN_MODULE 0x80020139u
#define PSP_MAIN_MODULE_ID 1u

static void hle_GetModuleId(void)          { psp_ret(PSP_MAIN_MODULE_ID); }
static void hle_GetModuleIdByAddress(void) { psp_ret(PSP_MAIN_MODULE_ID); }
static void hle_ModuleOk(void)             { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- sceCtrl ------------------------------------------------------------- */

static uint32_t g_buttons;
static uint8_t  g_analog_x = 128, g_analog_y = 128;
static uint32_t g_ctrl_frame;
static uint32_t g_latch[4];  /* PSPSDK SceCtrlLatch: make, break, press, release */
static uint32_t g_latch_samples;

/* The window submits samples on a host thread, independently of the guest.
 * Keep sampling and latch consumption atomic, including in inline mode. */
#ifdef _WIN32
static SRWLOCK g_ctrl_lock = SRWLOCK_INIT;
static void ctrl_lock(void) { AcquireSRWLockExclusive(&g_ctrl_lock); }
static void ctrl_unlock(void) { ReleaseSRWLockExclusive(&g_ctrl_lock); }
#else
static atomic_flag g_ctrl_lock = ATOMIC_FLAG_INIT;
static void ctrl_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_ctrl_lock, memory_order_acquire)) {}
}
static void ctrl_unlock(void) {
    atomic_flag_clear_explicit(&g_ctrl_lock, memory_order_release);
}
#endif

static void ctrl_sample(uint32_t buttons) {
    g_latch[0] |= buttons & ~g_buttons;
    g_latch[1] |= g_buttons & ~buttons;
    g_latch[2] |= buttons;
    g_latch[3] |= ~buttons;
    if (g_latch_samples < INT_MAX) g_latch_samples++;
    g_buttons = buttons;
}

void psp_ctrl_set(uint32_t buttons, uint8_t ax, uint8_t ay) {
    ctrl_lock();
    ctrl_sample(buttons);
    g_analog_x = ax;
    g_analog_y = ay;
    ctrl_unlock();
}

static void hle_CtrlSet(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

static void ctrl_latch(int consume) {
    uint32_t out = psp_arg(0), data[4], samples;
    if (!out || !psp_mem_ptr(out, 16)) { psp_ret(0x80010016u); return; }
    ctrl_lock();
    if (consume && !g_latch_samples) {
        /* PSPSDK documents an empty read as blocking until the next sample.
         * Use the existing frame wait rather than inventing a polling thread.
         * A host sample arriving during the wait takes precedence. */
        ctrl_unlock();
        psp_threadman_frame_wait();
        ctrl_lock();
        if (!g_latch_samples) ctrl_sample(g_buttons);
    }
    samples = g_latch_samples;
    memcpy(data, g_latch, sizeof data);
    if (consume) {
        memset(g_latch, 0, sizeof g_latch);
        g_latch_samples = 0;
    }
    ctrl_unlock();
    for (unsigned i = 0; i < 4; i++) psp_write32(out + i * 4, data[i]);
    psp_ret(samples);
}
static void hle_ReadLatch(void) { ctrl_latch(1); }
static void hle_PeekLatch(void) { ctrl_latch(0); }

/* SceCtrlData: u32 timestamp, u32 buttons, u8 lx, u8 ly, then padding to 16. */
static void hle_ReadBufferPositive(void) {
    uint32_t buf = psp_arg(0), count = psp_arg(1);
    if (!count) count = 1;
    ctrl_lock();
    uint32_t frame = g_ctrl_frame, buttons = g_buttons;
    uint8_t ax = g_analog_x, ay = g_analog_y;
    g_ctrl_frame += count;
    ctrl_unlock();
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = buf + i * 16;
        psp_write32(at, frame + i);
        psp_write32(at + 4, buttons);
        psp_write8(at + 8, ax);
        psp_write8(at + 9, ay);
        for (int k = 10; k < 16; k++) psp_write8(at + (uint32_t)k, 0);
    }
    psp_ret(count);
}

/* ---- sceAudio ------------------------------------------------------------ */

#define AUDIO_CHANNELS 8

typedef struct { int reserved; uint32_t samples; uint32_t format; } audio_ch;
static audio_ch g_audio[AUDIO_CHANNELS];
static uint64_t g_audio_blocks;

uint64_t psp_audio_blocks(void) { return g_audio_blocks; }

static void hle_ChReserve(void) {
    if (getenv("PSPRECOMP_TRACE_SAS"))
        fprintf(stderr, "audio: ChReserve ch %d samples %u format %u\n",
                (int)psp_arg(0), psp_arg(1), psp_arg(2));
    /* (channel, samplecount, format) -- channel -1 means "any". */
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0) {
        for (int i = 0; i < AUDIO_CHANNELS; i++)
            if (!g_audio[i].reserved) { ch = i; break; }
    }
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001); return; }
    g_audio[ch].reserved = 1;
    g_audio[ch].samples = psp_arg(1);
    g_audio[ch].format  = psp_arg(2);
    psp_ret((uint32_t)ch);
}

static void hle_ChRelease(void) {
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001); return; }
    g_audio[ch].reserved = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Nothing consumes the samples; the block count is what tells you audio is
 * flowing. */
static void hle_Output(void) {
    g_audio_blocks++;
    psp_ret(psp_arg(0) < AUDIO_CHANNELS ? g_audio[psp_arg(0)].samples : 0);
}

/* The blocking forms hold the caller while the hardware drains a buffer, and
 * that is what paces a game's audio thread. Returning at once lets a mixer
 * loop spin forever and starve every other guest thread.
 *
 * Under real threads the hardware queue is modelled on the host clock: a
 * buffer is due one play time after the last, and the call only waits while
 * the queue is more than a buffer ahead. Waiting a fixed play time per call
 * did not work there. The scheduler is cooperative, each tick of the wait
 * handed the CPU to the game's main thread for a whole frame, and WTF's audio
 * ran at about 10 buffers a second instead of 172; its title waits for a
 * jingle that, by its own sample count, never finished. A thread that falls
 * behind now catches up without waiting, as it would against a real queue.
 * Inline mode keeps the virtual-time wait. */
#if PSPRECOMP_REAL_THREADS
static uint64_t host_now_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static uint64_t g_audio_due_us[AUDIO_CHANNELS];
#endif

static void hle_OutputBlocking(void) {
    uint32_t ch = psp_arg(0);
    uint32_t samples = ch < AUDIO_CHANNELS ? g_audio[ch].samples : 0;
    static int traced;
    if (getenv("PSPRECOMP_TRACE_SAS") && traced++ < 4)
        fprintf(stderr, "audio: OutputBlocking ch %u samples %u buf 0x%08X\n", ch, samples, psp_arg(2));
    hle_Output();
    const uint64_t play_us = (uint64_t)samples * 1000000u / 44100u;
#if PSPRECOMP_REAL_THREADS
    if (ch < AUDIO_CHANNELS) {
        uint64_t now = host_now_us();
        if (g_audio_due_us[ch] < now) g_audio_due_us[ch] = now;   /* underrun */
        g_audio_due_us[ch] += play_us;
        while (host_now_us() + play_us < g_audio_due_us[ch]) psp_threadman_yield();
    }
#else
    psp_threadman_delay(play_us);
#endif
    psp_ret(samples);
}

/* The sample count a channel outputs per call. It is set at reserve time and
 * changed here; ignoring the change left WTF's channel at its reserved length,
 * so each blocking output waited for 4,096 samples while the game mixed 256,
 * and its audio ran 16 times slow. */
static void hle_SetChannelDataLen(void) {
    uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(0x80260003); return; }   /* invalid channel */
    g_audio[ch].samples = psp_arg(1);
    if (getenv("PSPRECOMP_TRACE_SAS"))
        fprintf(stderr, "audio: SetChannelDataLen ch %u samples %u\n", ch, psp_arg(1));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Zero remaining means "ready for more", so a game's audio loop keeps going. */
static void hle_GetChannelRestLength(void) { psp_ret(0); }

void psp_misc_reset(void) {
    /* The interrupt mask is per thread and lives in the scheduler; a reset
     * here cannot reach another thread's bracket, and should not. */
    g_exit_requested = 0;
    ctrl_lock();
    g_buttons = 0;
    g_analog_x = g_analog_y = 128;
    g_ctrl_frame = 0;
    memset(g_latch, 0, sizeof g_latch);
    g_latch_samples = 0;
    ctrl_unlock();
    memset(g_audio, 0, sizeof g_audio);
#if PSPRECOMP_REAL_THREADS
    memset(g_audio_due_us, 0, sizeof g_audio_due_us);
#endif
    g_audio_blocks = 0;
}

void psp_misc_init(void) { psp_misc_reset(); }

void psp_misc_register(void) {
    psp_hle_register(0x092968F4, "Kernel_Library", "sceKernelCpuSuspendIntr", hle_CpuSuspendIntr);
    psp_hle_register(0x5F10D406, "Kernel_Library", "sceKernelCpuResumeIntr",  hle_CpuResumeIntr);

    psp_hle_register(0x79D1C3FA, "UtilsForUser", "sceKernelDcacheWritebackAll",           hle_CacheOp);
    psp_hle_register(0xB435DEC5, "UtilsForUser", "sceKernelDcacheWritebackInvalidateAll", hle_CacheOp);
    psp_hle_register(0x27CC57F0, "UtilsForUser", "sceKernelLibcTime",         hle_LibcTime);
    psp_hle_register(0x91E4F6A7, "UtilsForUser", "sceKernelLibcClock",        hle_LibcClock);
    psp_hle_register(0x71EC4271, "UtilsForUser", "sceKernelLibcGettimeofday", hle_LibcGettimeofday);
    psp_hle_register(0x37FB5C42, "UtilsForUser", "sceKernelGetGPI",           hle_GetGPI);

    psp_hle_register(0x172D316E, "StdioForUser", "sceKernelStdin",  hle_Stdin);
    psp_hle_register(0xA6BAB2E9, "StdioForUser", "sceKernelStdout", hle_Stdout);
    psp_hle_register(0xF78BA90A, "StdioForUser", "sceKernelStderr", hle_Stderr);

    psp_hle_register(0xEADB1BD7, "sceSuspendForUser", "sceKernelPowerLock",   hle_ok);
    psp_hle_register(0x3AEE7261, "sceSuspendForUser", "sceKernelPowerUnlock", hle_ok);
    psp_hle_register(0x090CCB3F, "sceSuspendForUser", "sceKernelPowerTick",   hle_ok);

    psp_hle_register(0x05572A5F, "LoadExecForUser", "sceKernelExitGame",             hle_ExitGame);
    psp_hle_register(0x4AC57943, "LoadExecForUser", "sceKernelRegisterExitCallback", hle_RegisterExitCallback);

    /* Observed but unidentified. The game calls this three times on its
     * heap-setup path, and an unimplemented call returns 0 -- which for a
     * module query means "no module" and makes heap establishment fail.
     * Eighteen plausible ModuleMgr names were tried against SHA-1 with no
     * match, so the name is genuinely unknown and is not invented here. */
    psp_hle_register_unnamed(0xF9275D98, "ModuleMgrForUser", hle_GetModuleId);
    psp_hle_register(0xF0A26395, "ModuleMgrForUser", "sceKernelGetModuleId",          hle_GetModuleId);
    psp_hle_register(0xD8B73127, "ModuleMgrForUser", "sceKernelGetModuleIdByAddress", hle_GetModuleIdByAddress);
    psp_hle_register(0x50F0C1EC, "ModuleMgrForUser", "sceKernelStartModule",          hle_ModuleOk);
    psp_hle_register(0xD1FF982A, "ModuleMgrForUser", "sceKernelStopModule",           hle_ModuleOk);
    psp_hle_register(0x2E0911AA, "ModuleMgrForUser", "sceKernelUnloadModule",         hle_ModuleOk);

    psp_hle_register(0x1F4011E6, "sceCtrl", "sceCtrlSetSamplingMode",     hle_CtrlSet);
    psp_hle_register(0x6A2774F3, "sceCtrl", "sceCtrlSetSamplingCycle",    hle_CtrlSet);
    psp_hle_register(0x1F803938, "sceCtrl", "sceCtrlReadBufferPositive",  hle_ReadBufferPositive);
    psp_hle_register(0x0B588501, "sceCtrl", "sceCtrlReadLatch",           hle_ReadLatch);
    psp_hle_register(0xB1D0E5CD, "sceCtrl", "sceCtrlPeekLatch",           hle_PeekLatch);

    psp_hle_register(0x5EC81C55, "sceAudio", "sceAudioChReserve",            hle_ChReserve);
    psp_hle_register(0x6FC46853, "sceAudio", "sceAudioChRelease",            hle_ChRelease);
    psp_hle_register(0x136CAF51, "sceAudio", "sceAudioOutputBlocking",       hle_OutputBlocking);
    psp_hle_register(0x13F592BC, "sceAudio", "sceAudioOutputPannedBlocking", hle_OutputBlocking);
    psp_hle_register(0xE2D56B2D, "sceAudio", "sceAudioOutputPanned",         hle_Output);
    psp_hle_register(0xB011922F, "sceAudio", "sceAudioGetChannelRestLength", hle_GetChannelRestLength);
    psp_hle_register(0xCB2E439E, "sceAudio", "sceAudioSetChannelDataLen",    hle_SetChannelDataLen);
    psp_hle_register(0x95FD0C2D, "sceAudio", "sceAudioChangeChannelConfig",  hle_ok);
    psp_hle_register(0xB7E1D8E7, "sceAudio", "sceAudioChangeChannelVolume",  hle_ok);
}
