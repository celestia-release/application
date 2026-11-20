// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * This file is part of Celestia/application/app_main.
 *
 * App main entry: registers framework handles, loads settings.json (program, launch_mode, modules),
 * runs app_main_run() to load SOs (e.g. nova), then app_main_start_framework_modules() for UI or headless.
 * Config path: APP_SETTINGS_CONFIG (legacy APP_RUNTIME_CONFIG), or --config /path,
 * or default settings.json / /etc/config/settings.json (legacy app_runtime.json / app_user.json).
 */
#define LOG_TAG "APP MAIN"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <sys/syscall.h>   /* SYS_gettid: uclibc has no gettid() wrapper */

#include "config.h"
#include "hardware.h"
#include "log.h"
#include "runtime.h"
#include "system_time.h"
#include "system_error.h"
#include "network.h"
#include "multimedia.h"
#include "notification.h"
#ifdef CONFIG_APP_NEBULA
#include "nebula_protocol.h"
#endif
#ifdef CONFIG_APP_RTSP
#include "rtsp_server.h"
#endif

#include "app_config.h"
#include "app_main.h"
#include "app_main_error.h"
#if defined(KERNEL_AOS)
#include "system_thread.h"
#endif
/* Last-commit rename: GUI_LUNA/NOVA -> PACKAGES_WEARABLE/TABLET. Accept both. */
#if defined(CONFIG_APP_PACKAGES_WEARABLE) || defined(CONFIG_APP_GUI_LUNA) || \
    defined(CONFIG_APP_PACKAGES_TABLET) || defined(CONFIG_APP_GUI_NOVA)
#include "app_gui.h"
#endif
#if defined(CONFIG_APP_PACKAGES_TABLET) || defined(CONFIG_APP_GUI_NOVA)
#include "tablet_gui.h"  /* tablet_run() */
#endif

static const char *get_config_path(int argc, char **argv)
{
    const char *path = getenv("APP_SETTINGS_CONFIG");
    if (!path || !path[0])
        path = getenv("APP_RUNTIME_CONFIG"); /* legacy alias */
    if (path && path[0])
        return path;
    for (int i = 1; i + 1 < argc; i++) {
        if (argv[i] && strcmp(argv[i], "--config") == 0 && argv[i + 1])
            return argv[i + 1];
    }
    return NULL; /* app_main_load_config(NULL) uses default */
}

/* [CAPTRACE] Crash diagnostics: resolve faulting PC/LR to symbol names via dladdr so a
 * SIGSEGV/SIGBUS shows exactly which function (and .so) crashed on the serial log. */
static void crash_log_addr(const char *tag, unsigned long addr)
{
    Dl_info info;
    if (addr && dladdr((void *)addr, &info) && info.dli_fname) {
        unsigned long sym_off = info.dli_saddr ? (addr - (unsigned long)info.dli_saddr) : 0;
        unsigned long file_off = info.dli_fbase ? (addr - (unsigned long)info.dli_fbase) : 0;
        log_error("[CRASH] %s=0x%lx %s+0x%lx (%s file_off=0x%lx)",
                  tag, addr, info.dli_sname ? info.dli_sname : "?", sym_off,
                  info.dli_fname, file_off);
    } else {
        log_error("[CRASH] %s=0x%lx <unresolved>", tag, addr);
    }
}

/*
 * [CAPTRACE] Best-effort stack backtrace.
 *
 * uclibc has no usable backtrace(), and -O2 ARM builds omit frame pointers in most objects,
 * so the fp chain cannot be walked reliably. Scan upwards from sp instead (stacks grow down,
 * so higher addresses are mapped up to the stack base) and report every word that dladdr()
 * resolves inside a loaded object. That over-reports — stale words from earlier frames show
 * up too — but it names the .so and the functions on the path, which pc/lr alone do not.
 *
 * Raw hex is printed before symbolisation: dladdr() takes a loader lock and is not
 * async-signal-safe, so if it wedges or faults the addresses are already on the wire.
 */
#define CRASH_STACK_WORDS 1024   /* 4KB above sp */
#define CRASH_STACK_HITS  24

static void crash_log_stack(unsigned long sp)
{
    const unsigned long *w = (const unsigned long *)(sp & ~3UL);
    unsigned long hits[CRASH_STACK_HITS];
    int n = 0;

    if (sp == 0)
        return;

    for (int i = 0; i < CRASH_STACK_WORDS && n < CRASH_STACK_HITS; i++) {
        unsigned long v = w[i];
        Dl_info info;
        /* Cheap pre-filter: user-space text is well above the first page and below the
         * kernel split; skips the bulk of the words without paying for dladdr(). */
        if (v < 0x1000UL || v >= 0xc0000000UL)
            continue;
        if (dladdr((void *)v, &info) && info.dli_fname && info.dli_sname)
            hits[n++] = v;
    }

    for (int i = 0; i < n; i++)
        log_error("[CRASH] stack[%d]=0x%lx", i, hits[i]);
    for (int i = 0; i < n; i++) {
        char tag[16];
        snprintf(tag, sizeof(tag), "stack[%d]", i);
        crash_log_addr(tag, hits[i]);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *uctx)
{
    unsigned long pc = 0, lr = 0, sp = 0, fp = 0;
    /*
     * Re-entry guard. The handler itself dereferences suspect memory (dladdr on stack words),
     * and it is still installed while doing so, so a nested fault would recurse until the
     * stack blows and no log at all survives. On the second entry, bail straight to default.
     */
    static volatile sig_atomic_t in_handler = 0;
    if (in_handler) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    in_handler = 1;

#if defined(__arm__)
    ucontext_t *uc = (ucontext_t *)uctx;
    if (uc) {
        pc = uc->uc_mcontext.arm_pc;
        lr = uc->uc_mcontext.arm_lr;
        sp = uc->uc_mcontext.arm_sp;
        fp = uc->uc_mcontext.arm_fp;
    }
#elif defined(__aarch64__)
    ucontext_t *uc = (ucontext_t *)uctx;
    if (uc) {
        pc = (unsigned long)uc->uc_mcontext.pc;
        lr = (unsigned long)uc->uc_mcontext.regs[30];
        sp = (unsigned long)uc->uc_mcontext.sp;
        fp = (unsigned long)uc->uc_mcontext.regs[29];
    }
#elif defined(__riscv)
    /* Nova/AV200 (riscv64, musl): without this the whole register block
     * printed zeros and every crash looked like "jumped to NULL". */
    ucontext_t *uc = (ucontext_t *)uctx;
    if (uc) {
        pc = uc->uc_mcontext.__gregs[REG_PC];
        lr = uc->uc_mcontext.__gregs[REG_RA];
        sp = uc->uc_mcontext.__gregs[REG_SP];
        fp = uc->uc_mcontext.__gregs[REG_S0];
    }
#else
    (void)uctx;
#endif
    log_error("[CRASH] ====== signal=%d code=%d fault_addr=%p ======",
              sig, si ? si->si_code : 0, si ? si->si_addr : NULL);
    /* Which thread: the fault is usually in a worker (capture, cloud, p2p), not main. */
    log_error("[CRASH] pid=%d tid=%ld", (int)getpid(), syscall(SYS_gettid));
    /* Raw registers before dladdr. Symbol lookup takes the loader lock and
     * can fault; this log died on that second bus error and lost pc/lr. */
    log_error("[CRASH] pc=0x%lx lr=0x%lx sp=0x%lx fp=0x%lx", pc, lr, sp, fp);
#if defined(__arm__)
    /*
     * Argument registers at the faulting instruction. For a fault inside free()/malloc()
     * r0 still holds the pointer that was passed in: a small integer or an unmapped value
     * there means a bad pointer reached free(), while a plausible heap pointer means the
     * chunk header itself was corrupted by an earlier overrun. That distinction cannot be
     * made from fault_addr alone.
     */
    if (uc) {
        log_error("[CRASH] r0=0x%lx r1=0x%lx r2=0x%lx r3=0x%lx",
                  (unsigned long)uc->uc_mcontext.arm_r0, (unsigned long)uc->uc_mcontext.arm_r1,
                  (unsigned long)uc->uc_mcontext.arm_r2, (unsigned long)uc->uc_mcontext.arm_r3);
        log_error("[CRASH] r4=0x%lx r5=0x%lx r6=0x%lx r7=0x%lx",
                  (unsigned long)uc->uc_mcontext.arm_r4, (unsigned long)uc->uc_mcontext.arm_r5,
                  (unsigned long)uc->uc_mcontext.arm_r6, (unsigned long)uc->uc_mcontext.arm_r7);
        log_error("[CRASH] r8=0x%lx r9=0x%lx r10=0x%lx ip=0x%lx",
                  (unsigned long)uc->uc_mcontext.arm_r8, (unsigned long)uc->uc_mcontext.arm_r9,
                  (unsigned long)uc->uc_mcontext.arm_r10, (unsigned long)uc->uc_mcontext.arm_ip);
    }
#endif
    crash_log_addr("pc", pc);
    crash_log_addr("lr", lr);
    log_error("[CRASH] sp=0x%lx fp=0x%lx uctx=%p", sp, fp, uctx);
    crash_log_stack(sp);
    /* Restore default disposition and re-raise so the usual "Segmentation fault" still prints
     * and app.sh can restart the main binary as before. */
    signal(sig, SIG_DFL);
    raise(sig);
}


static void install_crash_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    log_info("[CAPTRACE] crash handler installed (SIGSEGV/SIGBUS/SIGABRT/SIGILL)");
}

#if defined(KERNEL_AOS)
typedef struct {
    int argc;
    char **argv;
    int result;
} app_main_aos_ctx_t;

static int app_main_entry(int argc, char **argv);

static int app_main_aos_worker(void *param)
{
    app_main_aos_ctx_t *ctx = (app_main_aos_ctx_t *)param;

    ctx->result = app_main_entry(ctx->argc, ctx->argv);
    return 0;
}
#endif

static int app_main_entry(int argc, char **argv)
{
    long long start_time = system_get_sys_run_time64();

    runtime_bootstrap_t *runtime;
    runtime_bootstrap_open(&runtime);
    if (runtime) {
        runtime->load_config(NULL);
        app_main_register_runtime(runtime);
    }

    app_main_init();
    if (app_main_load_config(get_config_path(argc, argv)) != 0) {
        log_error("app_main_load_config failed; continuing with default (headless, no modules)");
    }

    /* dlopen + module_init only; module_start runs after framework handles are ready. */
    app_main_run();

    int headless = !app_main_get_launch_mode_ui();
    const char *gui_config = app_main_get_gui_config_path();
    log_info("start framework modules, mode=%s", headless ? "headless/ipcamera" : "ui");
    if (app_main_start_framework_modules(headless, gui_config) != APP_MAIN_OK)
        return APP_MAIN_ERROR_FILE_OPEN_FAILED;
    log_info("framework modules started, elapsed=%lld", system_get_sys_run_time64() - start_time);

#if defined(CONFIG_APP_PACKAGES_WEARABLE) || defined(CONFIG_APP_GUI_LUNA)
    app_main_start_so_modules(); /* non-blocking for UI companion SOs */
    app_gui_run(); /* does not return */
#elif defined(CONFIG_APP_PACKAGES_TABLET) || defined(CONFIG_APP_GUI_NOVA)
    app_main_start_so_modules();
    tablet_run(); /* exits via exit(0) after graceful teardown when shutdown requested */
#else
    /* ipcamera / headless: module_start must block to keep ipc_main alive. */
    log_warn("no UI package linked (PACKAGES_WEARABLE/TABLET unset); running SO modules only");
    app_main_start_so_modules();
#endif
    return APP_MAIN_OK;
}

int main(int argc, char **argv)
{
    install_crash_handler();

    if (app_main_platform_startup() != APP_MAIN_OK) {
        log_error("Failed to initialize platform (framework, input)");
        return APP_MAIN_ERROR_INVALID_CONFIG;
    }

#if defined(KERNEL_AOS)
    /*
     * AliOS aos_app user stack is 8KB (0x2000). multimedia_open, network_init,
     * and ipc module_start exceed that depth; worker pthreads use 100KB (thread.c).
     */
    static app_main_aos_ctx_t ctx;
    int th = 0;

    ctx.argc = argc;
    ctx.argv = argv;
    ctx.result = APP_MAIN_ERROR_INVALID_CONFIG;
    if (thread_create(&th, app_main_aos_worker, &ctx, "appMain", 0, 0) != THREAD_OK) {
        log_error("appMain thread_create failed");
        return APP_MAIN_ERROR_INVALID_CONFIG;
    }
    thread_destroy(th);
    return ctx.result;
#else
    return app_main_entry(argc, argv);
#endif
}
