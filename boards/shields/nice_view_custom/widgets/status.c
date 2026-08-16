/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <lvgl.h>

#include <zmk/battery.h>
#include <zmk/display.h>

#include "status.h"
#include "bongocatart.h"

#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/wpm_state_changed.h>

#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/wpm.h>
#include <zmk/hid.h>

#include <inttypes.h>
#include <string.h>
#include <stdio.h>


/* ==========================================================================
 * Modifier icons
 * ========================================================================== */

LV_IMG_DECLARE(control_icon);
LV_IMG_DECLARE(shift_icon);

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_MAC_MODIFIERS)
LV_IMG_DECLARE(opt_icon);
LV_IMG_DECLARE(cmd_icon);
#else
LV_IMG_DECLARE(alt_icon);
LV_IMG_DECLARE(win_icon);
#endif


/* ==========================================================================
 * Widget list
 * ========================================================================== */

static sys_slist_t widgets =
    SYS_SLIST_STATIC_INIT(&widgets);


/* ==========================================================================
 * Output state
 * ========================================================================== */

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};


/* ==========================================================================
 * Layer state
 * ========================================================================== */

struct layer_status_state {
    uint8_t index;
    const char *label;
};


/* ==========================================================================
 * WPM state
 *
 * WPM wird NICHT dargestellt.
 * WPM steuert nur Bongo Cat.
 * ========================================================================== */

struct wpm_status_state {
    uint8_t wpm;
    bool is_key_event;
    bool key_pressed;
};


/* ==========================================================================
 * Bongo animation
 * ========================================================================== */

enum anim_state {
    ANIM_STATE_CASUAL,
    ANIM_STATE_FRENZIED
};

static enum anim_state current_anim_state =
    ANIM_STATE_CASUAL;


enum idle_anim_state {
    IDLE_INHALE,
    IDLE_REST1,
    IDLE_EXHALE,
    IDLE_REST2
};

static enum idle_anim_state current_idle_state =
    IDLE_INHALE;


static const lv_img_dsc_t *last_active_frame =
    &bongo_resting;


static bool use_first_frame = true;

static bool key_pressed = false;
static bool key_released = false;
static bool keys_active = false;

static uint8_t active_keys = 0;

static uint32_t last_key_event = 0;

static uint32_t last_idle_update = 0;

static const uint32_t IDLE_ANIMATION_INTERVAL = 750;


/* ==========================================================================
 * Breathing variation
 * ========================================================================== */

static uint32_t random_seed = 7919;

static int32_t breathing_interval_adjustment = 0;

static bool leaving_furious = false;


static int32_t get_random_adjustment(void) {

    static bool initialized = false;

    if (!initialized) {
        random_seed ^= k_uptime_get_32();
        initialized = true;
    }

    random_seed =
        random_seed * 1103515245 + 12345;

    return ((random_seed / 65536) % 501) - 250;
}


/* ==========================================================================
 * Work
 * ========================================================================== */

static struct k_work_delayable animation_work;

static struct k_work_delayable modifier_work;

static bool modifier_work_scheduled = false;


/* ==========================================================================
 * Modifier state
 * ========================================================================== */

static uint8_t get_current_modifiers(void) {
    return zmk_hid_get_explicit_mods();
}


/* ==========================================================================
 * WPM -> Bongo
 * ========================================================================== */

static void update_bongo_from_wpm(uint8_t wpm) {

    /*
     * Über 30 WPM:
     * Furious.
     */

    if (wpm > 30) {

        current_anim_state =
            ANIM_STATE_FRENZIED;

        leaving_furious = false;

        return;
    }


    /*
     * Wir verlassen Furious,
     * sobald WPM wieder <= 30 ist.
     */

    if (current_anim_state == ANIM_STATE_FRENZIED) {

        current_anim_state =
            ANIM_STATE_CASUAL;

        leaving_furious = true;

        current_idle_state =
            IDLE_EXHALE;

        last_idle_update =
            k_uptime_get_32();
    }
}


/* ==========================================================================
 * Bongo frame
 * ========================================================================== */

static const lv_img_dsc_t *get_bongo_frame(void) {

    /* ----------------------------------------------------------------------
     * Furious
     * ---------------------------------------------------------------------- */

    if (current_anim_state == ANIM_STATE_FRENZIED) {

        if (key_pressed || key_released) {

            if (use_first_frame) {
                last_active_frame =
                    &bongo_furiousup;
            } else {
                last_active_frame =
                    &bongo_furiousdown;
            }

            use_first_frame =
                !use_first_frame;

            return last_active_frame;
        }


        if (keys_active) {
            return last_active_frame;
        }
    }

    /* ----------------------------------------------------------------------
     * Casual
     * ---------------------------------------------------------------------- */

    else {

        if (key_pressed) {

            if (use_first_frame) {
                last_active_frame =
                    &bongo_casualright;
            } else {
                last_active_frame =
                    &bongo_casualleft;
            }

            use_first_frame =
                !use_first_frame;

            return last_active_frame;
        }


        if (keys_active) {
            return last_active_frame;
        }
    }


    /* ----------------------------------------------------------------------
     * Idle
     * ---------------------------------------------------------------------- */

    switch (current_idle_state) {

    case IDLE_INHALE:
        return &bongo_inhale;

    case IDLE_REST1:
        return &bongo_resting;

    case IDLE_EXHALE:
        return &bongo_exhale;

    case IDLE_REST2:
    default:
        return &bongo_resting;
    }
}


/* ==========================================================================
 * TOP PANEL
 *
 * Dieses Panel ist physisch oben.
 *
 * Nach Rotation:
 *
 * ┌─────────────────────────┐
 * │ Akku              WiFi  │
 * │                         │
 * │        BONGO            │
 * └─────────────────────────┘
 *
 * ========================================================================== */

static void draw_top(
    lv_obj_t *widget,
    lv_color_t cbuf[],
    const struct status_state *state
) {

    lv_obj_t *canvas =
        lv_obj_get_child(widget, 0);


    /* Background */

    lv_draw_rect_dsc_t bg;

    init_rect_dsc(
        &bg,
        LVGL_BACKGROUND
    );

    lv_canvas_draw_rect(
        canvas,
        0,
        0,
        CANVAS_SIZE,
        CANVAS_SIZE,
        &bg
    );


    /* Battery */

    draw_battery(
        canvas,
        state
    );


    /* ----------------------------------------------------------------------
     * Connection icon
     * ---------------------------------------------------------------------- */

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

    lv_draw_label_dsc_t label;

    init_label_dsc(
        &label,
        LVGL_FOREGROUND,
        &lv_font_montserrat_16,
        LV_TEXT_ALIGN_RIGHT
    );


    char output_text[10] = {};


    switch (
        state->selected_endpoint.transport
    ) {

    case ZMK_TRANSPORT_USB:

        strcat(
            output_text,
            LV_SYMBOL_USB
        );

        break;


    case ZMK_TRANSPORT_BLE:

        if (state->active_profile_bonded) {

            if (state->active_profile_connected) {

                strcat(
                    output_text,
                    LV_SYMBOL_WIFI
                );

            } else {

                strcat(
                    output_text,
                    LV_SYMBOL_CLOSE
                );
            }

        } else {

            strcat(
                output_text,
                LV_SYMBOL_SETTINGS
            );
        }

        break;


    default:
        break;
    }


    /*
     * Rechts oben.
     */

    lv_canvas_draw_text(
        canvas,
        38,
        0,
        28,
        &label,
        output_text
    );

#endif


    /*
     * Rotation für das physisch hochkant eingebaute nice!view.
     */

    rotate_canvas(
        canvas,
        cbuf
    );
}


/* ==========================================================================
 * MIDDLE PANEL
 *
 * Physisch in der Mitte:
 *
 * ┌─────────────────────────┐
 * │                         │
 * │        BONGO CAT        │
 * │                         │
 * │ CTRL ALT CMD SHIFT      │
 * └─────────────────────────┘
 *
 * ========================================================================== */

static void draw_middle(
    lv_obj_t *widget,
    lv_color_t cbuf[],
    const struct status_state *state
) {

    lv_obj_t *canvas =
        lv_obj_get_child(widget, 1);


    lv_draw_rect_dsc_t bg;

    init_rect_dsc(
        &bg,
        LVGL_BACKGROUND
    );


    /*
     * Komplettes Panel löschen.
     */

    lv_canvas_draw_rect(
        canvas,
        0,
        0,
        CANVAS_SIZE,
        CANVAS_SIZE,
        &bg
    );


    /* ----------------------------------------------------------------------
     * Bongo
     * ---------------------------------------------------------------------- */

    lv_draw_img_dsc_t img;

    lv_draw_img_dsc_init(
        &img
    );


    const lv_img_dsc_t *frame =
        get_bongo_frame();


    int image_width =
        frame->header.w;

    int image_height =
        frame->header.h;


    /*
     * Horizontal zentrieren.
     */

    int x =
        (CANVAS_SIZE - image_width) / 2;


    /*
     * Vertikal zentrieren.
     *
     * Wir lassen unten etwas Platz für Modifier.
     */

    int y =
        3;


    /*
     * Wenn das Bild größer als 68px ist,
     * wird es automatisch vom Canvas abgeschnitten.
     */

    ARG_UNUSED(image_height);


    lv_canvas_draw_img(
        canvas,
        x,
        y,
        frame,
        &img
    );


    /* ----------------------------------------------------------------------
     * Modifier
     * ---------------------------------------------------------------------- */

    /*
     * Deine vorhandene draw_modifiers()-Funktion bleibt erhalten.
     *
     * Die Icons sind bereits 16px auseinander und funktionieren so.
     */

    draw_modifiers(
        canvas,
        2,
        52
    );


    /*
     * Rotation.
     */

    rotate_canvas(
        canvas,
        cbuf
    );


    ARG_UNUSED(state);
}


/* ==========================================================================
 * BOTTOM PANEL
 *
 * Physisch unten:
 *
 * ┌─────────────────────────┐
 * │                         │
 * │    Bluetooth    Layer   │
 * │                         │
 * └─────────────────────────┘
 *
 * ========================================================================== */

static void draw_bottom(
    lv_obj_t *widget,
    lv_color_t cbuf[],
    const struct status_state *state
) {

    lv_obj_t *canvas =
        lv_obj_get_child(widget, 2);


    lv_draw_rect_dsc_t bg;

    init_rect_dsc(
        &bg,
        LVGL_BACKGROUND
    );


    lv_canvas_draw_rect(
        canvas,
        0,
        0,
        CANVAS_SIZE,
        CANVAS_SIZE,
        &bg
    );


#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

    /* ----------------------------------------------------------------------
     * Bluetooth profile
     * ---------------------------------------------------------------------- */

    lv_draw_arc_dsc_t arc;

    init_arc_dsc(
        &arc,
        LVGL_FOREGROUND,
        2
    );


    lv_draw_arc_dsc_t arc_filled;

    init_arc_dsc(
        &arc_filled,
        LVGL_FOREGROUND,
        7
    );


    /*
     * Profile links.
     */

    const int profile_x = 18;
    const int profile_y = 34;


    lv_canvas_draw_arc(
        canvas,
        profile_x,
        profile_y,
        11,
        0,
        360,
        &arc
    );


    lv_canvas_draw_arc(
        canvas,
        profile_x,
        profile_y,
        7,
        0,
        359,
        &arc_filled
    );


    lv_draw_label_dsc_t profile_label;

    init_label_dsc(
        &profile_label,
        LVGL_BACKGROUND,
        &lv_font_montserrat_16,
        LV_TEXT_ALIGN_CENTER
    );


    char profile_text[2] = {};


    snprintf(
        profile_text,
        sizeof(profile_text),
        "%" PRIu8,
        (uint8_t)(
            state->active_profile_index + 1
        )
    );


    lv_canvas_draw_text(
        canvas,
        profile_x - 5,
        profile_y - 9,
        10,
        &profile_label,
        profile_text
    );


    /* ----------------------------------------------------------------------
     * Layer
     * ---------------------------------------------------------------------- */

    lv_draw_label_dsc_t layer_label;

    init_label_dsc(
        &layer_label,
        LVGL_FOREGROUND,
        &lv_font_montserrat_14,
        LV_TEXT_ALIGN_CENTER
    );


    char layer_text[16] = {};


    if (state->layer_label != NULL) {

        snprintf(
            layer_text,
            sizeof(layer_text),
            "%s",
            state->layer_label
        );

    } else {

        snprintf(
            layer_text,
            sizeof(layer_text),
            "L%" PRIu8,
            state->layer_index
        );
    }


    lv_canvas_draw_text(
        canvas,
        34,
        28,
        30,
        &layer_label,
        layer_text
    );

#endif


    /*
     * Rotation.
     */

    rotate_canvas(
        canvas,
        cbuf
    );
}


/* ==========================================================================
 * Battery
 * ========================================================================== */

static void set_battery_status(
    struct zmk_widget_status *widget,
    struct battery_status_state state
) {

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)

    widget->state.charging =
        state.usb_present;

#endif

    widget->state.battery =
        state.level;


    draw_top(
        widget->obj,
        widget->cbuf,
        &widget->state
    );
}


static void battery_status_update_cb(
    struct battery_status_state state
) {

    struct zmk_widget_status *widget;


    SYS_SLIST_FOR_EACH_CONTAINER(
        &widgets,
        widget,
        node
    ) {

        set_battery_status(
            widget,
            state
        );
    }
}


static struct battery_status_state
battery_status_get_state(
    const zmk_event_t *eh
) {

    const struct zmk_battery_state_changed *ev =
        as_zmk_battery_state_changed(eh);


    return (struct battery_status_state) {

        .level =
            ev != NULL
                ? ev->state_of_charge
                : zmk_battery_state_of_charge(),

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)

        .usb_present =
            zmk_usb_is_powered(),

#endif
    };
}


ZMK_DISPLAY_WIDGET_LISTENER(
    widget_battery_status,
    struct battery_status_state,
    battery_status_update_cb,
    battery_status_get_state
);


ZMK_SUBSCRIPTION(
    widget_battery_status,
    zmk_battery_state_changed
);


#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)

ZMK_SUBSCRIPTION(
    widget_battery_status,
    zmk_usb_conn_state_changed
);

#endif


/* ==========================================================================
 * Output
 * ========================================================================== */

static void set_output_status(
    struct zmk_widget_status *widget,
    const struct output_status_state *state
) {

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

    widget->state.selected_endpoint =
        state->selected_endpoint;

    widget->state.active_profile_index =
        state->active_profile_index;

    widget->state.active_profile_connected =
        state->active_profile_connected;

    widget->state.active_profile_bonded =
        state->active_profile_bonded;

#endif


    draw_top(
        widget->obj,
        widget->cbuf,
        &widget->state
    );


    draw_bottom(
        widget->obj,
        widget->cbuf3,
        &widget->state
    );
}


static void output_status_update_cb(
    struct output_status_state state
) {

    struct zmk_widget_status *widget;


    SYS_SLIST_FOR_EACH_CONTAINER(
        &widgets,
        widget,
        node
    ) {

        set_output_status(
            widget,
            &state
        );
    }
}


static struct output_status_state
output_status_get_state(
    const zmk_event_t *eh
) {

    return (struct output_status_state) {

        .selected_endpoint =
            zmk_endpoints_selected(),

        .active_profile_index =
            zmk_ble_active_profile_index(),

        .active_profile_connected =
            zmk_ble_active_profile_is_connected(),

        .active_profile_bonded =
            !zmk_ble_active_profile_is_open(),
    };
}


ZMK_DISPLAY_WIDGET_LISTENER(
    widget_output_status,
    struct output_status_state,
    output_status_update_cb,
    output_status_get_state
);


ZMK_SUBSCRIPTION(
    widget_output_status,
    zmk_endpoint_changed
);


#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)

ZMK_SUBSCRIPTION(
    widget_output_status,
    zmk_usb_conn_state_changed
);

#endif


#if defined(CONFIG_ZMK_BLE)

ZMK_SUBSCRIPTION(
    widget_output_status,
    zmk_ble_active_profile_changed
);

#endif


/* ==========================================================================
 * Layer
 * ========================================================================== */

static void set_layer_status(
    struct zmk_widget_status *widget,
    struct layer_status_state state
) {

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

    widget->state.layer_index =
        state.index;

    widget->state.layer_label =
        state.label;

#endif


    draw_bottom(
        widget->obj,
        widget->cbuf3,
        &widget->state
    );
}


static void layer_status_update_cb(
    struct layer_status_state state
) {

    struct zmk_widget_status *widget;


    SYS_SLIST_FOR_EACH_CONTAINER(
        &widgets,
        widget,
        node
    ) {

        set_layer_status(
            widget,
            state
        );
    }
}


static struct layer_status_state
layer_status_get_state(
    const zmk_event_t *eh
) {

    uint8_t index =
        zmk_keymap_highest_layer_active();


    return (struct layer_status_state) {

        .index = index,

        .label =
            zmk_keymap_layer_name(index),
    };
}


ZMK_DISPLAY_WIDGET_LISTENER(
    widget_layer_status,
    struct layer_status_state,
    layer_status_update_cb,
    layer_status_get_state
);


ZMK_SUBSCRIPTION(
    widget_layer_status,
    zmk_layer_state_changed
);


/* ==========================================================================
 * Key events
 * ========================================================================== */

static void process_keypress_event(
    bool pressed
) {

    key_pressed =
        pressed;

    key_released =
        !pressed;

    last_key_event =
        k_uptime_get_32();


    if (pressed) {

        if (active_keys < UINT8_MAX) {
            active_keys++;
        }

        keys_active = true;

    } else {

        if (active_keys > 0) {
            active_keys--;
        }

        keys_active =
            active_keys > 0;
    }


    /*
     * Modifier sofort aktualisieren.
     */

    if (!modifier_work_scheduled) {

        modifier_work_scheduled = true;

        k_work_schedule(
            &modifier_work,
            K_MSEC(20)
        );
    }


    /*
     * Bongo sofort aktualisieren.
     */

    struct zmk_widget_status *widget;


    SYS_SLIST_FOR_EACH_CONTAINER(
        &widgets,
        widget,
        node
    ) {

        draw_middle(
            widget->obj,
            widget->cbuf2,
            &widget->state
        );
    }
}


/* ==========================================================================
 * Modifier worker
 * ========================================================================== */

static void modifier_work_handler(
    struct k_work *work
) {

    ARG_UNUSED(work);


    modifier_work_scheduled =
        false;


    uint8_t mods =
        get_current_modifiers();


    struct zmk_widget_status *widget;


    SYS_SLIST_FOR_EACH_CONTAINER(
        &widgets,
        widget,
        node
    ) {

        if (widget->state.modifiers != mods) {

            widget->state.modifiers =
                mods;


            for (int i = 0; i < NUM_SYMBOLS; i++) {

                modifier_symbols[i]->is_active =
                    (mods &
                     modifier_symbols[i]->modifier) != 0;
            }


            draw_middle(
                widget->obj,
                widget->cbuf2,
                &widget->state
            );
        }
    }


    if (keys_active) {

        modifier_work_scheduled =
            true;

        k_work_schedule(
            &modifier_work,
            K_MSEC(20)
        );
    }
}


/* ==========================================================================
 * WPM state
 * ========================================================================== */

static struct wpm_status_state
wpm_status_get_state(
    const zmk_event_t *eh
) {

    static uint8_t current_wpm = 0;


    const struct zmk_wpm_state_changed *wpm_ev =
        as_zmk_wpm_state_changed(eh);

    const struct zmk_position_state_changed *pos_ev =
        as_zmk_position_state_changed(eh);


    bool is_key_event = false;

    bool pressed = false;


    if (wpm_ev != NULL) {

        current_wpm =
            wpm_ev->state;

        update_bongo_from_wpm(
            current_wpm
        );
    }


    if (pos_ev != NULL) {

        is_key_event = true;

        pressed =
            pos_ev->state > 0;
    }


    return (struct wpm_status_state) {

        .wpm =
            current_wpm,

        .is_key_event =
            is_key_event,

        .key_pressed =
            pressed,
    };
}


static void wpm_status_update_cb(
    struct wpm_status_state state
) {

    struct zmk_widget_status *widget;


    SYS_SLIST_FOR_EACH_CONTAINER(
        &widgets,
        widget,
        node
    ) {

        if (state.is_key_event) {

            process_keypress_event(
                state.key_pressed
            );

        } else {

            /*
             * WPM selbst wird NICHT angezeigt.
             *
             * Nur Bongo aktualisieren.
             */

            update_bongo_from_wpm(
                state.wpm
            );


            draw_middle(
                widget->obj,
                widget->cbuf2,
                &widget->state
            );
        }
    }
}


ZMK_DISPLAY_WIDGET_LISTENER(
    widget_wpm_status,
    struct wpm_status_state,
    wpm_status_update_cb,
    wpm_status_get_state
);


ZMK_SUBSCRIPTION(
    widget_wpm_status,
    zmk_wpm_state_changed
);


ZMK_SUBSCRIPTION(
    widget_wpm_status,
    zmk_position_state_changed
);


/* ==========================================================================
 * Animation worker
 * ========================================================================== */

static void animation_work_handler(
    struct k_work *work
) {

    ARG_UNUSED(work);


    uint32_t now =
        k_uptime_get_32();


    uint32_t interval =
        IDLE_ANIMATION_INTERVAL +
        breathing_interval_adjustment;


    if (interval < 250) {
        interval = 250;
    }


    /*
     * Idle animation.
     */

    if (
        !keys_active &&
        (now - last_idle_update >= interval)
    ) {

        last_idle_update =
            now;


        switch (current_idle_state) {

        case IDLE_INHALE:

            current_idle_state =
                IDLE_REST1;

            break;


        case IDLE_REST1:

            current_idle_state =
                IDLE_EXHALE;

            break;


        case IDLE_EXHALE:

            current_idle_state =
                IDLE_REST2;

            if (leaving_furious) {
                leaving_furious = false;
            }

            break;


        case IDLE_REST2:

        default:

            current_idle_state =
                IDLE_INHALE;

            breathing_interval_adjustment =
                get_random_adjustment();

            break;
        }


        struct zmk_widget_status *widget;


        SYS_SLIST_FOR_EACH_CONTAINER(
            &widgets,
            widget,
            node
        ) {

            draw_middle(
                widget->obj,
                widget->cbuf2,
                &widget->state
            );
        }
    }


    /*
     * Einmalige Key-Events löschen.
     */

    key_pressed = false;
    key_released = false;


    /*
     * Weiterlaufen.
     */

    k_work_schedule(
        &animation_work,
        K_MSEC(100)
    );
}


/* ==========================================================================
 * Initialization
 * ========================================================================== */

int zmk_widget_status_init(
    struct zmk_widget_status *widget,
    lv_obj_t *parent
) {

    /*
     * Das echte nice!view ist 160 x 68 logisch.
     *
     * Da es physisch hochkant eingebaut wird, verwenden wir
     * drei 68x68 Panels.
     *
     * Nach Rotation:
     *
     *     TOP
     *      ↓
     *   MIDDLE
     *      ↓
     *    BOTTOM
     *
     * = komplette 68 x 160 Fläche.
     */

    widget->obj =
        lv_obj_create(parent);


    lv_obj_set_size(
        widget->obj,
        160,
        68
    );


    /* ----------------------------------------------------------------------
     * TOP
     * ---------------------------------------------------------------------- */

    lv_obj_t *top =
        lv_canvas_create(widget->obj);


    lv_obj_align(
        top,
        LV_ALIGN_TOP_RIGHT,
        0,
        0
    );


    lv_canvas_set_buffer(
        top,
        widget->cbuf,
        CANVAS_SIZE,
        CANVAS_SIZE,
        LV_IMG_CF_TRUE_COLOR
    );


    /* ----------------------------------------------------------------------
     * MIDDLE
     * ---------------------------------------------------------------------- */

    lv_obj_t *middle =
        lv_canvas_create(widget->obj);


    /*
     * Ohne WPM-Graph können wir das mittlere Panel etwas weiter
     * nach rechts schieben.
     *
     * 92..160 = 68px
     * 24..92 = 68px
     * -44..24 = 68px
     *
     * Damit sind die drei Panels lückenlos.
     */

    lv_obj_align(
        middle,
        LV_ALIGN_TOP_LEFT,
        24,
        0
    );


    lv_canvas_set_buffer(
        middle,
        widget->cbuf2,
        CANVAS_SIZE,
        CANVAS_SIZE,
        LV_IMG_CF_TRUE_COLOR
    );


    /* ----------------------------------------------------------------------
     * BOTTOM
     * ---------------------------------------------------------------------- */

    lv_obj_t *bottom =
        lv_canvas_create(widget->obj);


    lv_obj_align(
        bottom,
        LV_ALIGN_TOP_LEFT,
        -44,
        0
    );


    lv_canvas_set_buffer(
        bottom,
        widget->cbuf3,
        CANVAS_SIZE,
        CANVAS_SIZE,
        LV_IMG_CF_TRUE_COLOR
    );


    /* ----------------------------------------------------------------------
     * Widget list
     * ---------------------------------------------------------------------- */

    sys_slist_append(
        &widgets,
        &widget->node
    );


    /* ----------------------------------------------------------------------
     * Listeners
     * ---------------------------------------------------------------------- */

    widget_battery_status_init();

    widget_output_status_init();

    widget_layer_status_init();

    widget_wpm_status_init();


    /* ----------------------------------------------------------------------
     * Modifier initial state
     * ---------------------------------------------------------------------- */

    for (int i = 0; i < NUM_SYMBOLS; i++) {

        modifier_symbols[i]->is_active =
            false;
    }


    widget->state.modifiers =
        0;


    /* ----------------------------------------------------------------------
     * Animation initial state
     * ---------------------------------------------------------------------- */

    current_anim_state =
        ANIM_STATE_CASUAL;

    current_idle_state =
        IDLE_INHALE;

    last_active_frame =
        &bongo_resting;

    last_idle_update =
        k_uptime_get_32();


    /* ----------------------------------------------------------------------
     * Work
     * ---------------------------------------------------------------------- */

    k_work_init_delayable(
        &animation_work,
        animation_work_handler
    );


    k_work_init_delayable(
        &modifier_work,
        modifier_work_handler
    );


    k_work_schedule(
        &animation_work,
        K_MSEC(100)
    );


    /* ----------------------------------------------------------------------
     * Initial draw
     * ---------------------------------------------------------------------- */

    draw_top(
        widget->obj,
        widget->cbuf,
        &widget->state
    );


    draw_middle(
        widget->obj,
        widget->cbuf2,
        &widget->state
    );


    draw_bottom(
        widget->obj,
        widget->cbuf3,
        &widget->state
    );


    return 0;
}


/* ==========================================================================
 * Object
 * ========================================================================== */

lv_obj_t *zmk_widget_status_obj(
    struct zmk_widget_status *widget
) {
    return widget->obj;
}/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>

#include <lvgl.h>

#include <zmk/battery.h>
#include <zmk/display.h>
#include "status.h"

#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/wpm_state_changed.h>

#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/wpm.h>
#include <zmk/hid.h>

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "bongocatart.h"


/* ==========================================================================
 * Modifier icons
 * ========================================================================== */

LV_IMG_DECLARE(control_icon);
LV_IMG_DECLARE(shift_icon);

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_MAC_MODIFIERS)
LV_IMG_DECLARE(opt_icon);
LV_IMG_DECLARE(cmd_icon);
#else
LV_IMG_DECLARE(alt_icon);
LV_IMG_DECLARE(win_icon);
#endif


/* ==========================================================================
 * Widget list
 * ========================================================================== */

static sys_slist_t widgets =
    SYS_SLIST_STATIC_INIT(&widgets);


/* ==========================================================================
 * Output state
 * ========================================================================== */

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};


/* ==========================================================================
 * Layer state
 * ========================================================================== */

struct layer_status_state {
    uint8_t index;
    const char *label;
};


/* ==========================================================================
 * WPM state
 *
 * WPM wird NICHT angezeigt.
 * Es steuert ausschließlich die Bongo-Animation.
 * ========================================================================== */

struct wpm_status_state {
    uint8_t wpm;
    bool is_key_event;
    bool key_pressed;
};


/* ==========================================================================
 * Bongo animation state
 * ========================================================================== */

enum anim_state {
    ANIM_STATE_CASUAL,
    ANIM_STATE_FRENZIED
};

static enum anim_state current_anim_state =
    ANIM_STATE_CASUAL;


enum idle_anim_state {
    IDLE_INHALE,
    IDLE_REST1,
    IDLE_EXHALE,
    IDLE_REST2
};

static enum idle_anim_state current_idle_state =
    IDLE_INHALE;


/* ==========================================================================
 * Timing
 * ========================================================================== */

static uint32_t last_idle_update = 0;

static const uint32_t IDLE_ANIMATION_INTERVAL =
    750;

static uint32_t last_key_event = 0;

static const uint32_t KEY_DEBOUNCE_INTERVAL =
    20;


/* ==========================================================================
 * Key state
 * ========================================================================== */

static bool key_pressed = false;
static bool key_released = false;
static bool keys_active = false;

static uint8_t active_keys = 0;


/* ==========================================================================
 * Bongo frame state
 * ========================================================================== */

static bool use_first_frame = true;

static const lv_img_dsc_t *last_active_frame =
    &bongo_resting;


/* ==========================================================================
 * Work
 * ========================================================================== */

static struct k_work_delayable animation_work;
static struct k_work_delayable modifier_work;

static bool modifier_work_scheduled = false;


/* ==========================================================================
 * Breathing variation
 * ========================================================================== */

static uint32_t random_seed = 7919;

static int32_t breathing_interval_adjustment = 0;

static bool leaving_furious = false;


/* ==========================================================================
 * Random breathing variation
 * ========================================================================== */

static int32_t get_random_adjustment(void) {

    static bool initialized = false;

    if (!initialized) {
        random_seed ^= k_uptime_get_32();
        initialized = true;
    }

    random_seed =
        random_seed * 1103515245 + 12345;

    return ((random_seed / 65536) % 501) - 250;
}


/* ==========================================================================
 * Current modifiers
 * ========================================================================== */

static uint8_t get_current_modifiers(void) {
    return zmk_hid_get_explicit_mods();
}


/* ==========================================================================
 * Set Bongo state according to WPM
 * ========================================================================== */

static void update_bongo_from_wpm(uint8_t wpm) {

    if (wpm > 30) {

        current_anim_state =
            ANIM_STATE_FRENZIED;

        leaving_furious = false;

        return;
    }


    if (current_anim_state == ANIM_STATE_FRENZIED) {

        current_anim_state =
            ANIM_STATE_CASUAL;

        leaving_furious = true;

        current_idle_state =
            IDLE_EXHALE;

        last_idle_update =
            k_uptime_get_32();
    }
}


/* ==========================================================================
 * Get current Bongo frame
 * ========================================================================== */

static const lv_img_dsc_t *get_bongo_frame(void) {

    /* ----------------------------------------------------------------------
     * Furious
     * ---------------------------------------------------------------------- */

    if (current_anim_state == ANIM_STATE_FRENZIED) {

        if (key_pressed || key_released) {

            if (use_first_frame) {
                last_active_frame =
                    &bongo_furiousup;
            } else {
                last_active_frame =
                    &bongo_furiousdown;
            }

            use_first_frame =
                !use_first_frame;

            return last_active_frame;
        }

        if (keys_active) {
            return last_active_frame;
        }
    }


    /* ----------------------------------------------------------------------
     * Casual
     * ---------------------------------------------------------------------- */

    else {

        if (key_pressed) {

            if (use_first_frame) {
                last_active_frame =
                    &bongo_casualright;
            } else {
                last_active_frame =
                    &bongo_casualleft;
            }

            use_first_frame =
                !use_first_frame;

            return last_active_frame;
        }

        if (keys_active) {
            return last_active_frame;
        }
    }


    /* ----------------------------------------------------------------------
     * Idle / breathing
     * ---------------------------------------------------------------------- */

    switch (current_idle_state) {

    case IDLE_INHALE:
        return &bongo_inhale;

    case IDLE_REST1:
        return &bongo_resting;

    case IDLE_EXHALE:
        return &bongo_exhale;

    case IDLE_REST2:
    default:
        return &bongo_resting;
    }
}


/* ==========================================================================
 * Draw Bongo
 * ========================================================================== */

static void draw_bongo(
    lv_obj_t *canvas,
    lv_draw_rect_dsc_t *background
) {

    /*
     * Bongo-Bereich löschen.
     *
     * Oben bleiben Akku / Verbindung.
     * Unten bleiben Modifier / Profil / Layer.
     */

    lv_canvas_draw_rect(
        canvas,
        0,
        10,
        CANVAS_SIZE,
        37,
        background
    );


    lv_draw_img_dsc_t img_dsc;

    lv_draw_img_dsc_init(
        &img_dsc
    );


    const lv_img_dsc_t *frame =
        get_bongo_frame();


    /*
     * Bildgröße aus LVGL lesen.
     */

    int img_w =
        frame->header.w;

    int img_h =
        frame->header.h;


    /*
     * Horizontal zentrieren.
     */

    int x =
        (CANVAS_SIZE - img_w) / 2;


    /*
     * Bongo oben im mittleren Bereich.
     */

    int y = 10;


    /*
     * Falls das Bild größer als der verfügbare Bereich ist,
     * wird es von LVGL am Canvas abgeschnitten.
     *
     * Dadurch können auch deine bisherigen 68px-Bongo-Bilder
     * weiterverwendet werden.
     */

    ARG_UNUSED(img_h);


    lv_canvas_draw_img(
        canvas,
        x,
        y,
        frame,
        &img_dsc
    );
}


/* ==========================================================================
 * Draw complete left display
 *
 * 68 x 68
 *
 * ┌────────────────────────────────────┐
 * │ Akku                         WiFi  │
 * │                                    │
 * │            BONGO CAT               │
 * │                                    │
 * │ Ctrl   Shift   Alt   Win            │
 * │                              ● 1   │
 * │                              L 0   │
 * └────────────────────────────────────┘
 *
 * ========================================================================== */

static void draw_status(
    struct zmk_widget_status *widget
) {

    lv_obj_t *canvas =
        lv_obj_get_child(widget->obj, 0);


    /* ----------------------------------------------------------------------
     * Background
     * ---------------------------------------------------------------------- */

    lv_draw_rect_dsc_t background;

    init_rect_dsc(
        &background,
        LVGL_BACKGROUND
    );


    lv_canvas_draw_rect(
        canvas,
        0,
        0,
        CANVAS_SIZE,
        CANVAS_SIZE,
        &background
    );


    /* ----------------------------------------------------------------------
     * Text style
     * ---------------------------------------------------------------------- */

    lv_draw_label_dsc_t label;

    init_label_dsc(
        &label,
        LVGL_FOREGROUND,
        &lv_font_montserrat_14,
        LV_TEXT_ALIGN_CENTER
    );


    /* ----------------------------------------------------------------------
     * 1. Battery
     * ---------------------------------------------------------------------- */

    draw_battery(
        canvas,
        &widget->state
    );


    /* ----------------------------------------------------------------------
     * 2. USB / Bluetooth connection
     * ---------------------------------------------------------------------- */

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

    char output_text[10] = {};


    switch (
        widget->state.selected_endpoint.transport
    ) {

    case ZMK_TRANSPORT_USB:

        strcat(
            output_text,
            LV_SYMBOL_USB
        );

        break;


    case ZMK_TRANSPORT_BLE:

        if (widget->state.active_profile_bonded) {

            if (widget->state.active_profile_connected) {

                strcat(
                    output_text,
                    LV_SYMBOL_WIFI
                );

            } else {

                strcat(
                    output_text,
                    LV_SYMBOL_CLOSE
                );
            }

        } else {

            strcat(
                output_text,
                LV_SYMBOL_SETTINGS
            );
        }

        break;


    default:
        break;
    }


    /*
     * Verbindung oben rechts.
     */

    lv_canvas_draw_text(
        canvas,
        45,
        1,
        21,
        &label,
        output_text
    );

#endif


    /* ----------------------------------------------------------------------
     * 3. Bongo Cat
     * ---------------------------------------------------------------------- */

    draw_bongo(
        canvas,
        &background
    );


    /* ----------------------------------------------------------------------
     * 4. Modifier
     * ---------------------------------------------------------------------- */

    /*
     * Deine vorhandene Modifier-Zeichenfunktion wird weiterverwendet.
     *
     * Dadurch bleiben deine vorhandenen Logos und deren Aktivierungs-
     * Logik erhalten.
     */

    draw_modifiers(
        canvas,
        0,
        47
    );


    /* ----------------------------------------------------------------------
     * 5. Bluetooth profile
     * ---------------------------------------------------------------------- */

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

    lv_draw_arc_dsc_t arc;

    init_arc_dsc(
        &arc,
        LVGL_FOREGROUND,
        2
    );


    lv_draw_arc_dsc_t arc_filled;

    init_arc_dsc(
        &arc_filled,
        LVGL_FOREGROUND,
        5
    );


    const int profile_x = 49;
    const int profile_y = 61;


    lv_canvas_draw_arc(
        canvas,
        profile_x,
        profile_y,
        7,
        0,
        360,
        &arc
    );


    lv_canvas_draw_arc(
        canvas,
        profile_x,
        profile_y,
        4,
        0,
        359,
        &arc_filled
    );


    lv_draw_label_dsc_t profile_label;

    init_label_dsc(
        &profile_label,
        LVGL_BACKGROUND,
        &lv_font_montserrat_14,
        LV_TEXT_ALIGN_CENTER
    );


    char profile_text[2] = {};


    snprintf(
        profile_text,
        sizeof(profile_text),
        "%" PRIu8,
        (uint8_t)(
            widget->state.active_profile_index + 1
        )
    );


    lv_canvas_draw_text(
        canvas,
        profile_x - 5,
        profile_y - 7,
        10,
        &profile_label,
        profile_text
    );

#endif


    /* ----------------------------------------------------------------------
     * 6. Layer
     * ---------------------------------------------------------------------- */

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

    char layer_text[16] = {};


    if (widget->state.layer_label != NULL) {

        snprintf(
            layer_text,
            sizeof(layer_text),
            "%s",
            widget->state.layer_label
        );

    } else {

        snprintf(
            layer_text,
            sizeof(layer_text),
            "L%" PRIu8,
            widget->state.layer_index
        );
    }


    /*
     * Layer unten rechts.
     */

    lv_canvas_draw_text(
        canvas,
        56,
        55,
        12,
        &label,
        layer_text
    );

#endif


    /* ----------------------------------------------------------------------
     * Rotate
     * ---------------------------------------------------------------------- */

    rotate_canvas(
        canvas,
        widget->cbuf
    );
}


/* ==========================================================================
 * Battery
 * ========================================================================== */

static void set_battery_status(
    struct zmk_widget_status *widget,
    struct battery_status_state state
) {

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)

    widget->state.charging =
        state.usb_present;

#endif

    widget->state.battery =
        state.level;


    draw_status(widget);
}


static void battery_status_update_cb(
    struct battery_status_state state
) {

    struct zmk_widget_status *widget;


    SYS_SLIST_FOR_EACH_CONTAINER(
        &widgets,
        widget,
        node
    ) {

        set_battery_status(
            widget,
            state
        );
    }
}


static struct battery_status_state
battery_status_get_state(
    const zmk_event_t *eh
) {

    const struct zmk_battery_state_changed *ev =
        as_zmk_battery_state_changed(eh);


    return (struct battery_status_state) {

        .level =
            ev != NULL
                ? ev->state_of_charge
                : zmk_battery_state_of_charge(),

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)

        .usb_present =
            zmk_usb_is_powered(),

#endif
    };
}


ZMK_DISPLAY_WIDGET_LISTENER(
    widget_battery_status,
    struct battery_status_state,
    battery_status_update_cb,
    battery_status_get_state
);


ZMK_SUBSCRIPTION(
    widget_battery_status,
    zmk_battery_state_changed
);


#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)

ZMK_SUBSCRIPTION(
    widget_battery_status,
    zmk_usb_conn_state_changed
);

#endif


/* ==========================================================================
 * Output / Bluetooth profile
 * ========================================================================== */

static void set_output_status(
    struct zmk_widget_status *widget,
    const struct output_status_state *state
) {

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

    widget->state.selected_endpoint =
        state->selected_endpoint;

    widget->state.active_profile_index =
        state->active_profile_index;

    widget->state.active_profile_connected =
        state->active_profile_connected;

    widget->state.active_profile_bonded =
        state->active_profile_bonded;

#endif


    draw_status(widget);
}


static void output_status_update_cb(
    struct output_status_state state
) {

    struct zmk_widget_status *widget;


    SYS_SLIST_FOR_EACH_CONTAINER(
        &widgets,
        widget,
        node
    ) {

        set_output_status(
            widget,
            &state
        );
    }
}


static struct output_status_state
output_status_get_state(
    const zmk_event_t *eh
) {

    return (struct output_status_state) {

        .selected_endpoint =
            zmk_endpoints_selected(),

        .active_profile_index =
            zmk_ble_active_profile_index(),

        .active_profile_connected =
            zmk_ble_active_profile_is_connected(),

        .active_profile_bonded =
            !zmk_ble_active_profile_is_open(),
    };
}


ZMK_DISPLAY_WIDGET_LISTENER(
    widget_output_status,
    struct output_status_state,
    output_status_update_cb,
    output_status_get_state
);


ZMK_SUBSCRIPTION(
    widget_output_status,
    zmk_endpoint_changed
);


#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)

ZMK_SUBSCRIPTION(
    widget_output_status,
    zmk_usb_conn_state_changed
);

#endif


#if defined(CONFIG_ZMK_BLE)

ZMK_SUBSCRIPTION(
    widget_output_status,
    zmk_ble_active_profile_changed
);

#endif


/* ==========================================================================
 * Layer
 * ========================================================================== */

static void set_layer_status(
    struct zmk_widget_status *widget,
    struct layer_status_state state
) {

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

    widget->state.layer_index =
        state.index;

    widget->state.layer_label =
        state.label;

#endif


    draw_status(widget);
}


static void layer_status_update_cb(
    struct layer_status_state state
) {

    struct zmk_widget_status *widget;


    SYS_SLIST_FOR_EACH_CONTAINER(
        &widgets,
        widget,
        node
    ) {

        set_layer_status(
            widget,
            state
        );
    }
}


static struct layer_status_state
layer_status_get_state(
    const zmk_event_t *eh
) {

    uint8_t index =
        zmk_keymap_highest_layer_active();


    return (struct layer_status_state) {

        .index = index,

        .label =
            zmk_keymap_layer_name(index),
    };
}


ZMK_DISPLAY_WIDGET_LISTENER(
    widget_layer_status,
    struct layer_status_state,
    layer_status_update_cb,
    layer_status_get_state
);


ZMK_SUBSCRIPTION(
    widget_layer_status,
    zmk_layer_state_changed
);


/* ==========================================================================
 * Keypress handling
 * ========================================================================== */

static void process_keypress_event(
    bool pressed
) {

    key_pressed =
        pressed;

    key_released =
        !pressed;

    last_key_event =
        k_uptime_get_32();


    if (pressed) {

        if (active_keys < UINT8_MAX) {
            active_keys++;
        }

        keys_active = true;

    } else {

        if (active_keys > 0) {
            active_keys--;
        }

        keys_active =
            active_keys > 0;
    }


    /*
     * Modifier regelmäßig aktualisieren.
     */

    if (!modifier_work_scheduled) {

        modifier_work_scheduled = true;

        k_work_schedule(
            &modifier_work,
            K_MSEC(20)
        );
    }


    /*
     * Bongo sofort aktualisieren.
     */

    struct zmk_widget_status *widget;


    SYS_SLIST_FOR_EACH_CONTAINER(
        &widgets,
        widget,
        node
    ) {

        draw_status(widget);
    }
}


/* ==========================================================================
 * Modifier worker
 * ========================================================================== */

static void modifier_work_handler(
    struct k_work *work
) {

    ARG_UNUSED(work);


    modifier_work_scheduled =
        false;


    uint8_t mods =
        get_current_modifiers();


    struct zmk_widget_status *widget;


    SYS_SLIST_FOR_EACH_CONTAINER(
        &widgets,
        widget,
        node
    ) {

        if (widget->state.modifiers != mods) {

            widget->state.modifiers =
                mods;


            for (int i = 0; i < NUM_SYMBOLS; i++) {

                modifier_symbols[i]->is_active =
                    (mods &
                     modifier_symbols[i]->modifier) != 0;
            }


            draw_status(widget);
        }
    }


    /*
     * Solange Tasten gehalten werden, weiter prüfen.
     */

    if (keys_active) {

        modifier_work_scheduled =
            true;

        k_work_schedule(
            &modifier_work,
            K_MSEC(20)
        );
    }
}


/* ==========================================================================
 * WPM event
 * ========================================================================== */

static struct wpm_status_state
wpm_status_get_state(
    const zmk_event_t *eh
) {

    static uint8_t current_wpm = 0;


    const struct zmk_wpm_state_changed *wpm_ev =
        as_zmk_wpm_state_changed(eh);

    const struct zmk_position_state_changed *pos_ev =
        as_zmk_position_state_changed(eh);


    bool is_key_event = false;
    bool pressed = false;


    /* WPM */

    if (wpm_ev != NULL) {

        current_wpm =
            wpm_ev->state;

        update_bongo_from_wpm(
            current_wpm
        );
    }


    /* Key */

    if (pos_ev != NULL) {

        is_key_event =
            true;

        pressed =
            pos_ev->state > 0;
    }


    return (struct wpm_status_state) {

        .wpm =
            current_wpm,

        .is_key_event =
            is_key_event,

        .key_pressed =
            pressed,
    };
}


/* ==========================================================================
 * WPM update
 * ========================================================================== */

static void wpm_status_update_cb(
    struct wpm_status_state state
) {

    struct zmk_widget_status *widget;


    SYS_SLIST_FOR_EACH_CONTAINER(
        &widgets,
        widget,
        node
    ) {

        if (state.is_key_event) {

            process_keypress_event(
                state.key_pressed
            );

        } else {

            /*
             * WPM wird NICHT angezeigt.
             *
             * Nur Bongo neu zeichnen.
             */

            draw_status(widget);
        }
    }
}


ZMK_DISPLAY_WIDGET_LISTENER(
    widget_wpm_status,
    struct wpm_status_state,
    wpm_status_update_cb,
    wpm_status_get_state
);


ZMK_SUBSCRIPTION(
    widget_wpm_status,
    zmk_wpm_state_changed
);


ZMK_SUBSCRIPTION(
    widget_wpm_status,
    zmk_position_state_changed
);


/* ==========================================================================
 * Animation worker
 * ========================================================================== */

static void animation_work_handler(
    struct k_work *work
) {

    ARG_UNUSED(work);


    uint32_t now =
        k_uptime_get_32();


    uint32_t interval =
        IDLE_ANIMATION_INTERVAL +
        breathing_interval_adjustment;


    if (interval < 200) {
        interval = 200;
    }


    /*
     * Idle animation nur wenn keine Taste gehalten wird.
     */

    if (
        !keys_active &&
        (now - last_idle_update >= interval)
    ) {

        last_idle_update =
            now;


        switch (current_idle_state) {

        case IDLE_INHALE:

            current_idle_state =
                IDLE_REST1;

            break;


        case IDLE_REST1:

            current_idle_state =
                IDLE_EXHALE;

            break;


        case IDLE_EXHALE:

            current_idle_state =
                IDLE_REST2;

            if (leaving_furious) {
                leaving_furious = false;
            }

            break;


        case IDLE_REST2:

        default:

            current_idle_state =
                IDLE_INHALE;

            breathing_interval_adjustment =
                get_random_adjustment();

            break;
        }


        struct zmk_widget_status *widget;


        SYS_SLIST_FOR_EACH_CONTAINER(
            &widgets,
            widget,
            node
        ) {

            draw_status(widget);
        }
    }


    /*
     * Key flags nach dem Zeichnen zurücksetzen.
     */

    key_pressed = false;
    key_released = false;


    /*
     * Worker weiterlaufen lassen.
     */

    k_work_schedule(
        &animation_work,
        K_MSEC(100)
    );
}


/* ==========================================================================
 * Initialization
 * ========================================================================== */

int zmk_widget_status_init(
    struct zmk_widget_status *widget,
    lv_obj_t *parent
) {

    /*
     * ================================================================
     * WICHTIG:
     *
     * Dies ist ein einzelnes 68x68 Display.
     *
     * Kein 160x68!
     * Kein Artwork!
     * ================================================================
     */

    widget->obj =
        lv_obj_create(parent);


    lv_obj_set_size(
        widget->obj,
        68,
        68
    );


    /*
     * Canvas
     */

    lv_obj_t *canvas =
        lv_canvas_create(widget->obj);


    lv_obj_align(
        canvas,
        LV_ALIGN_TOP_LEFT,
        0,
        0
    );


    lv_canvas_set_buffer(
        canvas,
        widget->cbuf,
        CANVAS_SIZE,
        CANVAS_SIZE,
        LV_IMG_CF_TRUE_COLOR
    );


    /*
     * Widget registrieren.
     */

    sys_slist_append(
        &widgets,
        &widget->node
    );


    /*
     * Listener initialisieren.
     */

    widget_battery_status_init();

    widget_output_status_init();

    widget_layer_status_init();

    widget_wpm_status_init();


    /*
     * Modifier initialisieren.
     */

    for (int i = 0; i < NUM_SYMBOLS; i++) {

        modifier_symbols[i]->is_active =
            false;
    }

    widget->state.modifiers =
        0;


    /*
     * Animation initialisieren.
     */

    current_anim_state =
        ANIM_STATE_CASUAL;

    current_idle_state =
        IDLE_INHALE;

    last_active_frame =
        &bongo_resting;

    last_idle_update =
        k_uptime_get_32();


    /*
     * Work initialisieren.
     */

    k_work_init_delayable(
        &animation_work,
        animation_work_handler
    );

    k_work_init_delayable(
        &modifier_work,
        modifier_work_handler
    );


    /*
     * Initial zeichnen.
     */

    draw_status(widget);


    /*
     * Animation starten.
     */

    k_work_schedule(
        &animation_work,
        K_MSEC(100)
    );


    return 0;
}


/* ==========================================================================
 * Object
 * ========================================================================== */

lv_obj_t *zmk_widget_status_obj(
    struct zmk_widget_status *widget
) {

    return widget->obj;
}
