/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

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

#include <dt-bindings/zmk/modifiers.h>

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

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);


/* ==========================================================================
 * Output status
 * ========================================================================== */

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};


/* ==========================================================================
 * Layer status
 * ========================================================================== */

struct layer_status_state {
    uint8_t index;
    const char *label;
};


/* ==========================================================================
 * WPM status
 *
 * WPM wird NICHT angezeigt.
 * Es wird ausschließlich für die Bongo-Animation verwendet.
 * ========================================================================== */

struct wpm_status_state {
    uint8_t wpm;
    bool key_pressed;
    bool is_key_event;
};


/* ==========================================================================
 * Bongo animation
 * ========================================================================== */

enum anim_state {
    ANIM_STATE_CASUAL,
    ANIM_STATE_FRENZIED
};

static enum anim_state current_anim_state = ANIM_STATE_CASUAL;


enum idle_anim_state {
    IDLE_INHALE,
    IDLE_REST1,
    IDLE_EXHALE,
    IDLE_REST2
};

static enum idle_anim_state current_idle_state = IDLE_INHALE;


/* ==========================================================================
 * Timing
 * ========================================================================== */

static uint32_t last_idle_update = 0;

static const uint32_t IDLE_ANIMATION_INTERVAL = 750;

static uint32_t last_key_event = 0;

static const uint32_t KEY_DEBOUNCE_INTERVAL = 20;


/* ==========================================================================
 * Key state
 * ========================================================================== */

static bool key_pressed = false;
static bool key_released = false;
static bool keys_active = false;

static uint8_t active_keys = 0;


/* ==========================================================================
 * Animation frame state
 * ========================================================================== */

static bool use_first_frame = true;

static const lv_img_dsc_t *last_active_frame = &bongo_resting;


/* ==========================================================================
 * Animation work
 * ========================================================================== */

static struct k_work_delayable animation_work;
static struct k_work_delayable modifier_work;

static bool modifier_check_scheduled = false;

static const uint32_t MODIFIER_CHECK_INTERVAL = 20;


/* ==========================================================================
 * Breathing variation
 * ========================================================================== */

static uint32_t random_seed = 7919;

static int32_t breathing_interval_adjustment = 0;

static bool leaving_furious = false;


/* ==========================================================================
 * Random number
 * ========================================================================== */

static int32_t get_random_adjustment(void) {

    static bool seed_initialized = false;

    if (!seed_initialized) {
        random_seed ^= k_uptime_get_32();
        seed_initialized = true;
    }

    random_seed =
        random_seed * 1103515245 + 12345;

    return ((random_seed / 65536) % 501) - 250;
}


/* ==========================================================================
 * Current modifier state
 * ========================================================================== */

static uint8_t get_current_modifiers(void) {

    return zmk_hid_get_explicit_mods();
}


/* ==========================================================================
 * Bongo frame
 * ========================================================================== */

static const lv_img_dsc_t *get_current_bongo_frame(void) {

    /* ---------------------------------------------------------------
     * Furious mode
     * --------------------------------------------------------------- */

    if (current_anim_state == ANIM_STATE_FRENZIED) {

        if (key_pressed || key_released) {

            if (use_first_frame) {
                last_active_frame = &bongo_furiousup;
            } else {
                last_active_frame = &bongo_furiousdown;
            }

            use_first_frame = !use_first_frame;

            return last_active_frame;
        }

        if (keys_active) {
            return last_active_frame;
        }
    }


    /* ---------------------------------------------------------------
     * Casual mode
     * --------------------------------------------------------------- */

    else {

        if (key_pressed) {

            if (use_first_frame) {
                last_active_frame = &bongo_casualright;
            } else {
                last_active_frame = &bongo_casualleft;
            }

            use_first_frame = !use_first_frame;

            return last_active_frame;
        }

        if (keys_active) {
            return last_active_frame;
        }
    }


    /* ---------------------------------------------------------------
     * Idle animation
     * --------------------------------------------------------------- */

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
 * Update Bongo based on WPM
 * ========================================================================== */

static void update_animation_from_wpm(uint8_t wpm) {

    /*
     * More than 30 WPM = furious.
     */

    if (wpm > 30) {

        current_anim_state =
            ANIM_STATE_FRENZIED;

        leaving_furious = false;

        return;
    }


    /*
     * Going from furious back to casual.
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
 * Draw complete 68x68 status area
 *
 * Layout:
 *
 *   ┌────────────────────────────────────┐
 *   │ 🔋                         WiFi    │
 *   │                                    │
 *   │             BONGO CAT              │
 *   │                                    │
 *   │  Ctrl  Shift  Alt  Win             │
 *   │                           ●1  L0    │
 *   └────────────────────────────────────┘
 *
 * ========================================================================== */

static void draw_status(struct zmk_widget_status *widget) {

    lv_obj_t *canvas =
        lv_obj_get_child(widget->obj, 0);


    /* ----------------------------------------------------------------------
     * Background
     * ---------------------------------------------------------------------- */

    lv_draw_rect_dsc_t rect_black_dsc;

    init_rect_dsc(
        &rect_black_dsc,
        LVGL_BACKGROUND
    );

    lv_canvas_draw_rect(
        canvas,
        0,
        0,
        CANVAS_SIZE,
        CANVAS_SIZE,
        &rect_black_dsc
    );


    /* ----------------------------------------------------------------------
     * Text styles
     * ---------------------------------------------------------------------- */

    lv_draw_label_dsc_t label_dsc;

    init_label_dsc(
        &label_dsc,
        LVGL_FOREGROUND,
        &lv_font_montserrat_14,
        LV_TEXT_ALIGN_CENTER
    );


    lv_draw_label_dsc_t profile_label_dsc;

    init_label_dsc(
        &profile_label_dsc,
        LVGL_BACKGROUND,
        &lv_font_montserrat_14,
        LV_TEXT_ALIGN_CENTER
    );


    /* ----------------------------------------------------------------------
     * 1. BATTERY
     * ---------------------------------------------------------------------- */

    draw_battery(
        canvas,
        &widget->state
    );


    /* ----------------------------------------------------------------------
     * 2. USB / WIFI
     * ---------------------------------------------------------------------- */

    char output_text[10] = {};

    switch (widget->state.selected_endpoint.transport) {

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
     * WiFi / USB oben rechts.
     */

    lv_canvas_draw_text(
        canvas,
        45,
        1,
        20,
        &label_dsc,
        output_text
    );


    /* ----------------------------------------------------------------------
     * 3. BONGO CAT
     * ---------------------------------------------------------------------- */

    lv_draw_img_dsc_t img_dsc;

    lv_draw_img_dsc_init(
        &img_dsc
    );


    const lv_img_dsc_t *current_frame =
        get_current_bongo_frame();


    /*
     * Bongo-Bereich löschen.
     *
     * Dadurch bleiben keine Pixel des vorherigen Frames stehen.
     */

    lv_canvas_draw_rect(
        canvas,
        0,
        12,
        CANVAS_SIZE,
        34,
        &rect_black_dsc
    );


    /*
     * Bongo zeichnen.
     *
     * Die Bongo-Bilder aus deinem bisherigen Code wurden
     * bereits bei y=28 in einem 68x68 Canvas gezeichnet.
     *
     * Hier sitzt der Bongo weiter oben, damit unten Platz
     * für Modifier / Profil / Layer bleibt.
     */

    lv_canvas_draw_img(
        canvas,
        0,
        12,
        current_frame,
        &img_dsc
    );


    /* ----------------------------------------------------------------------
     * 4. MODIFIERS
     * ---------------------------------------------------------------------- */

    /*
     * Deine vorhandene draw_modifiers()-Implementierung bleibt erhalten.
     *
     * y=45 ist bewusst gewählt:
     *
     *   Bongo       ~12-45
     *   Modifier    ~45-57
     *   Profile     ~58-68
     */

    draw_modifiers(
        canvas,
        0,
        45
    );


    /* ----------------------------------------------------------------------
     * 5. BLUETOOTH PROFILE
     * ---------------------------------------------------------------------- */

    lv_draw_arc_dsc_t arc_dsc;

    init_arc_dsc(
        &arc_dsc,
        LVGL_FOREGROUND,
        2
    );


    lv_draw_arc_dsc_t arc_dsc_filled;

    init_arc_dsc(
        &arc_dsc_filled,
        LVGL_FOREGROUND,
        6
    );


    /*
     * Kleiner Profil-Kreis unten rechts.
     */

    const int profile_x = 48;
    const int profile_y = 61;


    lv_canvas_draw_arc(
        canvas,
        profile_x,
        profile_y,
        7,
        0,
        360,
        &arc_dsc
    );


    lv_canvas_draw_arc(
        canvas,
        profile_x,
        profile_y,
        4,
        0,
        359,
        &arc_dsc_filled
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
        &profile_label_dsc,
        profile_text
    );


    /* ----------------------------------------------------------------------
     * 6. LAYER
     * ---------------------------------------------------------------------- */

    char layer_text[16] = {};


    if (widget->state.layer_label == NULL) {

        snprintf(
            layer_text,
            sizeof(layer_text),
            "L%" PRIu8,
            widget->state.layer_index
        );

    } else {

        snprintf(
            layer_text,
            sizeof(layer_text),
            "%s",
            widget->state.layer_label
        );
    }


    /*
     * Layer ganz unten rechts.
     */

    lv_canvas_draw_text(
        canvas,
        54,
        54,
        14,
        &label_dsc,
        layer_text
    );


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


static struct battery_status_state battery_status_get_state(
    const zmk_event_t *eh
) {

    const struct zmk_battery_state_changed *ev =
        as_zmk_battery_state_changed(eh);


    return (struct battery_status_state) {

        .level =
            (ev != NULL)
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

    widget->state.selected_endpoint =
        state->selected_endpoint;

    widget->state.active_profile_index =
        state->active_profile_index;

    widget->state.active_profile_connected =
        state->active_profile_connected;

    widget->state.active_profile_bonded =
        state->active_profile_bonded;

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


static struct output_status_state output_status_get_state(
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

    widget->state.layer_index =
        state.index;

    widget->state.layer_label =
        state.label;

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


static struct layer_status_state layer_status_get_state(
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
    bool is_pressed
) {

    key_pressed =
        is_pressed;

    key_released =
        !is_pressed;

    last_key_event =
        k_uptime_get_32();


    if (is_pressed) {

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
     * Modifier update anstoßen.
     */

    if (!modifier_check_scheduled) {

        modifier_check_scheduled = true;

        k_work_schedule(
            &modifier_work,
            K_MSEC(MODIFIER_CHECK_INTERVAL)
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
 * Modifier work
 * ========================================================================== */

static void modifier_work_handler(
    struct k_work *work
) {

    ARG_UNUSED(work);

    modifier_check_scheduled = false;


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
                    (mods & modifier_symbols[i]->modifier) != 0;
            }


            draw_status(widget);
        }
    }


    /*
     * Solange Tasten gehalten werden, Modifier weiter prüfen.
     */

    if (keys_active) {

        if (!modifier_check_scheduled) {

            modifier_check_scheduled = true;

            k_work_schedule(
                &modifier_work,
                K_MSEC(MODIFIER_CHECK_INTERVAL)
            );
        }
    }
}


/* ==========================================================================
 * WPM state
 * ========================================================================== */

static struct wpm_status_state wpm_status_get_state(
    const zmk_event_t *eh
) {

    static uint8_t current_wpm = 0;


    const struct zmk_wpm_state_changed *wpm_ev =
        as_zmk_wpm_state_changed(eh);

    const struct zmk_position_state_changed *pos_ev =
        as_zmk_position_state_changed(eh);


    bool is_key_event = false;
    bool key_is_pressed = false;


    /*
     * WPM event
     */

    if (wpm_ev != NULL) {

        current_wpm =
            wpm_ev->state;

        update_animation_from_wpm(
            current_wpm
        );
    }


    /*
     * Key event
     */

    if (pos_ev != NULL) {

        is_key_event = true;

        key_is_pressed =
            pos_ev->state > 0;
    }


    return (struct wpm_status_state) {

        .wpm =
            current_wpm,

        .key_pressed =
            key_is_pressed,

        .is_key_event =
            is_key_event,
    };
}


/* ==========================================================================
 * WPM callback
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

        /*
         * Key event.
         */

        if (state.is_key_event) {

            process_keypress_event(
                state.key_pressed
            );

        } else {

            /*
             * WPM event.
             *
             * WPM wird NICHT gezeichnet.
             * Nur Bongo-Zustand wird aktualisiert.
             */

            widget->state.wpm[9] =
                state.wpm;

            draw_status(widget);
        }
    }
}


/* ==========================================================================
 * WPM listener
 * ========================================================================== */

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
 * Animation work
 * ========================================================================== */

static void animation_work_handler(
    struct k_work *work
) {

    ARG_UNUSED(work);


    uint32_t now =
        k_uptime_get_32();


    /*
     * Idle animation only when no key is held.
     */

    uint32_t interval =
        IDLE_ANIMATION_INTERVAL +
        breathing_interval_adjustment;


    if (interval < 100) {
        interval = 100;
    }


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
     * Worker alle 100 ms erneut ausführen.
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
     * ------------------------------------------------------------
     * Gesamt-Widget
     *
     * WICHTIG:
     * Das Display bleibt 160x68.
     * Wir verwenden davon links 68px.
     * ------------------------------------------------------------
     */

    widget->obj =
        lv_obj_create(parent);

    lv_obj_set_size(
        widget->obj,
        160,
        68
    );


    /*
     * ------------------------------------------------------------
     * EIN Canvas für die linke 68px-Hälfte.
     * ------------------------------------------------------------
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
     * ------------------------------------------------------------
     * Widget registrieren.
     * ------------------------------------------------------------
     */

    sys_slist_append(
        &widgets,
        &widget->node
    );


    /*
     * ------------------------------------------------------------
     * Listener initialisieren.
     * ------------------------------------------------------------
     */

    widget_battery_status_init();

    widget_output_status_init();

    widget_layer_status_init();

    widget_wpm_status_init();


    /*
     * ------------------------------------------------------------
     * Modifier initialisieren.
     * ------------------------------------------------------------
     */

    for (int i = 0; i < NUM_SYMBOLS; i++) {

        modifier_symbols[i]->is_active =
            false;
    }

    widget->state.modifiers =
        0;


    /*
     * ------------------------------------------------------------
     * Animation initialisieren.
     * ------------------------------------------------------------
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
     * ------------------------------------------------------------
     * Work Items
     * ------------------------------------------------------------
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
     * ------------------------------------------------------------
     * Initial render.
     * ------------------------------------------------------------
     */

    draw_status(widget);


    /*
     * ------------------------------------------------------------
     * Animation starten.
     * ------------------------------------------------------------
     */

    k_work_schedule(
        &animation_work,
        K_MSEC(100)
    );


    return 0;
}


/* ==========================================================================
 * Widget object
 * ========================================================================== */

lv_obj_t *zmk_widget_status_obj(
    struct zmk_widget_status *widget
) {

    return widget->obj;
}
