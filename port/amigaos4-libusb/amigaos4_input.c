/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

#define BTSTACK_FILE__ "amigaos4_input.c"

/*
 * Feed mouse events into AmigaOS 4.
 *
 * Events are written to input.device with IND_WRITEEVENT, so they enter the
 * normal input stream: Intuition, Workbench and every application see them as
 * coming from a real mouse. No driver and no input handler needed.
 *
 * NEVER write to the console from here, or from anything the run loop calls
 * while acting as the system mouse. The shell window is an Intuition window and
 * printf() goes through the console handler, which needs Intuition - and
 * Intuition is busy for as long as it is dragging or sizing a window, waiting
 * for the button release that only we can deliver. Printing there deadlocks the
 * machine: we block, stop reading USB, never send the release, and Intuition
 * waits for us forever. Diagnostics therefore go to the serial debug output.
 *
 * Movement is reported relative (IEQUALIFIER_RELATIVEMOUSE), which is what a HID
 * mouse delivers anyway, so the pointer keeps following the system's own
 * acceleration and screen limits.
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <exec/exectags.h>
#include <exec/io.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <libraries/keymap.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>

/* AmigaOS 4 SDK defines UNUSED as __attribute__((unused)) */
#undef UNUSED

/* With __USE_INLINE__ these expand to ITimer->x(); we keep our own interface
 * pointer instead of relying on a global ITimer */
#undef GetSysTime

#include "amigaos4_input.h"
#include "btstack_debug.h"
#include "bt_log.h"

static struct MsgPort  * input_port;
static struct IOStdReq * input_req;      /* template, used to open the device */
static struct Device   * input_device;
static bool              input_open;

/* timer.device, only used to time stamp the events and to pace movement */
static struct MsgPort     * timer_port;
static struct TimeRequest * timer_req;
static struct TimerIFace  * itimer;

/*
 * Events are written with SendIO, never DoIO.
 *
 * IND_WRITEEVENT runs the event through the whole input handler chain, Intuition
 * included, and DoIO() blocks until that is done. While Intuition is busy - it
 * only takes dragging a window border to get there - the request does not
 * complete, our run loop stops, and the USB transfers are no longer read: the
 * mouse appears to freeze until something else moves Intuition along.
 *
 * So each event gets its own request and buffer (the device may still reference
 * the event after the call returns), is sent asynchronously, and the replies are
 * collected from the run loop by amigaos4_input_poll().
 */
/*
 * One request in flight at a time. input.device is not documented to accept
 * concurrent IND_WRITEEVENT requests, and feeding it several at once is a prime
 * suspect for wedging it - the queue below is what decouples us from it, not
 * concurrency.
 */
#define INPUT_SLOT_COUNT 1

/*
 * Do not emit movement faster than this. Every movement event makes Intuition
 * redo whatever it is tracking - resizing a window means layer clipping and a
 * border redraw - and a HID mouse reporting at 130 Hz asks for far more than a
 * normal mouse ever does. Movement coalesces while waiting, so nothing is lost.
 */
#define INPUT_MOVE_INTERVAL_US 16000

typedef struct {
    struct IOStdReq  * req;
    struct InputEvent  event;
    bool               in_flight;
} input_slot_t;

static input_slot_t input_slots[INPUT_SLOT_COUNT];

/*
 * Events that found no free slot wait here. Movement coalesces into the last
 * queued movement, so continuous mouse motion during a long stall occupies a
 * single entry instead of growing without bound - and no distance is lost.
 */
#define INPUT_QUEUE_SIZE 32

typedef struct {
    uint8_t  class;
    uint16_t code;
    uint16_t qualifier;
    int16_t  x;
    int16_t  y;
} input_queued_event_t;

/*
 * The two previously pressed keys travel with every key event: that is how
 * keymap.library composes dead keys, so without them accented characters do not
 * work on the layouts that need them.
 */
static uint16_t key_prev1_code;
static uint16_t key_prev1_qual;
static uint16_t key_prev2_code;
static uint16_t key_prev2_qual;

static input_queued_event_t input_queue[INPUT_QUEUE_SIZE];
static uint8_t              input_queue_count;

/* statistics, reported by amigaos4_input_dump_stats() */
static uint32_t             input_dropped_events;
static uint32_t             input_sent_events;
static uint32_t             input_coalesced_events;
static uint8_t              input_queue_max;

/* time stamp of the last movement event we sent, for the rate limit */
static uint32_t             input_last_move_us;

/*
 * Key repeat, which is ours to generate.
 *
 * input.device stores the delay and rate the user chose and publishes them -
 * IND_GETTHRESH and IND_GETPERIOD to read them, and a notification hook to hear
 * about changes - but it does not apply them to what comes through it. That is
 * the keyboard driver's job, and feeding input.device is what this is, so a key
 * held down produced exactly one event and backspace deleted one character
 * however long it was held.
 *
 * Only one key repeats: the last one pressed, as on any keyboard. Modifiers
 * never do - a held shift is a state, not something to be sent over and over.
 *
 * The repeats come out of amigaos4_input_poll(), so the rate cannot be finer
 * than the caller polls - about 50 ms with the run loop this ships with, which
 * is close enough to the usual setting to feel normal. Getting the exact rate
 * would mean this file knowing about the run loop, and keeping it independent
 * of Bluetooth entirely is worth more than the last few milliseconds.
 */
#define REPEAT_NONE 0xFFFF

static uint16_t repeat_code = REPEAT_NONE;
static uint16_t repeat_qualifier;
static uint32_t repeat_due_us;
static uint32_t repeat_threshold_us = 500000;  /* until input.device is asked */
static uint32_t repeat_period_us    =  40000;

/*
 * Amiga raw key codes 0x60..0x67 are the qualifier keys - both shifts, caps
 * lock, control, both alts and both Amiga keys.
 */
static bool key_is_modifier(uint16_t rawkey){
    return (rawkey >= 0x60) && (rawkey <= 0x67);
}

/*
 * Ask input.device what the user chose for repeat delay and rate.
 *
 * These live in Prefs/Input and belong to the whole machine, so a Bluetooth
 * keyboard has to repeat at the same speed as every other one - inventing our
 * own numbers would make it the odd one out. Both commands are V50; if either
 * is refused the built-in defaults stand, which is a keyboard that repeats
 * slightly wrong rather than one that does not repeat at all.
 */
static void repeat_read_settings(struct MsgPort * port, struct Device * device){

    struct TimeRequest * request = AllocSysObjectTags(ASOT_IOREQUEST,
                                                      ASOIOR_Size,      sizeof(struct TimeRequest),
                                                      ASOIOR_ReplyPort, port,
                                                      TAG_END);
    if (request == NULL) return;

    request->Request.io_Device = device;
    request->Request.io_Unit   = NULL;

    request->Request.io_Command = IND_GETTHRESH;
    if (DoIO((struct IORequest *) request) == 0){
        repeat_threshold_us = (uint32_t) (request->Time.Seconds * 1000000 +
                                          request->Time.Microseconds);
    }

    request->Request.io_Command = IND_GETPERIOD;
    if (DoIO((struct IORequest *) request) == 0){
        repeat_period_us = (uint32_t) (request->Time.Seconds * 1000000 +
                                       request->Time.Microseconds);
    }

    /* a period of zero would repeat once per poll and flood the input chain */
    if (repeat_period_us < 10000) repeat_period_us = 10000;

    FreeSysObject(ASOT_IOREQUEST, request);
}

static uint32_t input_now_us(void){
    if (itimer == NULL) return 0;
    struct TimeVal now;
    itimer->GetSysTime(&now);
    return (uint32_t) ((now.Seconds * 1000000) + now.Microseconds);
}

static bool input_is_move(uint8_t class, uint16_t code){
    return (class == IECLASS_RAWMOUSE) && (code == IECODE_NOBUTTON);
}

/* mouse button state, so only changes are reported */
static uint8_t           button_state;

/*
 * Which conventions to use when building an event. There is no authoritative
 * documentation on what Intuition expects from an injected event during a modal
 * operation like sizing a window, so these are switchable to be bisected on the
 * real system instead of guessed. Defaults are what we believe to be correct.
 */
static bool              opt_timestamps        = true;
static bool              opt_button_qualifiers = true;
static bool              opt_relative_flag     = true;
static bool              opt_log_buttons;

void amigaos4_input_set_options(bool timestamps, bool button_qualifiers, bool relative_flag,
                                bool log_buttons){
    opt_timestamps        = timestamps;
    opt_button_qualifiers = button_qualifiers;
    opt_relative_flag     = relative_flag;
    opt_log_buttons       = log_buttons;
    /* startup, no injection going on yet: the console is safe here */
    printf("input: time stamps %s, button qualifiers %s, IEQUALIFIER_RELATIVEMOUSE %s\n",
           timestamps ? "on" : "off",
           button_qualifiers ? "on" : "off",
           relative_flag ? "on" : "off");
    if (log_buttons){
        printf("input: button events are logged to the serial debug output\n");
    }
}


/* map our button bits to the Amiga button codes */
static const struct {
    uint8_t mask;
    uint16_t code;
} button_codes[] = {
    { AMIGAOS4_INPUT_BUTTON_LEFT,   IECODE_LBUTTON },
    { AMIGAOS4_INPUT_BUTTON_RIGHT,  IECODE_RBUTTON },
    { AMIGAOS4_INPUT_BUTTON_MIDDLE, IECODE_MBUTTON },
};

/* -------------------------------------------------------------------------- */

/* qualifier bits describing which buttons are held right now - Intuition tracks
 * click, double click and drag state through these, so every RAWMOUSE event has
 * to carry them, movement included */
static uint16_t input_relative_qualifier(void){
    return opt_relative_flag ? IEQUALIFIER_RELATIVEMOUSE : 0;
}

static uint16_t input_button_qualifiers(uint8_t buttons){
    uint16_t qualifier = 0;
    if (opt_button_qualifiers == false) return 0;
    if (buttons & AMIGAOS4_INPUT_BUTTON_LEFT)   qualifier |= IEQUALIFIER_LEFTBUTTON;
    if (buttons & AMIGAOS4_INPUT_BUTTON_RIGHT)  qualifier |= IEQUALIFIER_RBUTTON;
    if (buttons & AMIGAOS4_INPUT_BUTTON_MIDDLE) qualifier |= IEQUALIFIER_MIDBUTTON;
    return qualifier;
}

/* collect the requests input.device has replied to - never blocks */
static void input_reap(void){
    struct Message * msg;
    while ((msg = GetMsg(input_port)) != NULL){
        uint8_t i;
        for (i = 0; i < INPUT_SLOT_COUNT; i++){
            if (&input_slots[i].req->io_Message == msg){
                input_slots[i].in_flight = false;
                break;
            }
        }
    }
}

/* hand one event to input.device, or return false if no slot is free */
static bool input_send(uint8_t class, uint16_t code, uint16_t qualifier, int16_t x, int16_t y){

    uint8_t i;
    for (i = 0; i < INPUT_SLOT_COUNT; i++){
        input_slot_t * slot = &input_slots[i];
        if (slot->in_flight) continue;

        memset(&slot->event, 0, sizeof(slot->event));
        slot->event.ie_NextEvent = NULL;
        slot->event.ie_Class     = class;
        slot->event.ie_SubClass  = 0;
        slot->event.ie_Code      = code;
        slot->event.ie_Qualifier = qualifier;
        slot->event.ie_position.ie_xy.ie_x = x;
        slot->event.ie_position.ie_xy.ie_y = y;

        /*
         * A real time stamp is required, not zero: Intuition derives double
         * click from the interval between two button events. With every event
         * stamped 0 that interval is always 0.
         */
        if (opt_timestamps && (itimer != NULL)){
            itimer->GetSysTime(&slot->event.ie_TimeStamp);
        }

        if (opt_log_buttons && (class == IECLASS_RAWMOUSE) && (code != IECODE_NOBUTTON)){
            /* serial, not the console - see the note at the top of this file */
            BT_LOG("input: button event code 0x%02x (%s), qualifier 0x%04x\n",
                        (unsigned int) code,
                        (code & IECODE_UP_PREFIX) ? "release" : "press",
                        (unsigned int) qualifier);
        }

        if (class == IECLASS_RAWKEY){
            slot->event.ie_position.ie_dead.ie_prev1DownCode = (uint8_t) key_prev1_code;
            slot->event.ie_position.ie_dead.ie_prev1DownQual = (uint8_t) key_prev1_qual;
            slot->event.ie_position.ie_dead.ie_prev2DownCode = (uint8_t) key_prev2_code;
            slot->event.ie_position.ie_dead.ie_prev2DownQual = (uint8_t) key_prev2_qual;

            /* only key presses take part in dead key composition */
            if ((code & IECODE_UP_PREFIX) == 0){
                key_prev2_code = key_prev1_code;
                key_prev2_qual = key_prev1_qual;
                key_prev1_code = code;
                key_prev1_qual = qualifier;
            }
        }

        /*
         * IND_ADDEVENT, not IND_WRITEEVENT. On OS4 this is what a driver
         * feeding the input stream uses - the boot mouse, boot keyboard and HID
         * drivers all do. IND_WRITEEVENT is the OS3 way and runs the event
         * through the handler chain, which is both slower and the reason a
         * blocking call there could deadlock against Intuition.
         */
        slot->req->io_Command = IND_ADDEVENT;
        slot->req->io_Flags   = 0;
        slot->req->io_Length  = sizeof(struct InputEvent);
        slot->req->io_Data    = &slot->event;
        slot->in_flight = true;
        SendIO((struct IORequest *) slot->req);
        input_sent_events++;
        if (input_is_move(class, code)){
            input_last_move_us = input_now_us();
        }
        return true;
    }
    return false;
}

/* true if a movement event may be sent now, see INPUT_MOVE_INTERVAL_US */
static bool input_move_allowed_now(void){
    if (itimer == NULL) return true;
    return (input_now_us() - input_last_move_us) >= INPUT_MOVE_INTERVAL_US;
}

/* send what is queued, as far as the device and the rate limit allow */
static void input_flush(void){
    input_reap();
    uint8_t sent = 0;
    while (sent < input_queue_count){
        input_queued_event_t * entry = &input_queue[sent];
        /* order is preserved: a movement held back by the rate limit also holds
         * back what is behind it */
        if (input_is_move(entry->class, entry->code) && (input_move_allowed_now() == false)) break;
        if (input_send(entry->class, entry->code, entry->qualifier, entry->x, entry->y) == false) break;
        sent++;
    }
    if (sent == 0) return;
    input_queue_count -= sent;
    if (input_queue_count > 0){
        memmove(&input_queue[0], &input_queue[sent], input_queue_count * sizeof(input_queued_event_t));
    }
}

static void input_write_event(uint8_t class, uint16_t code, uint16_t qualifier,
                              int16_t x, int16_t y){

    if (input_open == false) return;

    /* keep the order: while something is queued, everything queues */
    if (input_queue_count == 0){
        input_flush();
        if (input_queue_count == 0){
            bool may_send = (input_is_move(class, code) == false) || input_move_allowed_now();
            if (may_send && input_send(class, code, qualifier, x, y)) return;
        }
    }

    /* Coalesce consecutive movement into the last queued movement: waiting for
     * the device or for the rate limit then costs one entry, and the pointer
     * still travels the full distance once it goes out. */
    if (input_is_move(class, code) && (input_queue_count > 0)){
        input_queued_event_t * tail = &input_queue[input_queue_count - 1];
        if (input_is_move(tail->class, tail->code)){
            tail->x = (int16_t) (tail->x + x);
            tail->y = (int16_t) (tail->y + y);
            tail->qualifier = qualifier;
            input_coalesced_events++;
            return;
        }
    }

    if (input_queue_count >= INPUT_QUEUE_SIZE){
        input_dropped_events++;
        return;
    }
    if (input_queue_count >= input_queue_max){
        input_queue_max = input_queue_count + 1;
    }

    input_queued_event_t * entry = &input_queue[input_queue_count++];
    entry->class     = class;
    entry->code      = code;
    entry->qualifier = qualifier;
    entry->x         = x;
    entry->y         = y;
}

void amigaos4_input_poll(void){
    if (input_open == false) return;

    if (repeat_code != REPEAT_NONE){
        uint32_t now = input_now_us();
        /* unsigned subtraction, so this survives the microsecond counter
         * wrapping rather than repeating for the next hour and a half */
        if ((uint32_t) (now - repeat_due_us) < 0x80000000u){
            input_write_event(IECLASS_RAWKEY, repeat_code,
                              repeat_qualifier | IEQUALIFIER_REPEAT, 0, 0);
            repeat_due_us = now + repeat_period_us;
        }
    }

    input_flush();
}

void amigaos4_input_dump_stats(void){
    /* called from the run loop, so serial only - see the note at the top */
    BT_LOG("input: %lu sent, %lu coalesced, %lu dropped, queue max %u, in flight %u\n",
           (unsigned long) input_sent_events,
           (unsigned long) input_coalesced_events,
           (unsigned long) input_dropped_events,
           (unsigned int) input_queue_max,
           (unsigned int) (input_slots[0].in_flight ? 1 : 0));
}

bool amigaos4_input_open(void){

    if (input_open) return true;

    input_port = AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (input_port == NULL){
        log_error("cannot create input.device MsgPort");
        return false;
    }
    input_req = AllocSysObjectTags(ASOT_IOREQUEST,
                                   ASOIOR_Size,      sizeof(struct IOStdReq),
                                   ASOIOR_ReplyPort, input_port,
                                   TAG_END);
    if (input_req == NULL){
        log_error("cannot create input.device IORequest");
        FreeSysObject(ASOT_PORT, input_port);
        input_port = NULL;
        return false;
    }
    if (OpenDevice("input.device", 0, (struct IORequest *) input_req, 0) != 0){
        log_error("cannot open input.device");
        FreeSysObject(ASOT_IOREQUEST, input_req);
        FreeSysObject(ASOT_PORT, input_port);
        input_req = NULL;
        input_port = NULL;
        return false;
    }

    repeat_read_settings(input_port, input_req->io_Device);
    input_device = input_req->io_Device;

    /* One request per slot, all replying to our port. They are copies of the
     * opened request, which is how you get several requests for one device. */
    uint8_t i;
    for (i = 0; i < INPUT_SLOT_COUNT; i++){
        input_slots[i].req = AllocSysObjectTags(ASOT_IOREQUEST,
                                               ASOIOR_Size,      sizeof(struct IOStdReq),
                                               ASOIOR_ReplyPort, input_port,
                                               ASOIOR_Duplicate, input_req,
                                               TAG_END);
        if (input_slots[i].req == NULL){
            log_error("cannot duplicate input.device IORequest %u", i);
            break;
        }
        input_slots[i].in_flight = false;
    }
    if (input_slots[0].req == NULL){
        CloseDevice((struct IORequest *) input_req);
        FreeSysObject(ASOT_IOREQUEST, input_req);
        FreeSysObject(ASOT_PORT, input_port);
        input_req = NULL;
        input_port = NULL;
        return false;
    }

    input_queue_count = 0;
    input_dropped_events = 0;

    /* timer.device, for the event time stamps. Not fatal if it fails: events
     * still work, only double click detection gets unreliable. */
    timer_port = AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (timer_port != NULL){
        timer_req = AllocSysObjectTags(ASOT_IOREQUEST,
                                       ASOIOR_Size,      sizeof(struct TimeRequest),
                                       ASOIOR_ReplyPort, timer_port,
                                       TAG_END);
    }
    if ((timer_req != NULL) && (OpenDevice(TIMERNAME, UNIT_MICROHZ, (struct IORequest *) timer_req, 0) == 0)){
        itimer = (struct TimerIFace *) GetInterface((struct Library *) timer_req->Request.io_Device, "main", 1, NULL);
    }
    if (itimer == NULL){
        log_error("cannot open timer.device, events will not be time stamped");
    }

    button_state = 0;
    input_open = true;
    return true;
}

void amigaos4_input_close(void){

    /* nothing may be left repeating for a keyboard that has gone */
    repeat_code = REPEAT_NONE;


    if (input_open == false) return;

    if (input_dropped_events > 0){
        log_info("input: %lu events dropped (input.device could not keep up)",
                 (unsigned long) input_dropped_events);
    }

    /* Wait for the outstanding requests - their event buffers must not go away
     * while input.device still has them - but do not free the slots yet: the
     * button release below needs one. Queued movement is dropped, it is of no
     * interest now. */
    uint8_t i;
    input_queue_count = 0;
    for (i = 0; i < INPUT_SLOT_COUNT; i++){
        if (input_slots[i].req == NULL) continue;
        if (input_slots[i].in_flight){
            WaitIO((struct IORequest *) input_slots[i].req);
            input_slots[i].in_flight = false;
        }
    }

    /* Never leave a button down: without this, quitting while a button is held
     * leaves the whole system with a stuck mouse button. All slots are free now,
     * so this goes out immediately instead of being queued. */
    amigaos4_input_mouse_buttons(0);

    input_open = false;

    for (i = 0; i < INPUT_SLOT_COUNT; i++){
        if (input_slots[i].req == NULL) continue;
        if (input_slots[i].in_flight){
            WaitIO((struct IORequest *) input_slots[i].req);
            input_slots[i].in_flight = false;
        }
        FreeSysObject(ASOT_IOREQUEST, input_slots[i].req);
        input_slots[i].req = NULL;
    }

    CloseDevice((struct IORequest *) input_req);
    FreeSysObject(ASOT_IOREQUEST, input_req);
    input_req = NULL;

    /* drain replies before the port goes away */
    struct Message * msg;
    while ((msg = GetMsg(input_port)) != NULL){}
    UNUSED(msg);
    SetSignal(0, 1UL << input_port->mp_SigBit);
    FreeSysObject(ASOT_PORT, input_port);
    input_port = NULL;

    if (itimer != NULL){
        DropInterface((struct Interface *) itimer);
        itimer = NULL;
    }
    if (timer_req != NULL){
        CloseDevice((struct IORequest *) timer_req);
        FreeSysObject(ASOT_IOREQUEST, timer_req);
        timer_req = NULL;
    }
    if (timer_port != NULL){
        while ((msg = GetMsg(timer_port)) != NULL){}
        SetSignal(0, 1UL << timer_port->mp_SigBit);
        FreeSysObject(ASOT_PORT, timer_port);
        timer_port = NULL;
    }
}

void amigaos4_input_mouse_move(int16_t dx, int16_t dy){

    if ((dx == 0) && (dy == 0)) return;

    /* the held buttons have to travel with the movement, or dragging breaks */
    input_write_event(IECLASS_RAWMOUSE, IECODE_NOBUTTON,
                      input_relative_qualifier() | input_button_qualifiers(button_state),
                      dx, dy);
}

void amigaos4_input_mouse_buttons(uint8_t buttons){

    uint8_t changed = button_state ^ buttons;
    if (changed == 0) return;

    uint8_t i;
    for (i = 0; i < (sizeof(button_codes) / sizeof(button_codes[0])); i++){
        uint8_t mask = button_codes[i].mask;
        if ((changed & mask) == 0) continue;

        bool pressed = (buttons & mask) != 0;

        /* Report one button at a time, and let the state - and therefore the
         * qualifiers - follow each event, so a report that presses two buttons
         * at once still produces a consistent sequence. */
        if (pressed){
            button_state |= mask;
        } else {
            button_state &= ~mask;
        }

        input_write_event(IECLASS_RAWMOUSE,
                          button_codes[i].code | (pressed ? 0 : IECODE_UP_PREFIX),
                          input_relative_qualifier() | input_button_qualifiers(button_state),
                          0, 0);
    }

    button_state = buttons;
}

void amigaos4_input_key(uint16_t rawkey, bool pressed, uint16_t qualifier){

    /*
     * Codes above 0xFF are IECLASS_EXTENDEDRAWKEY and need an IEExtDeadKey
     * structure that the public SDK does not declare, so they are dropped. That
     * range is F13 and up plus the media keys; every key of an ordinary
     * keyboard fits in IECLASS_RAWKEY.
     */
    if (rawkey > 0xFF) return;

    if (pressed && !key_is_modifier(rawkey)){
        /* the newest key is the one that repeats, replacing any before it */
        repeat_code      = rawkey;
        repeat_qualifier = qualifier;
        repeat_due_us    = input_now_us() + repeat_threshold_us;
    } else if (!pressed && (rawkey == repeat_code)){
        repeat_code = REPEAT_NONE;
    }

    input_write_event(IECLASS_RAWKEY,
                      rawkey | (pressed ? 0 : IECODE_UP_PREFIX),
                      qualifier, 0, 0);
}

void amigaos4_input_mouse_wheel(int16_t horizontal, int16_t vertical){

    if ((horizontal == 0) && (vertical == 0)) return;

    input_write_event(IECLASS_MOUSEWHEEL, IECODE_NOBUTTON,
                      input_relative_qualifier() | input_button_qualifiers(button_state),
                      horizontal, vertical);
}
