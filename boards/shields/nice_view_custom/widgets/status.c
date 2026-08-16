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

#include <zmk/battery.h>
#include <zmk/display.h>
#include "status.h"

#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>

#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/wpm.h>
#include <zmk/hid.h>
#include <zmk/keys.h>

#include <dt-bindings/zmk/modifiers.h>

#include "bongocatart.h"

/* --------------------------------------------------------------------------
 * Modifier declaration
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
 * BongoCat images
 * -------------------------------------------------------------------------- */

LV_IMG_DECLARE(bongocatrest0);
LV_IMG_DECLARE(bongocatcasual1);
LV_IMG_DECLARE(bongocatcasual2);
LV_IMG_DECLARE(bongocatfast1);
LV_IMG_DECLARE(bongocatfast2);

LV_IMG_DECLARE(bongo_resting);
LV_IMG_DECLARE(bongo_casualright);
LV_IMG_DECLARE(bongo_casualleft);
LV_IMG_DECLARE(bongo_furiousup);
LV_IMG_DECLARE(bongo_furiousdown);
LV_IMG_DECLARE(bongo_inhale);
LV_IMG_DECLARE(bongo_exhale);

/* --------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------- */

static uint8_t get_current_modifiers(void);

/* --------------------------------------------------------------------------
 * Widget list
 * -------------------------------------------------------------------------- */

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/* --------------------------------------------------------------------------
 * State structures
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

struct wpm_status_state {
    uint8_t wpm;

    uint8_t wpm_history[10];

    uint8_t animation_state;

    bool key_pressed;
    bool is_key_event;
    bool is_animation_update;
};

/* --------------------------------------------------------------------------
 * BongoCat animation state
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

static uint32_t last_idle_update = 0;

static const uint32_t IDLE_ANIMATION_INTERVAL = 750;

static int32_t breathing_interval_adjustment = 0;

static bool leaving_furious = false;

/* --------------------------------------------------------------------------
 * Random number state
 * -------------------------------------------------------------------------- */

static uint32_t random_seed = 7919;

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
 * Key / animation state
 * -------------------------------------------------------------------------- */

static bool key_pressed = false;
static bool key_released = false;
static bool keys_active = false;

static uint8_t active_keys = 0;

static bool use_first_frame = true;

static const lv_img_dsc_t *last_active_frame = &bongo_resting;

static uint32_t last_key_event = 0;

static const uint32_t KEY_DEBOUNCE_INTERVAL = 20;

static bool debounce_check_scheduled = false;

static uint32_t last_keypress_time = 0;

static const uint32_t WPM_PAUSE_TIMEOUT = 10000;

static uint32_t last_wpm_update = 0;

static const uint32_t WPM_UPDATE_INTERVAL = 1000;

/* --------------------------------------------------------------------------
 * Workers
 * -------------------------------------------------------------------------- */

static struct k_work_delayable animation_work;
static struct k_work_delayable modifier_work;

/* --------------------------------------------------------------------------
 * TOP
 *
 * Physical display:
 *
 *     AKKU       WIFI
 *
 * -------------------------------------------------------------------------- */

static void draw_top(lv_obj_t *widget,
                     lv_color_t cbuf[],
                     const struct status_state *state) {

    /*
     * Canvas 0 = top section.
     *
     * The actual parent is 160x68 because the display is physically
     * mounted portrait. The canvas itself is 68x68 and is rotated
     * using rotate_canvas().
     */

    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(
        &label_dsc,
        LVGL_FOREGROUND,
        &lv_font_montserrat_14,
        LV_TEXT_ALIGN_CENTER
    );

    /* Clear canvas */

    lv_canvas_draw_rect(
        canvas,
        0,
        0,
        CANVAS_SIZE,
        CANVAS_SIZE,
        &rect_black_dsc
    );

    /* ------------------------------------------------------------------
     * Battery
     *
     * draw_battery() uses the normal battery graphic from util.c.
     *
     * x/y are chosen so that after the 270 degree canvas rotation
     * the battery appears in the upper physical section.
     * ------------------------------------------------------------------ */

    draw_battery(canvas, state);

    /* ------------------------------------------------------------------
     * Connection / WiFi
     * ------------------------------------------------------------------ */

    char output_text[8] = {};

    switch (state->selected_endpoint.transport) {

    case ZMK_TRANSPORT_USB:
        strcpy(output_text, LV_SYMBOL_USB);
        break;

    case ZMK_TRANSPORT_BLE:

        if (state->active_profile_bonded) {

            if (state->active_profile_connected) {
                strcpy(output_text, LV_SYMBOL_WIFI);
            } else {
                strcpy(output_text, LV_SYMBOL_CLOSE);
            }

        } else {
            strcpy(output_text, LV_SYMBOL_SETTINGS);
        }

        break;

    default:
        output_text[0] = '\0';
        break;
    }

    /*
     * Battery occupies the left side of this physical row.
     *
     * Because the canvas is rotated, changing the local Y coordinate
     * moves the element horizontally on the physical display.
     */

    lv_canvas_draw_text(
        canvas,
        2,
        38,
        28,
        &label_dsc,
        output_text
    );

    /* Rotate the complete top canvas */

    rotate_canvas(canvas, cbuf);
}

/* --------------------------------------------------------------------------
 * MIDDLE
 *
 * Physical display:
 *
 *             BONGOCAT
 *
 *             CTRL ALT WIN SHIFT
 *
 * -------------------------------------------------------------------------- */

static void draw_middle(lv_obj_t *widget,
                        lv_color_t cbuf[],
                        const struct status_state *state) {

    lv_obj_t *canvas = lv_obj_get_child(widget, 1);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);

    /* Clear canvas */

    lv_canvas_draw_rect(
        canvas,
        0,
        0,
        CANVAS_SIZE,
        CANVAS_SIZE,
        &rect_black_dsc
    );

    /* ------------------------------------------------------------------
     * Modifier row
     *
     * draw_modifiers() is deliberately kept below the cat.
     * ------------------------------------------------------------------ */

    draw_modifiers(canvas, 0, 62);

    /* ------------------------------------------------------------------
     * BongoCat
     * ------------------------------------------------------------------ */

    const lv_img_dsc_t *current_frame = &bongo_resting;

    if (current_anim_state == ANIM_STATE_CASUAL) {

        if (key_pressed) {

            if (use_first_frame) {
                last_active_frame = &bongo_casualright;
            } else {
                last_active_frame = &bongo_casualleft;
            }

            use_first_frame = !use_first_frame;

            current_frame = last_active_frame;

        } else if (keys_active) {

            current_frame = last_active_frame;

        } else if (
            k_uptime_get_32() - last_key_event <= KEY_DEBOUNCE_INTERVAL
        ) {

            current_frame = last_active_frame;

        } else {

            switch (current_idle_state) {

            case IDLE_INHALE:
                current_frame = &bongo_inhale;
                break;

            case IDLE_REST1:
                current_frame = &bongo_resting;
                break;

            case IDLE_EXHALE:
                current_frame = &bongo_exhale;
                break;

            case IDLE_REST2:
                current_frame = &bongo_resting;
                break;

            default:
                current_frame = &bongo_resting;
                break;
            }
        }

    } else {

        /* --------------------------------------------------------------
         * Frenzied animation
         * -------------------------------------------------------------- */

        if (key_pressed || key_released) {

            if (use_first_frame) {
                last_active_frame = &bongo_furiousup;
            } else {
                last_active_frame = &bongo_furiousdown;
            }

            use_first_frame = !use_first_frame;

            current_frame = last_active_frame;

        } else if (keys_active) {

            current_frame = last_active_frame;

        } else if (
            k_uptime_get_32() - last_key_event <= KEY_DEBOUNCE_INTERVAL
        ) {

            current_frame = last_active_frame;

        } else {

            switch (current_idle_state) {

            case IDLE_INHALE:
                current_frame = &bongo_inhale;
                break;

            case IDLE_REST1:
                current_frame = &bongo_resting;
                break;

            case IDLE_EXHALE:
                current_frame = &bongo_exhale;
                break;

            case IDLE_REST2:
                current_frame = &bongo_resting;
                break;

            default:
                current_frame = &bongo_resting;
                break;
            }
        }
    }

    /* The release event has now been consumed */

    key_released = false;

    /* ------------------------------------------------------------------
     * Draw BongoCat
     * ------------------------------------------------------------------ */

    lv_canvas_draw_img(
        canvas,
        0,
        0,
        current_frame,
        &img_dsc
    );

    /* Rotate middle canvas */

    rotate_canvas(canvas, cbuf);
}

/* --------------------------------------------------------------------------
 * BOTTOM
 *
 * Physical display:
 *
 *     Bluetooth Profile
 *
 *     Layer
 *
 * -------------------------------------------------------------------------- */

static void draw_bottom(lv_obj_t *widget,
                        lv_color_t cbuf[],
                        const struct status_state *state) {

    lv_obj_t *canvas = lv_obj_get_child(widget, 2);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    lv_draw_arc_dsc_t arc_dsc;
    init_arc_dsc(&arc_dsc, LVGL_FOREGROUND, 2);

    lv_draw_arc_dsc_t arc_dsc_filled;
    init_arc_dsc(&arc_dsc_filled, LVGL_FOREGROUND, 7);

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(
        &label_dsc,
        LVGL_FOREGROUND,
        &lv_font_montserrat_14,
        LV_TEXT_ALIGN_CENTER
    );

    lv_draw_label_dsc_t label_dsc_black;
    init_label_dsc(
        &label_dsc_black,
        LVGL_BACKGROUND,
        &lv_font_montserrat_14,
        LV_TEXT_ALIGN_CENTER
    );

    /* Clear canvas */

    lv_canvas_draw_rect(
        canvas,
        0,
        0,
        CANVAS_SIZE,
        CANVAS_SIZE,
        &rect_black_dsc
    );

    /* ------------------------------------------------------------------
     * Bluetooth profile
     *
     * Show all five profiles.
     *
     * Connected  = solid outer ring
     * Selected   = filled center
     * Unconnected/bonded = outer ring
     * Open       = no ring
     * ------------------------------------------------------------------ */

    const int profile_y = 13;

    for (int i = 0; i < 5; i++) {

        int profile_x = 6 + (i * 14);

        bool selected =
            i == state->active_profile_index;

        bool bonded =
            state->active_profile_bonded;

        bool connected =
            state->active_profile_connected;

        if (bonded) {

            lv_canvas_draw_arc(
                canvas,
                profile_x,
                profile_y,
                6,
                0,
                359,
                &arc_dsc
            );

            if (selected) {

                lv_canvas_draw_arc(
                    canvas,
                    profile_x,
                    profile_y,
                    4,
                    0,
                    359,
                    &arc_dsc_filled
                );
            }
        }

        char profile_text[2];

        snprintf(
            profile_text,
            sizeof(profile_text),
            "%d",
            i + 1
        );

        /*
         * Selected profile gets inverted number when filled.
         */

        lv_draw_label_dsc_t *profile_label =
            selected && bonded
                ? &label_dsc_black
                : &label_dsc;

        lv_canvas_draw_text(
            canvas,
            profile_x - 5,
            profile_y - 7,
            10,
            profile_label,
            profile_text
        );

        /*
         * Silence compiler warnings for the state variables when the
         * profile is not the currently selected profile.
         */

        (void)connected;
    }

    /* ------------------------------------------------------------------
     * Layer
     * ------------------------------------------------------------------ */

    if (state->layer_label == NULL) {

        char layer_text[16];

        snprintf(
            layer_text,
            sizeof(layer_text),
            "LAYER %d",
            state->layer_index
        );

        lv_canvas_draw_text(
            canvas,
            0,
            38,
            CANVAS_SIZE,
            &label_dsc,
            layer_text
        );

    } else {

        lv_canvas_draw_text(
            canvas,
            0,
            38,
            CANVAS_SIZE,
            &label_dsc,
            state->layer_label
        );
    }

    /* Rotate bottom canvas */

    rotate_canvas(canvas, cbuf);
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
)

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
 * OUTPUT / BLUETOOTH
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
)

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

    widget->state.layer_index =
        state.index;

    widget->state.layer_label =
        state.label;

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
)

ZMK_SUBSCRIPTION(
    widget_layer_status,
    zmk_layer_state_changed
);

/* ==========================================================================
 * MODIFIERS
 * ========================================================================== */

static uint8_t get_current_modifiers(void) {

    uint8_t mods =
        zmk_hid_get_explicit_mods();

#if IS_ENABLED(CONFIG_ZMK_WIDGET_MODIFIERS_DEBUG)
    LOG_INF("Current mods: %02x", mods);
#endif

    return mods;
}

static void modifier_work_handler(
    struct k_work *work
) {

    ARG_UNUSED(work);

    uint32_t current_time =
        k_uptime_get_32();

    if (debounce_check_scheduled) {

        debounce_check_scheduled = false;

        if (
            !keys_active &&
            (
                current_time - last_key_event >=
                KEY_DEBOUNCE_INTERVAL
            )
        ) {

            struct zmk_widget_status *widget;

            SYS_SLIST_FOR_EACH_CONTAINER(
                &widgets,
                widget,
                node
            ) {

                last_active_frame =
                    &bongo_resting;

                current_idle_state =
                    IDLE_REST2;

                last_idle_update =
                    current_time -
                    IDLE_ANIMATION_INTERVAL;

                draw_middle(
                    widget->obj,
                    widget->cbuf2,
                    &widget->state
                );
            }

        } else if (keys_active) {

            k_work_schedule(
                &modifier_work,
                K_MSEC(KEY_DEBOUNCE_INTERVAL)
            );

            debounce_check_scheduled = true;
        }
    }

    /* --------------------------------------------------------------
     * Modifier state
     * -------------------------------------------------------------- */

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
                    (
                        mods &
                        modifier_symbols[i]->modifier
                    ) != 0;
            }

            draw_middle(
                widget->obj,
                widget->cbuf2,
                &widget->state
            );
        }
    }
}

/* ==========================================================================
 * KEY EVENTS
 * ========================================================================== */

static void process_keypress_event(
    bool is_pressed,
    struct zmk_widget_status *widget
) {

    key_pressed = is_pressed;
    key_released = !is_pressed;

    if (is_pressed) {

        active_keys++;

        keys_active = true;

        last_key_event =
            k_uptime_get_32();

    } else {

        if (active_keys > 0) {
            active_keys--;
        }

        keys_active =
            active_keys > 0;

        last_key_event =
            k_uptime_get_32();
    }

    k_work_schedule(
        &modifier_work,
        K_MSEC(KEY_DEBOUNCE_INTERVAL)
    );

    debounce_check_scheduled = true;

    draw_middle(
        widget->obj,
        widget->cbuf2,
        &widget->state
    );
}

/* ==========================================================================
 * WPM / KEY EVENT LISTENER
 *
 * WPM is no longer displayed.
 * It is still used to control BongoCat's animation speed/state.
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

        uint32_t current_time =
            k_uptime_get_32();

        if (state.is_key_event) {

            last_keypress_time =
                current_time;

            process_keypress_event(
                state.key_pressed,
                widget
            );
        }

        /*
         * Use recent WPM to switch between casual and furious
         * BongoCat animation.
         */

        int recent_wpm = 0;

        for (int i = 5; i < 10; i++) {
            recent_wpm +=
                widget->state.wpm[i];
        }

        recent_wpm /= 5;

        if (recent_wpm > 30) {

            current_anim_state =
                ANIM_STATE_FRENZIED;

            leaving_furious = false;

        } else if (
            current_anim_state ==
            ANIM_STATE_FRENZIED
        ) {

            current_anim_state =
                ANIM_STATE_CASUAL;

            leaving_furious = true;

            current_idle_state =
                IDLE_EXHALE;

            last_idle_update =
                current_time;
        }

        /*
         * Keep WPM history internally for animation logic.
         */

        if (state.wpm > 0) {

            for (int i = 0; i < 9; i++) {
                widget->state.wpm[i] =
                    widget->state.wpm[i + 1];
            }

            widget->state.wpm[9] =
                state.wpm;

            last_wpm_update =
                current_time;
        }
    }
}

struct wpm_status_state wpm_status_get_state(
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

    if (wpm_ev != NULL) {

        current_wpm =
            wpm_ev->state;

        for (int i = 0; i < 9; i++) {
            wpm_history[i] =
                wpm_history[i + 1];
        }

        wpm_history[9] =
            current_wpm;
    }

    if (pos_ev != NULL) {

        is_key_event = true;

        key_is_pressed =
            pos_ev->state > 0;
    }

    return (struct wpm_status_state) {

        .wpm =
            current_wpm,

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

        .animation_state =
            current_anim_state,

        .key_pressed =
            key_is_pressed,

        .is_key_event =
            is_key_event,

        .is_animation_update =
            false
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(
    widget_wpm_status,
    struct wpm_status_state,
    wpm_status_update_cb,
    wpm_status_get_state
)

ZMK_SUBSCRIPTION(
    widget_wpm_status,
    zmk_wpm_state_changed
);

ZMK_SUBSCRIPTION(
    widget_wpm_status,
    zmk_position_state_changed
);

/* ==========================================================================
 * BONGO ANIMATION WORKER
 * ========================================================================== */

static void animation_work_handler(
    struct k_work *work
) {

    ARG_UNUSED(work);

    uint32_t current_time =
        k_uptime_get_32();

    bool needs_redraw = false;

    /* --------------------------------------------------------------
     * Idle animation
     * -------------------------------------------------------------- */

    if (
        current_time - last_idle_update >
        IDLE_ANIMATION_INTERVAL
    ) {

        last_idle_update =
            current_time;

        /*
         * If furious mode has been inactive for a while,
         * return to casual mode.
         */

        if (
            current_anim_state ==
                ANIM_STATE_FRENZIED &&
            current_time - last_keypress_time >
                WPM_UPDATE_INTERVAL * 2
        ) {

            current_anim_state =
                ANIM_STATE_CASUAL;

            current_idle_state =
                IDLE_INHALE;

            breathing_interval_adjustment =
                get_random_adjustment();

            needs_redraw = true;
        }

        /*
         * Do not run idle breathing while keys are being held.
         */

        if (!keys_active) {

            switch (current_idle_state) {

            case IDLE_INHALE:

                current_idle_state =
                    IDLE_REST1;

                needs_redraw = true;

                break;

            case IDLE_REST1:

                current_idle_state =
                    IDLE_EXHALE;

                needs_redraw = true;

                break;

            case IDLE_EXHALE:

                current_idle_state =
                    IDLE_REST2;

                if (leaving_furious) {
                    leaving_furious = false;
                }

                needs_redraw = true;

                break;

            case IDLE_REST2:

                current_idle_state =
                    IDLE_INHALE;

                if (!leaving_furious) {

                    breathing_interval_adjustment =
                        get_random_adjustment();
                }

                needs_redraw = true;

                break;
            }
        }
    }

    /* --------------------------------------------------------------
     * Redraw BongoCat if animation changed.
     * -------------------------------------------------------------- */

    if (needs_redraw) {

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
     * Check animation frequently enough for smooth key response.
     */

    uint32_t next_check =
        MIN(
            WPM_UPDATE_INTERVAL / 4,
            IDLE_ANIMATION_INTERVAL / 2
        );

    k_work_schedule(
        &animation_work,
        K_MSEC(next_check)
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
     * Parent is deliberately 160x68.
     *
     * The three 68x68 canvases are arranged horizontally:
     *
     *     [ BOTTOM ][ MIDDLE ][ TOP ]
     *
     * After rotate_canvas() this becomes physically:
     *
     *     [ TOP ]
     *     [ MIDDLE ]
     *     [ BOTTOM ]
     *
     * on the portrait-mounted 68x160 display.
     */

    widget->obj =
        lv_obj_create(parent);

    lv_obj_set_size(
        widget->obj,
        160,
        68
    );

    /* ------------------------------------------------------------------
     * TOP CANVAS
     * ------------------------------------------------------------------ */

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

    /* ------------------------------------------------------------------
     * MIDDLE CANVAS
     * ------------------------------------------------------------------ */

    lv_obj_t *middle =
        lv_canvas_create(widget->obj);

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

    /* ------------------------------------------------------------------
     * BOTTOM CANVAS
     * ------------------------------------------------------------------ */

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

    /* ------------------------------------------------------------------
     * Register widget
     * ------------------------------------------------------------------ */

    sys_slist_append(
        &widgets,
        &widget->node
    );

    /* ------------------------------------------------------------------
     * Initialize listeners
     * ------------------------------------------------------------------ */

    widget_battery_status_init();
    widget_output_status_init();
    widget_layer_status_init();
    widget_wpm_status_init();

    /* ------------------------------------------------------------------
     * Initialize workers
     * ------------------------------------------------------------------ */

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
        K_MSEC(IDLE_ANIMATION_INTERVAL)
    );

    /* ------------------------------------------------------------------
     * Initial modifier state
     * ------------------------------------------------------------------ */

    for (int i = 0; i < NUM_SYMBOLS; i++) {

        modifier_symbols[i]->is_active =
            false;
    }

    widget->state.modifiers = 0;

    /* ------------------------------------------------------------------
     * Initial draw
     * ------------------------------------------------------------------ */

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
 * OBJECT ACCESS
 * ========================================================================== */

lv_obj_t *zmk_widget_status_obj(
    struct zmk_widget_status *widget
) {

    return widget->obj;
}
