/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

#define BTSTACK_FILE__ "btstack_run_loop_amigaos.c"

/*
 * BTstack run loop for AmigaOS 4.
 *
 * Replaces btstack_run_loop_posix, which cannot be used here: clib4's select()
 * does not implement its timeout as a sleep when there is no valid descriptor
 * (and the pipes the posix run loop creates for its wake-up mechanism are not
 * usable either). The result was a run loop that executed exactly one timer -
 * the one already overdue when it started - and then blocked in select() for
 * ever, with no timers, no CTRL-C and no USB polling.
 *
 * This implementation uses the native Amiga primitives instead:
 *
 *  - timer.device (UNIT_MICROHZ) provides the timeout for the wait
 *  - Wait() sleeps on: the timer signal, any MsgPort registered by the
 *    transport (USB transfer completion), and SIGBREAKF_CTRL_C
 *  - no polling, no busy loop: the run loop wakes up when there is work
 *
 * Data sources with a file descriptor are not supported (nothing in this port
 * uses them); DATA_SOURCE_CALLBACK_POLL sources are polled every iteration.
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <exec/exectags.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>

/* AmigaOS 4 SDK defines UNUSED as __attribute__((unused)) */
#undef UNUSED

/* With __USE_INLINE__ these are macros expanding to ITimer->x(); we keep our own
 * interface pointer instead of relying on a global ITimer, so call through it */
#undef GetSysTime
#undef SubTime

#include "btstack_config.h"
#include "btstack_debug.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_amigaos.h"
#include "btstack_util.h"

/* timer.device, used for the timeout of Wait() and as time base */
static struct MsgPort        * timer_port;
static struct TimeRequest    * timer_request;
static struct Device         * timer_base;
static struct TimerIFace     * itimer;
static bool                    timer_request_pending;

/* signal mask of the MsgPorts registered by the transport */
static uint32                  extra_signal_mask;

/* wake-up signal for execute_on_main_thread */
static struct Task           * main_task;
static uint32                  wakeup_signal_mask;

static bool                    run_loop_exit_requested;

/* called on the first CTRL-C to allow a graceful shutdown; a second CTRL-C
 * leaves the run loop unconditionally */
static void                 (* break_handler)(void);
static bool                    break_handler_called;
/* CTRL-C reported by another source than the signal, e.g. the console in RAW
 * mode delivering 0x03 as a character */
static bool                    break_pending;
/* one key press may arrive both as signal and as character: collapse breaks
 * that happen within this window into one */
#define BREAK_DEBOUNCE_MS      1000
static uint32_t                break_last_time_ms;
static bool                    break_seen;

static struct TimeVal          start_time;

/* -------------------------------------------------------------------------- */

void btstack_run_loop_amigaos_add_signal_mask(uint32 mask){
    extra_signal_mask |= mask;
}

void btstack_run_loop_amigaos_remove_signal_mask(uint32 mask){
    extra_signal_mask &= ~mask;
}

void btstack_run_loop_amigaos_set_break_handler(void (*handler)(void)){
    break_handler = handler;
}

void btstack_run_loop_amigaos_trigger_break(void){
    break_pending = true;
    /* wake the run loop up so the break is handled right away */
    if (main_task != NULL){
        Signal(main_task, wakeup_signal_mask);
    }
}

static uint32_t btstack_run_loop_amigaos_get_time_ms(void){
    if (itimer == NULL) return 0;
    struct TimeVal now;
    itimer->GetSysTime(&now);
    itimer->SubTime(&now, &start_time);
    return (uint32_t) ((now.Seconds * 1000) + (now.Microseconds / 1000));
}

uint32_t btstack_run_loop_amigaos_get_time_us(void){
    if (itimer == NULL) return 0;
    struct TimeVal now;
    itimer->GetSysTime(&now);
    itimer->SubTime(&now, &start_time);
    return (uint32_t) ((now.Seconds * 1000000) + now.Microseconds);
}

static void btstack_run_loop_amigaos_set_timer(btstack_timer_source_t * ts, uint32_t timeout_in_ms){
    ts->timeout = btstack_run_loop_amigaos_get_time_ms() + timeout_in_ms;
}

/* start the timeout for the next Wait() */
static void btstack_run_loop_amigaos_start_timeout(uint32_t timeout_ms){
    if (timer_request == NULL) return;
    timer_request->Request.io_Command = TR_ADDREQUEST;
    timer_request->Time.Seconds       = timeout_ms / 1000;
    timer_request->Time.Microseconds  = (timeout_ms % 1000) * 1000;
    SendIO((struct IORequest *) timer_request);
    timer_request_pending = true;
}

static void btstack_run_loop_amigaos_stop_timeout(void){
    if ((timer_request == NULL) || (timer_request_pending == false)) return;
    if (CheckIO((struct IORequest *) timer_request) == NULL){
        AbortIO((struct IORequest *) timer_request);
    }
    WaitIO((struct IORequest *) timer_request);
    timer_request_pending = false;

    /*
     * Clear the timer port's signal.
     *
     * When Wait() returned for another signal - a completed USB transfer, say -
     * the timer is still running, so we abort it here. That abort replies the
     * request, which sets the port's signal bit, and WaitIO() only removes the
     * message: the bit stays set. The next Wait() would then return immediately,
     * abort the timer again, set the bit again... a spin at 100% CPU, starting
     * with the first USB completion. The port is drained at this point, so
     * dropping the signal cannot lose anything.
     */
    SetSignal(0, 1UL << timer_port->mp_SigBit);
}

static void btstack_run_loop_amigaos_execute(void){

    log_info("AmigaOS run loop using timer.device and Wait()");

    run_loop_exit_requested = false;

    while (run_loop_exit_requested == false){

        /* run callbacks posted with execute_on_main_thread */
        btstack_run_loop_base_execute_callbacks();

        /* poll data sources that asked to be polled */
        btstack_run_loop_base_poll_data_sources();

        /* time until the next timer expires, -1 if there is no timer */
        uint32_t now_ms = btstack_run_loop_amigaos_get_time_ms();
        int32_t  delta_ms = btstack_run_loop_base_get_time_until_timeout(now_ms);

        uint32 signals_to_wait = extra_signal_mask | wakeup_signal_mask | SIGBREAKF_CTRL_C;
        if (delta_ms >= 0){
            /* wake up at the latest when the timer expires */
            btstack_run_loop_amigaos_start_timeout((uint32_t) delta_ms);
            signals_to_wait |= (1UL << timer_port->mp_SigBit);
        }

        uint32 signals = Wait(signals_to_wait);

        if (delta_ms >= 0){
            btstack_run_loop_amigaos_stop_timeout();
        }

        /* CTRL-C: either as signal, or reported as a character by the console */
        bool break_requested = ((signals & SIGBREAKF_CTRL_C) != 0) || break_pending;
        break_pending = false;
        if (break_requested){
            /* a single key press can arrive through both channels - count it once */
            uint32_t break_time_ms = btstack_run_loop_amigaos_get_time_ms();
            if (break_seen && ((break_time_ms - break_last_time_ms) < BREAK_DEBOUNCE_MS)){
                break_requested = false;
            }
            break_last_time_ms = break_time_ms;
            break_seen = true;
        }

        if (break_requested){
            if ((break_handler != NULL) && (break_handler_called == false)){
                /* graceful shutdown: power off HCI and keep running until the
                 * stack reports HCI_STATE_OFF (which triggers exit) */
                break_handler_called = true;
                log_info("CTRL-C received, starting graceful shutdown");
                printf("CTRL-C received, shutting down Bluetooth (press CTRL-C again to force)\n");
                (*break_handler)();
            } else {
                log_info("CTRL-C received, leaving run loop");
                printf("CTRL-C received, leaving run loop\n");
                run_loop_exit_requested = true;
            }
        }

        /* drain the messages of the registered MsgPorts: the transport checks
         * its transfers from a timer / data source, we only need the wake-up.
         * Messages are consumed by WaitIO() in the transport. */

        /* process expired timers */
        btstack_run_loop_base_process_timers(btstack_run_loop_amigaos_get_time_ms());
    }
}

static void btstack_run_loop_amigaos_trigger_exit(void){
    run_loop_exit_requested = true;
    if (main_task != NULL){
        Signal(main_task, wakeup_signal_mask);
    }
}

static void btstack_run_loop_amigaos_execute_on_main_thread(btstack_context_callback_registration_t * callback_registration){
    btstack_run_loop_base_add_callback(callback_registration);
    /* wake the run loop up so the callback is executed right away */
    if (main_task != NULL){
        Signal(main_task, wakeup_signal_mask);
    }
}

static void btstack_run_loop_amigaos_poll_data_sources_from_irq(void){
    if (main_task != NULL){
        Signal(main_task, wakeup_signal_mask);
    }
}

static void btstack_run_loop_amigaos_init(void){
    btstack_run_loop_base_init();

    main_task = FindTask(NULL);

    /* a dedicated signal to wake the run loop up */
    BYTE signal_bit = AllocSignal(-1);
    if (signal_bit == -1){
        log_error("AllocSignal failed, using SIGBREAKF_CTRL_F");
        wakeup_signal_mask = SIGBREAKF_CTRL_F;
    } else {
        wakeup_signal_mask = 1UL << signal_bit;
    }

    /* open timer.device for the wait timeout and as time base */
    timer_port = AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (timer_port == NULL){
        log_error("cannot create timer MsgPort");
        return;
    }
    timer_request = AllocSysObjectTags(ASOT_IOREQUEST,
                                      ASOIOR_Size,      sizeof(struct TimeRequest),
                                      ASOIOR_ReplyPort, timer_port,
                                      TAG_END);
    if (timer_request == NULL){
        log_error("cannot create timer IORequest");
        return;
    }
    if (OpenDevice(TIMERNAME, UNIT_MICROHZ, (struct IORequest *) timer_request, 0) != 0){
        log_error("cannot open " TIMERNAME);
        FreeSysObject(ASOT_IOREQUEST, timer_request);
        timer_request = NULL;
        return;
    }
    timer_base = timer_request->Request.io_Device;
    itimer = (struct TimerIFace *) GetInterface((struct Library *) timer_base, "main", 1, NULL);
    if (itimer == NULL){
        log_error("cannot get timer.device interface");
        return;
    }

    /* time base for get_time_ms */
    itimer->GetSysTime(&start_time);
}

void btstack_run_loop_amigaos_deinit(void){
    log_info("run loop deinit");
    btstack_run_loop_amigaos_stop_timeout();
    if (itimer != NULL){
        DropInterface((struct Interface *) itimer);
        itimer = NULL;
    }
    if (timer_request != NULL){
        CloseDevice((struct IORequest *) timer_request);
        FreeSysObject(ASOT_IOREQUEST, timer_request);
        timer_request = NULL;
    }
    if (timer_port != NULL){
        /* drain anything still queued before the port goes away */
        struct Message * msg;
        while ((msg = GetMsg(timer_port)) != NULL){}
        UNUSED(msg);
        SetSignal(0, 1UL << timer_port->mp_SigBit);
        FreeSysObject(ASOT_PORT, timer_port);
        timer_port = NULL;
    }
    if ((wakeup_signal_mask != 0) && (wakeup_signal_mask != SIGBREAKF_CTRL_F)){
        uint32 mask = wakeup_signal_mask;
        int8 bit;
        for (bit = 0; bit < 32; bit++){
            if ((mask >> bit) == 1) break;
        }
        FreeSignal(bit);
        wakeup_signal_mask = 0;
    }
    main_task = NULL;
    log_info("run loop deinit done");
}

static const btstack_run_loop_t btstack_run_loop_amigaos = {
    &btstack_run_loop_amigaos_init,
    &btstack_run_loop_base_add_data_source,
    &btstack_run_loop_base_remove_data_source,
    &btstack_run_loop_base_enable_data_source_callbacks,
    &btstack_run_loop_base_disable_data_source_callbacks,
    &btstack_run_loop_amigaos_set_timer,
    &btstack_run_loop_base_add_timer,
    &btstack_run_loop_base_remove_timer,
    &btstack_run_loop_amigaos_execute,
    &btstack_run_loop_base_dump_timer,
    &btstack_run_loop_amigaos_get_time_ms,
    &btstack_run_loop_amigaos_poll_data_sources_from_irq,
    &btstack_run_loop_amigaos_execute_on_main_thread,
    &btstack_run_loop_amigaos_trigger_exit,
};

const btstack_run_loop_t * btstack_run_loop_amigaos_get_instance(void){
    return &btstack_run_loop_amigaos;
}
