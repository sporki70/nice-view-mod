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

#include <dt-bindings/zmk/modifiers.h>

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "bongocatart.h"


/* --------------------------------------------------------------------------
 * Modifier icons
 * -------------------------------------------------------------------------- */

LV_IMG_DECLARE(control_icon);
LV_IMG_DECLARE(shift_icon);

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_MAC_MODIFIERS)
LV_IMG_DECLARE(opt_icon);
LV_IMG_DECLARE(cmd_icon);
#else
LV_IMG_DECLARE(alt_icon);
LV_IMG_DECLARE(win_icon);
#endif


/* --------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------- */

static uint8_t get_current_modifiers(void);
static void draw_status(struct zmk_widget_status *widget);
static void process_keypress_event(bool is_pressed);


/* --------------------------------------------------------------------------
 * Global widget list
 * -------------------------------------------------------------------------- */

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);


/* --------------------------------------------------------------------------
 * Status state structures
 * -------------------------------------------------------------------------- */

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};

struct layer_status_state {
    uint8_t index;
    const char *label;
};


/* --------------------------------------------------------------------------
 * WPM state
 *
 * WPM is still used for the Bongo Cat animation.
 * Nothing related to the WPM graph is drawn anymore.
 * -------------------------------------------------------------------------- */

struct wpm_status_state {
    uint8_t wpm;
    uint8_t wpm_history[10];

    bool key_pressed;
    bool is_key_event;
    bool is_animation_update;
};


/* --------------------------------------------------------------------------
 * Bongo animation state
 * -------------------------------------------------------------------------- */

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


/*
 * Timing
 */

static uint32_t last_idle_update = 0;

static const uint32_t IDLE_ANIMATION_INTERVAL = 750;

static uint32_t last_key_event = 0;

static const uint32_t KEY_DEBOUNCE_INTERVAL = 20;


/*
 * Animation frames
 */

static bool key_pressed = false;
static bool key_released = false;
static bool keys_active = false;

static bool use_first_frame = true;

static const lv_img_dsc_t *last_active_frame = &bongo_resting;


/*
 * Number of currently pressed keys.
 *
 * This is intentionally kept as a counter because multiple keys can
 * be held simultaneously.
 */

static uint8_t active_keys = 0;


/*
 * Work items
 */

static struct k_work_delayable animation_work;
static struct k_work_delayable modifier_work;


/*
 * Modifier polling interval.
 */

static const uint32_t MODIFIER_CHECK_INTERVAL = 20;


/*
 * Used to avoid scheduling duplicate modifier checks.
 */

static bool modifier_check_scheduled = false;


/*
 * Used to slightly randomize breathing.
 */

static uint32_t random_seed = 7919;

static int32_t breathing_interval_adjustment = 0;


/*
 * Used when switching from furious to casual.
 */

static bool leaving_furious = false;


/* --------------------------------------------------------------------------
 * Random adjustment
 * -------------------------------------------------------------------------- */

static int32_t get_random_adjustment(void) {
    static bool seed_initialized = false;

    if (!seed_initialized) {
        random_seed ^= k_uptime_get_32();
        seed_initialized = true;
    }

    random_seed = random_seed * 1103515245 + 12345;

    return ((random_seed / 65536) % 501) - 250;
}


/* --------------------------------------------------------------------------
 * Helper: current Bongo frame
 * -------------------------------------------------------------------------- */

static const lv_img_dsc_t *get_current_bongo_frame(void) {

    /*
     * ------------------------------------------------------------
     * Furious mode
     * ------------------------------------------------------------
     */

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


    /*
     * ------------------------------------------------------------
     * Casual mode
     * ------------------------------------------------------------
     */

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


    /*
     * ------------------------------------------------------------
     * Idle / breathing animation
     * ------------------------------------------------------------
     */

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


/* --------------------------------------------------------------------------
 * Draw complete left-side status display
 *
 * Layout:
 *
 *   0 -------------------------------------------------------- 68
 *
 *   ┌──────────────────────────────────────────────────────────┐
 *   │ battery / wifi                                           │
 *   │                                                          │
 *   │                 BONGO CAT                               │
 *   │                                                          │
 *   │              modifier icons                             │
 *   │                                                          │
 *   │                       BT profile     Layer                │
 *   └──────────────────────────────────────────────────────────┘
 *
 * -------------------------------------------------------------------------- */

static void draw_status(struct zmk_widget_status *widget) {

    lv_obj_t *canvas = lv_obj_get_child(widget->obj, 0);

    /*
     * ------------------------------------------------------------
     * Background
     * ------------------------------------------------------------
     */

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    lv_canvas_draw_rect(
        canvas,
        0,
        0,
        CANVAS_SIZE,
        CANVAS_SIZE,
        &rect_black_dsc
    );


    /*
     * ------------------------------------------------------------
     * Fonts
     * ------------------------------------------------------------
     */

    lv_draw_label_dsc_t label_small;
    init_label_dsc(
        &label_small,
        LVGL_FOREGROUND,
        &lv_font_montserrat_14,
        LV_TEXT_ALIGN_CENTER
    );

    lv_draw_label_dsc_t label_profile;
    init_label_dsc(
        &label_profile,
        LVGL_BACKGROUND,
        &lv_font_montserrat_14,
        LV_TEXT_ALIGN_CENTER
    );


    /*
     * ------------------------------------------------------------
     * 1. BATTERY
     * ------------------------------------------------------------
     *
     * Keep using your existing battery renderer.
     */

    draw_battery(canvas, &widget->state);


    /*
     * ------------------------------------------------------------
     * 2. USB / WIFI
     * ------------------------------------------------------------
     */

    char output_text[10] = {};

    switch (widget->state.selected_endpoint.transport) {

    case ZMK_TRANSPORT_USB:
        strcat(output_text, LV_SYMBOL_USB);
        break;

    case ZMK_TRANSPORT_BLE:

        if (widget->state.active_profile_bonded) {

            if (widget->state.active_profile_connected) {
                strcat(output_text, LV_SYMBOL_WIFI);
            } else {
                strcat(output_text, LV_SYMBOL_CLOSE);
            }

        } else {
            strcat(output_text, LV_SYMBOL_SETTINGS);
        }

        break;

    default:
        break;
    }


    /*
     * WiFi / USB is placed in the upper-right area.
     */

    lv_canvas_draw_text(
        canvas,
        42,
        2,
        24,
        &label_small,
        output_text
    );


    /*
     * ------------------------------------------------------------
     * 3. BONGO CAT
     * ------------------------------------------------------------
     */

    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);

    const lv_img_dsc_t *current_frame =
        get_current_bongo_frame();


    /*
     * Clear Bongo area first.
     *
     * This prevents old pixels from remaining when switching
     * between animation frames.
     */

    lv_canvas_draw_rect(
        canvas,
        0,
        14,
        CANVAS_SIZE,
        32,
        &rect_black_dsc
    );


    /*
     * Bongo image.
     *
     * The original images are 68px wide, so keep them centered
     * on the 68px status display.
     */

    lv_canvas_draw_img(
        canvas,
        0,
        14,
        current_frame,
        &img_dsc
    );


    /*
     * key_pressed is an event flag.
     * Once the frame has been consumed, clear it.
     */

    key_pressed = false;
    key_released = false;


    /*
     * ------------------------------------------------------------
     * 4. MODIFIERS
     * ------------------------------------------------------------
     *
     * Your existing draw_modifiers() is reused.
     *
     * This means your existing modifier logos remain intact.
     */

    draw_modifiers(
        canvas,
        0,
        45
    );


    /*
     * ------------------------------------------------------------
     * 5. BLUETOOTH PROFILE
     * ------------------------------------------------------------
     */

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
        7
    );


    /*
     * Profile circle.
     *
     * Bottom-left of the display.
     */

    const int profile_x = 48;
    const int profile_y = 59;

    lv_canvas_draw_arc(
        canvas,
        profile_x,
        profile_y,
        8,
        0,
        360,
        &arc_dsc
    );

    lv_canvas_draw_arc(
        canvas,
        profile_x,
        profile_y,
        5,
        0,
        359,
        &arc_dsc_filled
    );


    char profile_label[2];

    snprintf(
        profile_label,
        sizeof(profile_label),
        "%" PRIu8,
        (uint8_t)(widget->state.active_profile_index + 1)
    );


    lv_canvas_draw_text(
        canvas,
        profile_x - 5,
        profile_y - 7,
        10,
        &label_profile,
        profile_label
    );


    /*
     * ------------------------------------------------------------
     * 6. LAYER
     * ------------------------------------------------------------
     */

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
     * Layer is placed in the bottom-right.
     */

    lv_canvas_draw_text(
        canvas,
        54,
        54,
        14,
        &label_small,
        layer_text
    );


    /*
     * ------------------------------------------------------------
     * Rotation
     * ------------------------------------------------------------
     */

    rotate_canvas(
        canvas,
        widget->cbuf
    );
}


/* ==========================================================================
 * BATTERY
 * ========================================================================== */

static void set_battery_status(
    struct zmk_widget_status *widget,
    struct battery_status_state state
) {

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif

    widget->state.battery = state.level;

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
        set_battery_status(widget, state);
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
        .usb_present = zmk_usb_is_powered(),
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
 * OUTPUT / BLUETOOTH PROFILE
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
        set_output_status(widget, &state);
    }
}


static struct output_status_state output_status_get_state(
    const zmk_event_t *_eh
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
 * LAYER
 * ========================================================================== */

static void set_layer_status(
    struct zmk_widget_status *widget,
    struct layer_status_state state
) {

    widget->state.layer_index = state.index;
    widget->state.layer_label = state.label;

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
        set_layer_status(widget, state);
    }
}


static struct layer_status_state layer_status_get_state(
    const zmk_event_t *eh
) {

    uint8_t index =
        zmk_keymap_highest_layer_active();

    return (struct layer_status_state) {
        .index = index,
        .label = zmk_keymap_layer_name(index)
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
 * KEY EVENTS
 * ========================================================================== */

static void process_keypress_event(
    bool is_pressed
) {

    key_pressed = is_pressed;
    key_released = !is_pressed;

    last_key_event = k_uptime_get_32();


    if (is_pressed) {

        /*
         * Prevent overflow.
         */

        if (active_keys < UINT8_MAX) {
            active_keys++;
        }

        keys_active = true;

    } else {

        if (active_keys > 0) {
            active_keys--;
        }

        keys_active = (active_keys > 0);
    }


    /*
     * Modifier state may have changed.
     */

    if (!modifier_check_scheduled) {

        modifier_check_scheduled = true;

        k_work_schedule(
            &modifier_work,
            K_MSEC(MODIFIER_CHECK_INTERVAL)
        );
    }


    /*
     * Redraw immediately so Bongo responds immediately.
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
 * MODIFIER WORK
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

            widget->state.modifiers = mods;


            for (int i = 0; i < NUM_SYMBOLS; i++) {

                modifier_symbols[i]->is_active =
                    (mods & modifier_symbols[i]->modifier) != 0;
            }


            draw_status(widget);
        }
    }


    /*
     * If keys are still being held, check again.
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
 * MODIFIER STATE
 * ========================================================================== */

static uint8_t get_current_modifiers(void) {

    uint8_t mods =
        zmk_hid_get_explicit_mods();

#if IS_ENABLED(CONFIG_ZMK_WIDGET_MODIFIERS_DEBUG)

    LOG_INF(
        "Current mods: %02x",
        mods
    );

#endif

    return mods;
}


/* ==========================================================================
 * WPM
 *
 * WPM is NOT displayed.
 *
 * It is only used to control Bongo animation state.
 * ========================================================================== */

static void update_animation_from_wpm(
    uint8_t current_wpm
) {

    /*
     * Furious when WPM is above 30.
     */

    if (current_wpm > 30) {

        current_anim_state =
            ANIM_STATE_FRENZIED;

        leaving_furious = false;

        return;
    }


    /*
     * If we leave furious mode, let Bongo finish the
     * exhale transition.
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
 * WPM EVENT STATE
 * ========================================================================== */

static struct wpm_status_state wpm_status_get_state(
    const zmk_event_t *eh
) {

    static uint8_t wpm_history[10] = {0};
    static uint8_t current_wpm = 0;


    const struct zmk_wpm_state_changed *wpm_ev =
        as_zmk_wpm_state_changed(eh);

    const struct zmk_position_state_changed *pos_ev =
        as_zmk_position_state_changed(eh);


    bool is_key_event = false;
    bool key_is_pressed = false;


    /*
     * ------------------------------------------------------------
     * WPM event
     * ------------------------------------------------------------
     */

    if (wpm_ev != NULL) {

        current_wpm =
            wpm_ev->state;


        /*
         * Maintain the history for animation decisions
         * and future use.
         */

        for (int i = 0; i < 9; i++) {
            wpm_history[i] =
                wpm_history[i + 1];
        }

        wpm_history[9] =
            current_wpm;


        /*
         * Bongo reacts to WPM.
         */

        update_animation_from_wpm(
            current_wpm
        );
    }


    /*
     * ------------------------------------------------------------
     * Position event
     * ------------------------------------------------------------
     */

    if (pos_ev != NULL) {

        is_key_event = true;

        key_is_pressed =
            pos_ev->state > 0;
    }


    return (struct wpm_status_state) {

        .wpm = current_wpm,

        .wpm_history = {
            wpm_history[0],
            wpm_history[1],
            wpm_history[2],
            wpm_history[3],
            wpm_history[4],
            wpm_history[5],
            wpm_history[6],
            wpm_history[7],
            wpm_history[8],
            wpm_history[9]
        },

        .key_pressed =
            key_is_pressed,

        .is_key_event =
            is_key_event,

        .is_animation_update =
            false
    };
}


/* ==========================================================================
 * WPM UPDATE CALLBACK
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
         * Position event.
         *
         * This controls immediate Bongo movement.
         */

        if (state.is_key_event) {

            process_keypress_event(
                state.key_pressed
            );
        }


        /*
         * WPM event.
         *
         * The WPM value has already been used by
         * update_animation_from_wpm().
         *
         * We deliberately DO NOT draw a WPM graph.
         */

        if (!state.is_key_event) {

            draw_status(widget);
        }
    }
}


/* ==========================================================================
 * WPM LISTENER
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
 * IDLE / ANIMATION WORK
 * ========================================================================== */

static void animation_work_handler(
    struct k_work *work
) {

    ARG_UNUSED(work);

    uint32_t current_time =
        k_uptime_get_32();


    /*
     * ------------------------------------------------------------
     * Idle animation
     * ------------------------------------------------------------
     */

    uint32_t idle_interval =
        IDLE_ANIMATION_INTERVAL +
        breathing_interval_adjustment;


    /*
     * Prevent negative / very small intervals.
     */

    if (idle_interval < 100) {
        idle_interval = 100;
    }


    if (
        current_time - last_idle_update >=
        idle_interval
    ) {

        last_idle_update =
            current_time;


        /*
         * Only advance breathing while no key is held.
         */

        if (!keys_active) {

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


            /*
             * Redraw all widgets.
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
    }


    /*
     * ------------------------------------------------------------
     * Keep worker alive
     * ------------------------------------------------------------
     */

    k_work_schedule(
        &animation_work,
        K_MSEC(100)
    );
}


/* ==========================================================================
 * INITIALIZATION
 * ========================================================================== */

int zmk_widget_status_init(
    struct zmk_widget_status *widget,
    lv_obj_t *parent
) {

    /*
     * ------------------------------------------------------------
     * Widget
     * ------------------------------------------------------------
     */

    widget->obj =
        lv_obj_create(parent);

    /*
     * Left-half status display.
     *
     * Artwork is NOT created here.
     */

    lv_obj_set_size(
        widget->obj,
        68,
        68
    );


    /*
     * ------------------------------------------------------------
     * Single canvas
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
     * Add widget
     * ------------------------------------------------------------
     */

    sys_slist_append(
        &widgets,
        &widget->node
    );


    /*
     * ------------------------------------------------------------
     * Initialize listeners
     * ------------------------------------------------------------
     */

    widget_battery_status_init();
    widget_output_status_init();
    widget_layer_status_init();
    widget_wpm_status_init();


    /*
     * ------------------------------------------------------------
     * Animation workers
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
     * Initial modifier state
     * ------------------------------------------------------------
     */

    for (int i = 0; i < NUM_SYMBOLS; i++) {
        modifier_symbols[i]->is_active = false;
    }

    widget->state.modifiers = 0;


    /*
     * ------------------------------------------------------------
     * Initial animation state
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
     * Initial draw
     * ------------------------------------------------------------
     */

    draw_status(widget);


    /*
     * ------------------------------------------------------------
     * Start animation worker
     * ------------------------------------------------------------ */

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
