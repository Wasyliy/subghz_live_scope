/*
 * SubGHz Live Scope
 *
 * Live raw-pulse oscilloscope + RSSI strip for SubGHz, with automatic band
 * hopping across the hopper frequency list from setting_user. Stays parked
 * on a frequency while a signal is present, resumes hopping once it's gone.
 *
 * Receive only. This app never transmits.
 *
 * Controls:
 *   OK          - pause/resume hopping (park on current frequency)
 *   Left/Right  - step frequency manually (only while paused)
 *   Back        - exit
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <notification/notification_messages.h>

#include <lib/subghz/subghz_worker.h>
#include <lib/subghz/subghz_setting.h>
#include <lib/subghz/devices/devices.h>

#include "helpers/radio_device_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "SubGhzLiveScope"

#define SCREEN_W_PX  128
#define SCREEN_H_PX  64
#define HISTORY_LEN  SCREEN_W_PX

#define RSSI_FLOOR          -127.0f
#define RSSI_CEIL           0.0f
#define RSSI_STAY_THRESHOLD -90.0f

#define SCOPE_TICK_MS    60
#define SCOPE_HOLD_TICKS 16 /* ~1s of signal keeps us parked before resuming hop */

#define PRESET_NAME  "AM650"
#define SETTING_PATH "/ext/subghz/assets/setting_user"

#define RSSI_BAR_BASELINE_Y 27
#define RSSI_BAR_MAX_H      17
#define SCOPE_BASELINE_Y    63
#define SCOPE_MAX_H         33

typedef struct
{
    FuriString *frequency_str;
    uint8_t rssi_history[HISTORY_LEN];
    uint16_t activity[HISTORY_LEN];
    size_t ind;
    bool filled;
    float rssi_now;
    bool signal_now;
    bool paused;
} ScopeModel;

typedef struct
{
    View *view;
    ViewDispatcher *view_dispatcher;
    Gui *gui;
    NotificationApp *notifications;

    SubGhzWorker *worker;
    SubGhzSetting *setting;
    const SubGhzDevice *radio_device;
    FuriTimer *tick_timer;
    FuriMutex *data_mutex;

    size_t hopper_idx;
    uint8_t stay_ticks;
    bool paused;

    /* Pulse edges counted since the last tick; protected by data_mutex since
     * the worker's pair callback runs on its own thread. */
    uint32_t pulse_accum;
} ScopeApp;

/* ---------------------------------------------------------------------- */
/* Radio control                                                           */
/* ---------------------------------------------------------------------- */

static void scope_stop_rx(ScopeApp *app)
{
    if (subghz_worker_is_running(app->worker))
    {
        subghz_worker_stop(app->worker);
        subghz_devices_stop_async_rx(app->radio_device);
    }
    subghz_devices_idle(app->radio_device);
}

static void scope_start_rx(ScopeApp *app, uint32_t frequency)
{
    subghz_devices_idle(app->radio_device);
    subghz_devices_set_frequency(app->radio_device, frequency);
    subghz_devices_flush_rx(app->radio_device);
    subghz_devices_start_async_rx(app->radio_device, subghz_worker_rx_callback, app->worker);
    subghz_worker_start(app->worker);
}

static void scope_set_frequency_label(ScopeApp *app, uint32_t freq)
{
    with_view_model(
        app->view,
        ScopeModel * model,
        {
            furi_string_printf(
                model->frequency_str, "%lu.%03lu MHz", freq / 1000000, (freq / 1000) % 1000);
        },
        false);
}

static uint32_t scope_hopper_frequency(ScopeApp *app, size_t idx)
{
    size_t count = subghz_setting_get_hopper_frequency_count(app->setting);
    if (count == 0)
        return subghz_setting_get_default_frequency(app->setting);

    return subghz_setting_get_hopper_frequency(app->setting, idx % count);
}

static void scope_goto_hopper_index(ScopeApp *app, size_t idx)
{
    scope_stop_rx(app);
    app->hopper_idx = idx;
    uint32_t freq = scope_hopper_frequency(app, app->hopper_idx);
    scope_start_rx(app, freq);
    scope_set_frequency_label(app, freq);
    app->stay_ticks = 0;

    /* New frequency = new sweep, don't carry over the old column history. */
    with_view_model(
        app->view,
        ScopeModel * model,
        {
            memset(model->activity, 0, sizeof(model->activity));
            memset(model->rssi_history, 0, sizeof(model->rssi_history));
            model->ind = 0;
            model->filled = false;
        },
        false);
}

static bool scope_radio_init(ScopeApp *app)
{
    app->radio_device = radio_device_loader_set(NULL, SubGhzRadioDeviceTypeExternalCC1101);
    if (!app->radio_device)
        return false;

    app->setting = subghz_setting_alloc();
    subghz_setting_load(app->setting, SETTING_PATH);

    uint8_t *preset_data = subghz_setting_get_preset_data_by_name(app->setting, PRESET_NAME);

    subghz_devices_reset(app->radio_device);
    subghz_devices_idle(app->radio_device);
    subghz_devices_load_preset(app->radio_device, FuriHalSubGhzPresetCustom, preset_data);

    return true;
}

/* ---------------------------------------------------------------------- */
/* Worker callback (own thread) - just counts edges, no drawing here       */
/* ---------------------------------------------------------------------- */

static void scope_worker_pair_callback(void *context, bool level, uint32_t duration)
{
    UNUSED(level);
    UNUSED(duration);
    ScopeApp *app = context;

    furi_mutex_acquire(app->data_mutex, FuriWaitForever);
    if (app->pulse_accum < 0xFFFFFFFFu)
        app->pulse_accum++;
    furi_mutex_release(app->data_mutex);
}

/* ---------------------------------------------------------------------- */
/* Periodic tick: scroll the scope, poll RSSI, decide on hopping           */
/* ---------------------------------------------------------------------- */

static void scope_timer_callback(void *context)
{
    ScopeApp *app = context;

    furi_mutex_acquire(app->data_mutex, FuriWaitForever);
    uint32_t pulses = app->pulse_accum;
    app->pulse_accum = 0;
    furi_mutex_release(app->data_mutex);

    float rssi = subghz_devices_get_rssi(app->radio_device);
    bool above = rssi > RSSI_STAY_THRESHOLD;

    if (!app->paused)
    {
        if (above)
        {
            app->stay_ticks = SCOPE_HOLD_TICKS;
        }
        else if (app->stay_ticks > 0)
        {
            app->stay_ticks--;
        }
        else
        {
            size_t count = subghz_setting_get_hopper_frequency_count(app->setting);
            if (count > 0)
            {
                scope_goto_hopper_index(app, (app->hopper_idx + 1) % count);
                notification_message(app->notifications, &sequence_blink_cyan_10);
            }
        }
    }

    if (above)
        notification_message(app->notifications, &sequence_blink_green_10);

    uint8_t u_rssi;
    if (rssi < RSSI_FLOOR)
        u_rssi = 0;
    else if (rssi > RSSI_CEIL)
        u_rssi = RSSI_BAR_MAX_H;
    else
        u_rssi = (uint8_t)(((rssi - RSSI_FLOOR) / (RSSI_CEIL - RSSI_FLOOR)) * RSSI_BAR_MAX_H);

    with_view_model(
        app->view,
        ScopeModel * model,
        {
            size_t col = model->ind % HISTORY_LEN;
            model->activity[col] = (pulses > 65535) ? 65535 : (uint16_t)pulses;
            model->rssi_history[col] = u_rssi;

            model->ind++;
            if (model->ind >= HISTORY_LEN)
            {
                model->filled = true;
                model->ind = 0;
            }

            model->rssi_now = rssi;
            model->signal_now = above;
            model->paused = app->paused;
        },
        true);
}

/* ---------------------------------------------------------------------- */
/* Drawing                                                                  */
/* ---------------------------------------------------------------------- */

static void scope_draw_callback(Canvas *canvas, void *ctx)
{
    ScopeModel *model = ctx;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    canvas_draw_str(canvas, 0, 7, furi_string_get_cstr(model->frequency_str));

    char rssi_buf[12];
    snprintf(rssi_buf, sizeof(rssi_buf), "%d dBm", (int)model->rssi_now);
    canvas_draw_str(canvas, 70, 7, rssi_buf);

    const char *status = model->paused ? "HOLD" : (model->signal_now ? "SIG" : "scan");
    canvas_draw_str(canvas, 114, 7, status);

    canvas_draw_line(canvas, 0, RSSI_BAR_BASELINE_Y, SCREEN_W_PX - 1, RSSI_BAR_BASELINE_Y);
    canvas_draw_line(canvas, 0, SCOPE_BASELINE_Y, SCREEN_W_PX - 1, SCOPE_BASELINE_Y);

    size_t visible = model->filled ? HISTORY_LEN : model->ind;

    uint16_t act_max = 1;
    for (size_t i = 0; i < visible; i++)
    {
        size_t idx = model->filled ? (model->ind + i) % HISTORY_LEN : i;
        if (model->activity[idx] > act_max)
            act_max = model->activity[idx];
    }

    for (size_t i = 0; i < visible; i++)
    {
        size_t idx = model->filled ? (model->ind + i) % HISTORY_LEN : i;

        uint8_t rh = model->rssi_history[idx];
        if (rh > RSSI_BAR_MAX_H)
            rh = RSSI_BAR_MAX_H;
        canvas_draw_line(
            canvas, (int)i, RSSI_BAR_BASELINE_Y, (int)i, RSSI_BAR_BASELINE_Y - rh);

        uint32_t sh = ((uint32_t)model->activity[idx] * SCOPE_MAX_H) / act_max;
        if (sh > SCOPE_MAX_H)
            sh = SCOPE_MAX_H;
        canvas_draw_line(
            canvas, (int)i, SCOPE_BASELINE_Y, (int)i, SCOPE_BASELINE_Y - (int)sh);
    }
}

/* ---------------------------------------------------------------------- */
/* Input                                                                    */
/* ---------------------------------------------------------------------- */

static bool scope_input_callback(InputEvent *event, void *context)
{
    ScopeApp *app = context;

    if (event->type != InputTypeShort)
        return true;

    if (event->key == InputKeyBack)
    {
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }

    if (event->key == InputKeyOk)
    {
        app->paused = !app->paused;
        with_view_model(
            app->view, ScopeModel * model, { model->paused = app->paused; }, true);
        return true;
    }

    if (app->paused && (event->key == InputKeyLeft || event->key == InputKeyRight))
    {
        size_t count = subghz_setting_get_hopper_frequency_count(app->setting);
        if (count > 0)
        {
            size_t idx = (event->key == InputKeyRight)
                             ? (app->hopper_idx + 1) % count
                             : (app->hopper_idx == 0 ? count - 1 : app->hopper_idx - 1);
            scope_goto_hopper_index(app, idx);
        }
        return true;
    }

    return true;
}

/* ---------------------------------------------------------------------- */
/* Entry point                                                              */
/* ---------------------------------------------------------------------- */

int32_t subghz_live_scope_app(void *p)
{
    UNUSED(p);

    ScopeApp *app = malloc(sizeof(ScopeApp));
    if (!app)
        return 0;
    memset(app, 0, sizeof(ScopeApp));

    app->data_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    subghz_devices_init();

    if (!scope_radio_init(app))
    {
        FURI_LOG_E(TAG, "No SubGHz radio device available");
        subghz_devices_deinit();
        furi_record_close(RECORD_NOTIFICATION);
        furi_record_close(RECORD_GUI);
        furi_mutex_free(app->data_mutex);
        free(app);
        return 0;
    }

    app->worker = subghz_worker_alloc();
    subghz_worker_set_context(app->worker, app);
    subghz_worker_set_pair_callback(app->worker, scope_worker_pair_callback);

    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(ScopeModel));
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, scope_draw_callback);
    view_set_input_callback(app->view, scope_input_callback);

    with_view_model(
        app->view, ScopeModel * model, { model->frequency_str = furi_string_alloc(); }, false);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_add_view(app->view_dispatcher, 0, app->view);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    app->hopper_idx = 0;
    uint32_t start_freq = scope_hopper_frequency(app, app->hopper_idx);
    scope_start_rx(app, start_freq);
    scope_set_frequency_label(app, start_freq);

    app->tick_timer = furi_timer_alloc(scope_timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(app->tick_timer, furi_ms_to_ticks(SCOPE_TICK_MS));

    view_dispatcher_run(app->view_dispatcher);

    furi_timer_stop(app->tick_timer);
    furi_timer_free(app->tick_timer);

    scope_stop_rx(app);
    radio_device_loader_end(app->radio_device);
    subghz_worker_free(app->worker);

    view_dispatcher_remove_view(app->view_dispatcher, 0);
    with_view_model(
        app->view, ScopeModel * model, { furi_string_free(model->frequency_str); }, false);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);

    subghz_setting_free(app->setting);
    subghz_devices_deinit();

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    furi_mutex_free(app->data_mutex);
    free(app);
    return 0;
}
