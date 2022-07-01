/* awiegand_emu.c
 *
 * author:  atomsmasha
 * date:    june 2022
 * purpose: Intentded to simulate a 're-settable' 26-bit 
 *          (H13010) Wiegand HID. 
 * 
 * notes: - This is for the Flipper Zero
 *        - LFRFID code has not yet been implemented. This 
 *          is currently being worked on and may require
 *          a C++ refactor.
 *        - So currently this is just a tech demo for the 
 *          UI. 
 *        - Up/Down to change value
 *        - Left/Right to move to the next digit
 *        - 'Enter' for blinken lights
 *        - 'Return' to exit
*/

#include "furi/common_defines.h"
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <stdlib.h>

#define CODELEN 6

static const NotificationSequence sequence_blink_start_magenta = {
    &message_blink_start_10,
    &message_blink_set_color_magenta,
    &message_do_not_reset,
    NULL,
};

static const NotificationSequence sequence_blink_stop = {
    &message_blink_stop,
    NULL,
};

typedef struct {
    uint8_t card_code[CODELEN];
    uint8_t cursor_position;
    bool transmitting;
} WiegandEmuModel;

typedef struct {
    WiegandEmuModel* model;
    osMutexId_t* model_mutex;

    osMessageQueueId_t input_queue;

    NotificationApp* notifications;
    ViewPort* view_port;
    Gui* gui;
} WiegandEmu;

static void render_callback(Canvas* canvas, void* ctx) {
    WiegandEmu* wiegand_emu = ctx;
    furi_check(osMutexAcquire(wiegand_emu->model_mutex, osWaitForever) == osOK);

    uint8_t y_pos = 28;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);

    // this is a reusable buffer for typesetting
    char temp_value[2];
    for (uint8_t i = 1; i < CODELEN + 1; i++) {
        snprintf(temp_value, 4, "%X", wiegand_emu->model->card_code[i - 1]);
        canvas_draw_str(canvas, i * 10, y_pos, temp_value);
    }

    canvas_draw_box(canvas, ((wiegand_emu->model->cursor_position + 1) * 10) + 1, y_pos + 3, 3, 3);
   
    osMutexRelease(wiegand_emu->model_mutex);
}

static void input_callback(InputEvent* input_event, void* ctx) {
    WiegandEmu* wiegand_emu = ctx;

    osMessageQueuePut(wiegand_emu->input_queue, input_event, 0, osWaitForever);
}

static void increment_nibble(void* ctx) {
    WiegandEmu* wiegand_emu = ctx;

    uint8_t curs_pos = wiegand_emu->model->cursor_position;
    uint8_t val = wiegand_emu->model->card_code[curs_pos];

    if (val < 0xF) {
        wiegand_emu->model->card_code[curs_pos]+= 0x1;
    } else {
        wiegand_emu->model->card_code[curs_pos] = 0x0;
    }
}

static void decrement_nibble(void* ctx) {
    WiegandEmu* wiegand_emu = ctx;

    uint8_t curs_pos = wiegand_emu->model->cursor_position;
    uint8_t val = wiegand_emu->model->card_code[curs_pos];

    if (val > 0x0) {
        wiegand_emu->model->card_code[curs_pos]-= 0x1;
    } else {
        wiegand_emu->model->card_code[curs_pos] = 0xF;
    }
}

static void cursor_right(void* ctx) {
    WiegandEmu* wiegand_emu = ctx;
    uint8_t curs_pos = wiegand_emu->model->cursor_position;

    if (curs_pos < CODELEN - 1) {
        wiegand_emu->model->cursor_position += 1;
    } else {
        wiegand_emu->model->cursor_position = 0;
    }
}

static void cursor_left(void* ctx) {
    WiegandEmu* wiegand_emu = ctx;
    uint8_t curs_pos = wiegand_emu->model->cursor_position;

    if (curs_pos > 0) {
        wiegand_emu->model->cursor_position -= 1;
    } else {
        wiegand_emu->model->cursor_position = CODELEN - 1;
    }
}

static void toggle_emulation(void* ctx) {
    WiegandEmu* wiegand_emu = ctx;
    wiegand_emu->model->transmitting = !wiegand_emu->model->transmitting;

    if (wiegand_emu->model->transmitting == true) {
        notification_message(wiegand_emu->notifications, &sequence_blink_start_magenta);
    } else {
        notification_message(wiegand_emu->notifications, &sequence_blink_stop);
    }
}

WiegandEmu* wiegand_emu_alloc() {
    WiegandEmu* instance = malloc(sizeof(WiegandEmu));

    instance->model = malloc(sizeof(WiegandEmuModel));
    for (uint8_t i = 0; i < CODELEN; i++ ) {
        instance->model->card_code[i] = 0x00;
    }
    instance->model->cursor_position = 0;
    instance->model->transmitting = false; 

    instance->model_mutex = osMutexNew(NULL);

    instance->input_queue = osMessageQueueNew(8, sizeof(InputEvent), NULL);

    instance->notifications = furi_record_open("notification");

    instance->view_port = view_port_alloc();
    view_port_draw_callback_set(instance->view_port, render_callback, instance);
    view_port_input_callback_set(instance->view_port, input_callback, instance);

    instance->gui = furi_record_open("gui");
    gui_add_view_port(instance->gui, instance->view_port, GuiLayerFullscreen);

    return instance;
}

void wiegand_emu_free(WiegandEmu* instance) {
    gui_remove_view_port(instance->gui, instance->view_port);
    furi_record_close("gui");
    view_port_free(instance->view_port);

    osMessageQueueDelete(instance->input_queue);

    // The LED will keep blinken after exit if we don't explicitly 
    // kill it before we close the notification record
    if (instance->model->transmitting == true) {
        notification_message(instance->notifications, &sequence_blink_stop);
    }

    furi_record_close("notification");

    osMutexDelete(instance->model_mutex);

    free(instance->model);
    free(instance);
}

int32_t awiegand_emu_app(void *p) {
    UNUSED(p);
    WiegandEmu* wiegand_emu = wiegand_emu_alloc();

    InputEvent event;
    for(bool processing = true; processing;) {
        osStatus_t status = osMessageQueueGet(wiegand_emu->input_queue, &event, NULL, 100);
        furi_check(osMutexAcquire(wiegand_emu->model_mutex, osWaitForever) == osOK);
        if (status==osOK) {
            if(event.type==InputTypePress) {
                switch(event.key) {
                    case InputKeyUp:
                        increment_nibble(wiegand_emu);
                        break;
                    case InputKeyDown:
                        decrement_nibble(wiegand_emu);
                        break;
                    case InputKeyRight:
                        cursor_right(wiegand_emu);
                        break;
                    case InputKeyLeft:
                        cursor_left(wiegand_emu);
                        break;
                    case InputKeyOk:
                        toggle_emulation(wiegand_emu);
                        break;
                    case InputKeyBack:
                        processing = false;
                        break;
                }
            }
        }
        osMutexRelease(wiegand_emu->model_mutex);
        view_port_update(wiegand_emu->view_port);
    }

    wiegand_emu_free(wiegand_emu);
    return 0;
}
