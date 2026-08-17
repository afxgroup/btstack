/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

#define BTSTACK_FILE__ "btstack_stdin_amigaos.c"

/*
 * Console input for AmigaOS 4.
 *
 * Replaces btstack_stdin_posix, which cannot be used here: it registers stdin as
 * a file descriptor data source and relies on the posix run loop's select(),
 * which does not work on AmigaOS 4 (see btstack_run_loop_amigaos.c).
 *
 * Instead the console file handle is switched to RAW mode - so single keystrokes
 * are delivered without waiting for Enter, and without echo, same as the
 * ICANON/ECHO handling of the posix version - and polled from a run loop timer
 * using WaitForChar() with a zero timeout, which never blocks.
 *
 * RAW mode is restored on btstack_stdin_reset() and via atexit(), so the shell
 * is not left in raw mode if an example exits without cleaning up.
 */

#include <stdio.h>
#include <stdlib.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* AmigaOS 4 SDK defines UNUSED as __attribute__((unused)) */
#undef UNUSED

#include "btstack_config.h"
#include "btstack_debug.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_amigaos.h"
#include "btstack_stdin.h"

/* how often the console is checked for new keystrokes */
#define STDIN_POLL_INTERVAL_MS 50

/* characters read per poll, so pasted input or key repeat is not throttled */
#define STDIN_MAX_CHARS_PER_POLL 16

static btstack_timer_source_t stdin_timer;
static void               (* stdin_handler)(char c);
static BPTR                  stdin_fh;
static bool                  stdin_interactive;
static bool                  raw_mode_active;
static bool                  activated;
static bool                  atexit_registered;

static void btstack_stdin_amigaos_restore_mode(void){
    if (raw_mode_active == false) return;
    SetMode(stdin_fh, DOSFALSE);
    raw_mode_active = false;
}

static void btstack_stdin_amigaos_process(btstack_timer_source_t * ts){

    uint8_t count;
    for (count = 0; count < STDIN_MAX_CHARS_PER_POLL; count++){
        /* WaitForChar() with timeout 0 reports whether a keystroke is pending
         * without blocking, but only works on an interactive handle. With input
         * redirected from a file, FGetC() returns data or EOF right away. */
        if (stdin_interactive && (WaitForChar(stdin_fh, 0) == DOSFALSE)) break;
        LONG c = FGetC(stdin_fh);
        if (c == -1) break;

        /* In RAW mode the console delivers CTRL-C as a character. Don't pass it
         * to the application - it must keep meaning "quit", exactly as when it
         * arrives as SIGBREAKF_CTRL_C. */
        if (c == 0x03){
            log_info("stdin: CTRL-C");
            btstack_run_loop_amigaos_trigger_break();
            continue;
        }

        if (stdin_handler == NULL) continue;
#ifdef ENABLE_BTSTACK_STDIN_LOGGING
        log_info("stdin: %c", (char) c);
#endif
        (*stdin_handler)((char) c);
        /* the handler may have called btstack_stdin_reset() */
        if (activated == false) return;
    }

    btstack_run_loop_set_timer(ts, STDIN_POLL_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

void btstack_stdin_setup(void (*handler)(char c)){

    if (activated) return;

    stdin_fh = Input();
    if (stdin_fh == ZERO){
        log_error("btstack_stdin_setup: no console input handle");
        return;
    }

    stdin_interactive = (IsInteractive(stdin_fh) == DOSTRUE);

    /* RAW mode: deliver keys immediately, no line editing, no echo - the
     * equivalent of clearing ICANON/ECHO in the posix implementation.
     * Only meaningful for a console handle. */
    if (stdin_interactive){
        if (SetMode(stdin_fh, DOSTRUE) == DOSTRUE){
            raw_mode_active = true;
        } else {
            log_info("btstack_stdin_setup: cannot switch console to RAW mode");
        }
    }

    if (atexit_registered == false){
        atexit(&btstack_stdin_amigaos_restore_mode);
        atexit_registered = true;
    }

    stdin_handler = handler;
    activated = true;

    btstack_run_loop_set_timer_handler(&stdin_timer, &btstack_stdin_amigaos_process);
    btstack_run_loop_set_timer(&stdin_timer, STDIN_POLL_INTERVAL_MS);
    btstack_run_loop_add_timer(&stdin_timer);
}

void btstack_stdin_reset(void){

    if (activated == false) return;
    activated = false;

    btstack_run_loop_remove_timer(&stdin_timer);
    stdin_handler = NULL;

    btstack_stdin_amigaos_restore_mode();
}
