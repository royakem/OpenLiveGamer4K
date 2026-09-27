/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * openlivegamer4k-control: native GTK4 control application for the
 * OpenLiveGamer4K capture driver.
 *
 * Read-only status by default (safe with OBS attached). Preview is an
 * explicit user action that takes the exclusive V4L2 node; stopping
 * preview releases the device immediately.
 *
 * --backend auto (default): probe /dev/videoN for the GC573 card.
 * --backend mock: deterministic in-process device (offline dev/CI).
 */
#define GDK_DISABLE_DEPRECATION_WARNINGS
#define GTK_DISABLE_DEPRECATION_WARNINGS
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>

#include "olg4k/backend.h"
#include "olg4k/worker.h"

extern const Olg4kBackend olg4k_backend_real;
extern const Olg4kBackend olg4k_backend_mock;

struct App {
    GtkApplication *app;
    Olg4kDevice *device;
    const Olg4kBackend *backend;
    char devpath[64];
    struct Olg4kWorker *worker;
    bool previewing;

    GtkWindow *window;
    GtkLabel *status_driver;
    GtkLabel *status_input;
    GtkLabel *status_format;
    GtkLabel *status_size;
    GtkLabel *status_rate;
    GtkLabel *status_mode;
    GtkPicture *preview_image;
    GtkButton *btn_preview;
    GtkComboBoxText *cmb_format;
    GtkComboBoxText *cmb_size;
    GtkComboBoxText *cmb_rate;
    GtkLabel *lbl_error;
    GdkTexture *frame_tex;
    unsigned long long last_frames;
    guint fps_timer_id;
    int fps;
    GMutex frame_lock;
    guint8 *pending_rgb;
    guint pending_width, pending_height;
    gboolean frame_source_pending;
    guint frame_source_id;
    gboolean closing;
    GtkLabel *audio_state;
    GtkButton *audio_enable, *audio_test;
    GtkCheckButton *audio_consent;
    GtkLabel *audio_result;
    GSubprocess *audio_process;
    gboolean testing;
    guint audio_status_timer;
    gboolean mock_backend;
    GSubprocess *report_process;
    GtkButton *report_button;
};
static int discover_device(struct App *a, const char *which);
static void update_status_labels(struct App *a);
static void set_preview_ui(struct App *a, bool active);

/* ---- frame callback (worker thread) -> GLib main loop ---- */

static gboolean frame_job(gpointer data)
{
    struct App *a = data;
    guint8 *rgb;
    guint width, height;
    gboolean closing;
    GdkTexture *tex;
    g_mutex_lock(&a->frame_lock);
    rgb = a->pending_rgb; width = a->pending_width; height = a->pending_height;
    a->pending_rgb = NULL; a->frame_source_pending = FALSE; a->frame_source_id = 0;
    closing = a->closing;
    g_mutex_unlock(&a->frame_lock);
    if (!rgb || closing) { g_free(rgb); return G_SOURCE_REMOVE; }
    GBytes *bytes = g_bytes_new_take(rgb, (gsize)width * height * 3);
    tex = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8,
                                 bytes, (gsize)width * 3);
    g_bytes_unref(bytes);
    if (a->frame_tex)
        g_object_unref(a->frame_tex);
    a->frame_tex = tex;
    gtk_picture_set_paintable(a->preview_image, GDK_PAINTABLE(tex));
    return G_SOURCE_REMOVE;
}

static void on_frame(struct Olg4kWorker *worker, const uint8_t *rgb24,
                     unsigned int width, unsigned int height, void *user_data)
{
    struct App *a = user_data;
    GSource *source = NULL;
    (void)worker;
    g_mutex_lock(&a->frame_lock);
    if (!a->closing) {
        g_free(a->pending_rgb);
        a->pending_rgb = g_memdup2(rgb24, (gsize)width * height * 3);
        a->pending_width = width; a->pending_height = height;
        if (!a->frame_source_pending) {
            a->frame_source_pending = TRUE;
            source = g_idle_source_new();
            g_source_set_callback(source, frame_job, a, NULL);
            a->frame_source_id = g_source_attach(source, NULL);
        }
    }
    g_mutex_unlock(&a->frame_lock);
    if (source) g_source_unref(source);
}

typedef enum { AUDIO_UNSUPPORTED, AUDIO_OFF, AUDIO_ON } AudioState;
static AudioState audio_kernel_state(void)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    struct dirent *e;
    AudioState state = AUDIO_UNSUPPORTED;
    if (!d) return state;
    while ((e = readdir(d))) {
        char path[512], value[32]; FILE *f;
        if (e->d_name[0] == '.') continue;
        g_snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", e->d_name);
        f = fopen(path, "r"); if (!f) continue;
        if (!fgets(value, sizeof(value), f)) { fclose(f); continue; }
        fclose(f); if (g_ascii_strcasecmp(g_strstrip(value), "0x1461")) continue;
        g_snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", e->d_name);
        f = fopen(path, "r"); if (!f) continue;
        if (!fgets(value, sizeof(value), f)) { fclose(f); continue; }
        fclose(f); if (g_ascii_strcasecmp(g_strstrip(value), "0x0054")) continue;
        g_snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver/module", e->d_name);
        char *target = realpath(path, NULL);
        if (!target || !g_str_has_suffix(target, "/gc573_pure")) { free(target); break; }
        free(target);
        g_snprintf(path, sizeof(path), "/sys/module/gc573_pure/parameters/audio_experimental");
        f = fopen(path, "r");
        if (!f) break;
        if (fgets(value, sizeof(value), f)) {
            g_strstrip(value);
            if (!strcmp(value, "Y") || !strcmp(value, "1")) state = AUDIO_ON;
            else if (!strcmp(value, "N") || !strcmp(value, "0")) state = AUDIO_OFF;
        }
        fclose(f);
        break;
    }
    closedir(d); return state;
}
static gboolean audio_card_found(void)
{
    DIR *d = opendir("/sys/bus/pci/devices"); struct dirent *e;
    gboolean found = FALSE;
    if (!d) return FALSE;
    while ((e = readdir(d))) {
        char path[512], value[32]; FILE *f;
        if (e->d_name[0] == '.') continue;
        g_snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", e->d_name);
        f = fopen(path, "r"); if (!f) continue;
        if (!fgets(value, sizeof(value), f)) { fclose(f); continue; }
        fclose(f); if (g_ascii_strcasecmp(g_strstrip(value), "0x1461")) continue;
        g_snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", e->d_name);
        f = fopen(path, "r"); if (!f) continue;
        if (fgets(value, sizeof(value), f) && !g_ascii_strcasecmp(g_strstrip(value), "0x0054")) found = TRUE;
        fclose(f); if (found) break;
    }
    closedir(d); return found;
}

static void refresh_audio_state(struct App *a)
{
    if (a->mock_backend) {
        gtk_label_set_text(a->audio_state, "HDMI audio: unavailable in mock mode");
        gtk_button_set_label(a->audio_enable, "Enable HDMI audio");
        gtk_widget_set_sensitive(GTK_WIDGET(a->audio_enable), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(a->audio_test), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(a->report_button), FALSE);
        return;
    }
    AudioState state = audio_kernel_state();
    gtk_label_set_text(a->audio_state, state == AUDIO_ON ? "HDMI audio: on" :
        state == AUDIO_OFF ? "HDMI audio: off" :
        audio_card_found() ? "HDMI audio: unavailable (parameter missing or card not bound)" :
        "HDMI audio: unavailable (GC573 1461:0054 not found)");
    gtk_button_set_label(a->audio_enable, state == AUDIO_ON ? "Disable HDMI audio" : "Enable HDMI audio");
    gtk_widget_set_sensitive(GTK_WIDGET(a->audio_enable), !a->audio_process && !a->report_process && state != AUDIO_UNSUPPORTED &&
        (state == AUDIO_ON || gtk_check_button_get_active(a->audio_consent)));
    gtk_widget_set_sensitive(GTK_WIDGET(a->audio_test), !a->audio_process && !a->report_process && state == AUDIO_ON);
    gtk_widget_set_sensitive(GTK_WIDGET(a->report_button), !a->report_process && !a->audio_process);
}

static void audio_set_controls(struct App *a, gboolean busy)
{
    if (busy) {
        gtk_widget_set_sensitive(GTK_WIDGET(a->audio_enable), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(a->audio_test), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(a->report_button), FALSE);
    } else refresh_audio_state(a);
    gtk_widget_set_sensitive(GTK_WIDGET(a->btn_preview), !busy && a->device != NULL);
}

static void stop_preview(struct App *a)
{
    if (!a->previewing) return;
    if (a->fps_timer_id) { g_source_remove(a->fps_timer_id); a->fps_timer_id = 0; }
    if (a->worker) { olg4k_worker_stop(a->worker); olg4k_worker_free(a->worker); a->worker = NULL; }
    if (a->device) olg4k_set_mode(a->device, OLG4K_MODE_STATUS);
    gtk_widget_set_visible(GTK_WIDGET(a->preview_image), FALSE);
    if (a->frame_tex) { g_object_unref(a->frame_tex); a->frame_tex = NULL; }
    set_preview_ui(a, FALSE);
}

static void audio_done(GObject *source, GAsyncResult *res, gpointer data)
{
    struct App *a = data; GError *err = NULL; gchar *out = NULL, *stderr_text = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res,
                                                        &out, &stderr_text, &err);
    gboolean test = a->testing;
    g_clear_object(&a->audio_process); a->testing = FALSE;
    if (a->closing) { g_clear_error(&err); g_free(out); g_free(stderr_text); return; }
    if (!ok) gtk_label_set_text(a->audio_result, err ? err->message : "Audio command failed.");
    else {
        JsonParser *parser = json_parser_new();
        GError *json_error = NULL;
        if (out && json_parser_load_from_data(parser, out, -1, &json_error) &&
            JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
            JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
            const char *result = json_object_has_member(obj, "result") ? json_object_get_string_member(obj, "result") : NULL;
            const char *detail = json_object_has_member(obj, "error") ? json_object_get_string_member(obj, "error") : NULL;
            char *message = NULL;
            if (test) {
                if (g_strcmp0(result, "silent") == 0) message = g_strdup("Captured silence: all samples were zero.");
                else if (g_strcmp0(result, "signal") == 0) message = g_strdup("Nonzero audio activity detected. Fidelity and A/V sync are unverified.");
                else if (g_strcmp0(result, "busy") == 0) message = g_strdup_printf("Inconclusive: capture device is busy%s%s", detail ? " — " : ".", detail ? detail : "");
                else if (g_strcmp0(result, "unsupported") == 0) message = g_strdup_printf("Test unavailable%s%s", detail ? ": " : ".", detail ? detail : "");
                else message = g_strdup_printf("Inconclusive test%s%s", detail ? ": " : ".", detail ? detail : "");
                if (json_object_has_member(obj, "frames") && json_object_has_member(obj, "peak")) {
                    char *with_metrics = g_strdup_printf("%s Frames: %" G_GINT64_FORMAT ", peak: %" G_GINT64_FORMAT ".", message, (gint64)json_object_get_int_member(obj, "frames"), (gint64)json_object_get_int_member(obj, "peak"));
                    g_free(message); message = with_metrics;
                }
            } else if (json_object_has_member(obj, "enabled")) {
                message = g_strdup(json_object_get_boolean_member(obj, "enabled") ? "HDMI audio enabled." : "HDMI audio disabled.");
            } else message = g_strdup(detail ? detail : "Audio helper returned an unexpected result.");
            gtk_label_set_text(a->audio_result, message); g_free(message);
        } else if (!g_subprocess_get_successful(G_SUBPROCESS(source))) {
            const char *message = stderr_text && *stderr_text ? stderr_text : "Audio command failed (nonzero exit status).";
            gtk_label_set_text(a->audio_result, message);
        } else gtk_label_set_text(a->audio_result, "Audio command returned invalid JSON; result is inconclusive.");
        g_clear_error(&json_error); g_object_unref(parser);
    }
    g_clear_error(&err); g_free(out); g_free(stderr_text);
    if (!test && a->device) { olg4k_close(a->device); g_free(a->device); a->device = NULL; }
    if (!a->closing && !test) {
        if (discover_device(a, "auto") == 0) update_status_labels(a);
        else gtk_label_set_text(a->lbl_error, "Waiting for the capture device to reconnect…");
        refresh_audio_state(a);
    }
    if (!a->closing) {
        if (a->device) gtk_label_set_text(a->lbl_error, a->devpath);
        audio_set_controls(a, FALSE);
    }
}

static void launch_audio_command(struct App *a, gboolean test)
{
    if (a->mock_backend) return;
    AudioState state = audio_kernel_state();
    gboolean consent = gtk_check_button_get_active(a->audio_consent);
    if (a->audio_process || a->report_process || state == AUDIO_UNSUPPORTED || (test && state != AUDIO_ON) ||
        (!test && state == AUDIO_OFF && !consent)) return;
    GError *err = NULL; const char *argv_enable[] = { "pkexec", "/usr/libexec/openlivegamer4k/audio-helper", state == AUDIO_ON ? "disable" : "enable", NULL };
    const char *argv_test[] = { "/usr/libexec/openlivegamer4k/audio-test", NULL };
    if (!test) stop_preview(a);
    if (!test && a->device) { olg4k_close(a->device); g_free(a->device); a->device = NULL; }
    a->testing = test; audio_set_controls(a, TRUE);
    gtk_label_set_text(a->audio_result, test ? "Running 5-second HDMI audio test…" : "Applying kernel audio change…");
    a->audio_process = g_subprocess_newv(test ? argv_test : argv_enable,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &err);
    if (!a->audio_process) {
        gtk_label_set_text(a->audio_result, err ? err->message : "Could not start audio command");
        g_clear_error(&err); a->testing = FALSE;
        if (!test && discover_device(a, "auto") == 0) update_status_labels(a);
        audio_set_controls(a, FALSE); return;
    }
    g_subprocess_communicate_utf8_async(a->audio_process, NULL, NULL, audio_done, a);
}

static void on_audio_toggle(GtkButton *button, gpointer data)
{
    struct App *a = data; (void)button; launch_audio_command(a, FALSE);
}
static void on_audio_test(GtkButton *button, gpointer data)
{
    struct App *a = data; (void)button; launch_audio_command(a, TRUE);
}
static void on_consent_toggled(GtkCheckButton *button, gpointer data)
{
    struct App *a = data;
    GKeyFile *key = g_key_file_new();
    char *dir = g_build_filename(g_get_user_config_dir(), "openlivegamer4k", NULL);
    char *path = g_build_filename(dir, "control.ini", NULL);
    g_mkdir_with_parents(dir, 0700);
    g_key_file_set_boolean(key, "audio", "consent", gtk_check_button_get_active(button));
    gsize length = 0; char *contents = g_key_file_to_data(key, &length, NULL);
    GError *error = NULL;
    if (!g_file_set_contents(path, contents, length, &error)) {
        if (a->lbl_error) gtk_label_set_text(a->lbl_error, error->message);
        g_clear_error(&error);
    }
    g_free(contents); g_free(path); g_free(dir); g_key_file_unref(key);
    refresh_audio_state(a);
}

static void report_done(GObject *source, GAsyncResult *res, gpointer data)
{
    struct App *a = data; GError *error = NULL; char *out = NULL, *errout = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res, &out, &errout, &error);
    gboolean success = ok && g_subprocess_get_successful(G_SUBPROCESS(source));
    g_clear_object(&a->report_process);
    if (!a->closing) {
        const char *message = success ? (out && *out ? out : "Diagnostic report saved.") :
            errout && *errout ? errout : error ? error->message : "Could not save diagnostic report.";
        gtk_label_set_text(a->audio_result, message);
        refresh_audio_state(a);
    }
    g_clear_error(&error); g_free(out); g_free(errout);
}

static void report_chooser_response(GtkNativeDialog *native, int response, gpointer data)
{
    struct App *a = data; GError *error = NULL;
    if (response == GTK_RESPONSE_ACCEPT && !a->mock_backend && !a->report_process && !a->audio_process) {
        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(native));
        char *path = file ? g_file_get_path(file) : NULL;
        if (path) {
            const char *argv[] = { "/usr/libexec/openlivegamer4k/collect-audio-report", path, NULL };
            a->report_process = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
            if (a->report_process)
                g_subprocess_communicate_utf8_async(a->report_process, NULL, NULL, report_done, a);
        }
        if (file) g_object_unref(file);
        if (!a->report_process && !a->closing)
            gtk_label_set_text(a->audio_result, error ? error->message : "Could not start report collector.");
        g_free(path); g_clear_error(&error);
    }
    g_object_unref(native);
}

static void on_save_report(GtkButton *button, gpointer data)
{
    struct App *a = data;
    (void)button;
    if (a->mock_backend || a->report_process || a->audio_process) return;
    GtkFileChooserNative *chooser = gtk_file_chooser_native_new("Save diagnostic report",
        a->window, GTK_FILE_CHOOSER_ACTION_SAVE, "Save", "Cancel");
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), "openlivegamer4k-diagnostics.json");
    g_signal_connect(chooser, "response", G_CALLBACK(report_chooser_response), a);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}

static gboolean load_audio_consent(void)
{
    char *path = g_build_filename(g_get_user_config_dir(), "openlivegamer4k", "control.ini", NULL);
    GKeyFile *key = g_key_file_new(); GError *error = NULL;
    gboolean consent = g_key_file_load_from_file(key, path, G_KEY_FILE_NONE, NULL) &&
        g_key_file_get_boolean(key, "audio", "consent", &error);
    g_clear_error(&error); g_key_file_unref(key); g_free(path);
    return consent;
}

static gboolean on_close_request(GtkWindow *window, gpointer data)
{
    struct App *a = data; (void)window;
    if (a->audio_process || a->report_process) {
        gtk_label_set_text(a->lbl_error, a->testing ?
            "Please wait for the five-second audio test to finish before closing." :
            a->report_process ? "Please wait for the diagnostic report to finish before closing." :
            "Please wait for the kernel audio reload to finish before closing.");
        return TRUE;
    }
    return FALSE;
}

static gboolean audio_status_tick(gpointer data)
{
    struct App *a = data;
    if (!a->closing && !a->audio_process && !a->mock_backend) {
        if (!a->device && discover_device(a, "auto") == 0) {
            update_status_labels(a);
            gtk_label_set_text(a->lbl_error, a->devpath);
            gtk_widget_set_sensitive(GTK_WIDGET(a->btn_preview), TRUE);
        }
        refresh_audio_state(a);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean fps_tick(gpointer data)
{
    struct App *a = data;
    if (a->worker && olg4k_worker_state(a->worker) == OLG4K_WORKER_ERROR) {
        const char *why = olg4k_worker_error(a->worker);
        char *message = g_strdup_printf("Preview failed: %s", why ? why : "capture worker error");
        stop_preview(a);
        gtk_label_set_text(a->lbl_error, message);
        g_free(message);
        return G_SOURCE_REMOVE;
    }
    if (a->worker && olg4k_worker_state(a->worker) == OLG4K_WORKER_RUNNING) {
        unsigned long long n = olg4k_worker_frames(a->worker);
        a->fps = (int)(n - a->last_frames);
        a->last_frames = n;
        {
            char buf[64];
            g_snprintf(buf, sizeof(buf), "%d fps", a->fps);
            gtk_label_set_text(a->status_rate, buf);
        }
    }
    return G_SOURCE_CONTINUE;
}

/* ---- device discovery ---- */

static int discover_device(struct App *a, const char *which)
{
    if (g_strcmp0(which, "mock") == 0) {
        a->backend = &olg4k_backend_mock;
        g_strlcpy(a->devpath, "/dev/mock0", sizeof(a->devpath));
        a->device = g_new0(Olg4kDevice, 1);
        if (olg4k_open(a->device, a->backend, NULL, a->devpath,
                       OLG4K_MODE_STATUS))
            return -1;
        return 0;
    }

    a->backend = &olg4k_backend_real;
    for (int i = 0; i < 32; i++) {
        char path[32];
        Olg4kDevice *probe = g_new0(Olg4kDevice, 1);
        g_snprintf(path, sizeof(path), "/dev/video%d", i);
        if (olg4k_open(probe, a->backend, NULL, path, OLG4K_MODE_STATUS)) {
            g_free(probe);
            continue;
        }
        {
            struct v4l2_capability cap;
            memset(&cap, 0, sizeof(cap));
            if (olg4k_querycap(probe, &cap) == 0 &&
                strstr((char *)cap.card, "Live Gamer 4K")) {
                a->device = probe;
                g_strlcpy(a->devpath, path, sizeof(a->devpath));
                return 0;
            }
            olg4k_close(probe);
            g_free(probe);
        }
    }
    return -1;
}

static void set_row(GtkLabel *key, GtkLabel *val, const char *text)
{
    (void)key;
    gtk_label_set_text(val, text);
}

static void update_status_labels(struct App *a)
{
    struct v4l2_capability cap;
    struct v4l2_input in;
    struct v4l2_format f;
    struct v4l2_streamparm p;
    char buf[128];

    if (!a->device)
        return;

    memset(&cap, 0, sizeof(cap));
    if (olg4k_querycap(a->device, &cap) == 0) {
        g_snprintf(buf, sizeof(buf), "%s  (%s)", (char *)cap.card,
                   (char *)cap.driver);
        set_row(NULL, a->status_driver, buf);
    }

    memset(&in, 0, sizeof(in));
    if (olg4k_enum_input(a->device, 0, &in) == 0)
        set_row(NULL, a->status_input, (char *)in.name);

    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (olg4k_g_fmt(a->device, &f) == 0) {
        static const char *names[] = { "YUYV 4:2:2", "NV12 4:2:0",
                                       "RGB24", "BGR24" };
        static const uint32_t fourccs[] = {
            V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_NV12,
            V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_BGR24 };
        const char *fmt = "unknown";
        for (unsigned int i = 0; i < G_N_ELEMENTS(fourccs); i++)
            if (fourccs[i] == f.fmt.pix.pixelformat)
                fmt = names[i];
        set_row(NULL, a->status_format, fmt);
        g_snprintf(buf, sizeof(buf), "%ux%u  (stride %u)", f.fmt.pix.width,
                   f.fmt.pix.height, f.fmt.pix.bytesperline);
        set_row(NULL, a->status_size, buf);
    }

    memset(&p, 0, sizeof(p));
    p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (olg4k_g_parm(a->device, &p) == 0) {
        double fps = (double)p.parm.capture.timeperframe.denominator /
                     (double)p.parm.capture.timeperframe.numerator;
        g_snprintf(buf, sizeof(buf), "%.3f fps", fps);
        if (!a->previewing)
            set_row(NULL, a->status_rate, buf);
    }
}

static void refresh_mode_label(struct App *a)
{
    gtk_label_set_text(a->status_mode,
                       a->previewing ? "preview (OBS must be stopped)"
                                     : "read-only (OBS may be running)");
}

/* ---- preview start/stop ---- */

static void set_preview_ui(struct App *a, bool active)
{
    a->previewing = active;
    gtk_widget_set_sensitive(GTK_WIDGET(a->cmb_format), !active);
    gtk_widget_set_sensitive(GTK_WIDGET(a->cmb_size), !active);
    gtk_widget_set_sensitive(GTK_WIDGET(a->cmb_rate), !active);
    gtk_button_set_label(a->btn_preview, active ? "Stop preview"
                                                : "Start preview");
    refresh_mode_label(a);
}

static void apply_selections(struct App *a)
{
    int fi = gtk_combo_box_get_active(GTK_COMBO_BOX(a->cmb_format));
    int si = gtk_combo_box_get_active(GTK_COMBO_BOX(a->cmb_size));
    int ri = gtk_combo_box_get_active(GTK_COMBO_BOX(a->cmb_rate));
    struct v4l2_format f;
    struct v4l2_streamparm p;
    static const uint32_t fourccs[] = {
        V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_NV12,
        V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_BGR24 };
    static const struct { unsigned w, h; } sizes[] = {
        { 3840, 2160 }, { 1920, 1080 }, { 1280, 720 }, { 1280, 800 } };
    static const struct { unsigned num, den; } rates[] = {
        { 1, 60 }, { 1001, 60000 }, { 1, 50 }, { 1, 30 },
        { 1001, 30000 }, { 1, 25 }, { 1, 24 }, { 1001, 24000 } };

    if (fi < 0 || si < 0 || ri < 0)
        return;
    if (fi >= (int)G_N_ELEMENTS(fourccs) || si >= (int)G_N_ELEMENTS(sizes) ||
        ri >= (int)G_N_ELEMENTS(rates))
        return;

    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = sizes[si].w;
    f.fmt.pix.height = sizes[si].h;
    f.fmt.pix.pixelformat = fourccs[fi];
        if (olg4k_s_fmt(a->device, &f))
    {
        gtk_label_set_text(a->lbl_error, "Could not apply capture format (device busy or unsupported setting).");
        return;
    }

    memset(&p, 0, sizeof(p));
    p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    p.parm.capture.timeperframe.numerator = rates[ri].num;
    p.parm.capture.timeperframe.denominator = rates[ri].den;
    if (olg4k_s_parm(a->device, &p)) {
        gtk_label_set_text(a->lbl_error, "Could not apply capture rate (device busy or unsupported setting).");
        return;
    }

    update_status_labels(a);
}

static void on_preview_clicked(GtkButton *btn, gpointer data)
{
    struct App *a = data;
    (void)btn;

    if (!a->previewing) {
        if (!a->device)
            return;
        /* preview takes the exclusive node: switch mode, apply settings */
        olg4k_set_mode(a->device, OLG4K_MODE_PREVIEW);
        apply_selections(a);
        if (!olg4k_worker_prepare(&a->worker, a->device, 4, on_frame, a)) {
            gtk_label_set_text(a->lbl_error, "preview setup failed");
            olg4k_set_mode(a->device, OLG4K_MODE_STATUS);
            return;
        }
        if (!olg4k_worker_start(a->worker)) {
            gtk_label_set_text(a->lbl_error,
                               "preview start failed (device busy?)");
            olg4k_worker_free(a->worker);
            a->worker = NULL;
            olg4k_set_mode(a->device, OLG4K_MODE_STATUS);
            return;
        }
        a->last_frames = 0;
        a->fps = 0;
        a->fps_timer_id = g_timeout_add(1000, fps_tick, a);
        gtk_widget_set_visible(GTK_WIDGET(a->preview_image), TRUE);
        set_preview_ui(a, true);
    } else {
        stop_preview(a);
        update_status_labels(a);
    }
}



/* ---- window ---- */

static void add_row(GtkWidget *box, const char *key, GtkLabel **val_out)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkLabel *k = GTK_LABEL(gtk_label_new(key));
    GtkLabel *v = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(k, 0.0);
    gtk_label_set_xalign(v, 1.0);
    gtk_widget_set_size_request(GTK_WIDGET(k), 100, -1);
    gtk_widget_set_hexpand(GTK_WIDGET(v), TRUE);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(k));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(v));
    gtk_box_append(GTK_BOX(box), row);
    *val_out = v;
}

static void populate_combos(struct App *a)
{
    gtk_combo_box_text_append_text(a->cmb_format, "YUYV 4:2:2");
    gtk_combo_box_text_append_text(a->cmb_format, "NV12 4:2:0");
    gtk_combo_box_text_append_text(a->cmb_format, "RGB24");
    gtk_combo_box_text_append_text(a->cmb_format, "BGR24");
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->cmb_format), 0);

    gtk_combo_box_text_append_text(a->cmb_size, "3840 x 2160");
    gtk_combo_box_text_append_text(a->cmb_size, "1920 x 1080");
    gtk_combo_box_text_append_text(a->cmb_size, "1280 x 720");
    gtk_combo_box_text_append_text(a->cmb_size, "1280 x 800");
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->cmb_size), 1);

    gtk_combo_box_text_append_text(a->cmb_rate, "60 fps");
    gtk_combo_box_text_append_text(a->cmb_rate, "59.94 fps");
    gtk_combo_box_text_append_text(a->cmb_rate, "50 fps");
    gtk_combo_box_text_append_text(a->cmb_rate, "30 fps");
    gtk_combo_box_text_append_text(a->cmb_rate, "29.97 fps");
    gtk_combo_box_text_append_text(a->cmb_rate, "25 fps");
    gtk_combo_box_text_append_text(a->cmb_rate, "24 fps");
    gtk_combo_box_text_append_text(a->cmb_rate, "23.976 fps");
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->cmb_rate), 3);
}

static void on_activate(GtkApplication *app, gpointer data)
{
    struct App *a = data;
    if (a->window) {
        gtk_window_present(a->window);
        return;
    }

    a->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(a->window, "OpenLiveGamer4K Control");
    gtk_window_set_default_size(a->window, 900, 760);
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *root_widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), root_widget);
    gtk_window_set_child(a->window, scroller);
    {
        GtkBox *root = GTK_BOX(root_widget);

        /* status grid */
        {
            GtkWidget *grid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
            gtk_widget_set_margin_top(grid, 12);
            gtk_widget_set_margin_bottom(grid, 6);
            gtk_widget_set_margin_start(grid, 12);
            gtk_widget_set_margin_end(grid, 12);

            add_row(grid, "Device:", &a->status_driver);
            add_row(grid, "Input:", &a->status_input);
            add_row(grid, "Format:", &a->status_format);
            add_row(grid, "Size:", &a->status_size);
            add_row(grid, "Rate:", &a->status_rate);
            add_row(grid, "Mode:", &a->status_mode);
            gtk_box_append(root, GTK_WIDGET(grid));
        }

        /* preview area */
        a->preview_image = GTK_PICTURE(gtk_picture_new());
        gtk_picture_set_can_shrink(a->preview_image, TRUE);
        gtk_picture_set_content_fit(a->preview_image, GTK_CONTENT_FIT_CONTAIN);
        gtk_widget_set_size_request(GTK_WIDGET(a->preview_image), 640, 360);
        gtk_widget_set_hexpand(GTK_WIDGET(a->preview_image), TRUE);
        gtk_widget_set_visible(GTK_WIDGET(a->preview_image), FALSE);
        gtk_widget_set_halign(GTK_WIDGET(a->preview_image), GTK_ALIGN_CENTER);
        gtk_box_append(root, GTK_WIDGET(a->preview_image));

        /* capture settings */
        {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_margin_start(row, 12);
            gtk_widget_set_margin_end(row, 12);
            a->cmb_format = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
            a->cmb_size = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
            a->cmb_rate = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
            gtk_box_append(GTK_BOX(row), GTK_WIDGET(a->cmb_format));
            gtk_box_append(GTK_BOX(row), GTK_WIDGET(a->cmb_size));
            gtk_box_append(GTK_BOX(row), GTK_WIDGET(a->cmb_rate));
            gtk_box_append(GTK_BOX(row), GTK_WIDGET(gtk_separator_new(GTK_ORIENTATION_VERTICAL)));
            a->btn_preview = GTK_BUTTON(gtk_button_new_with_label("Start preview"));
            gtk_box_append(GTK_BOX(row), GTK_WIDGET(a->btn_preview));
        gtk_box_append(root, GTK_WIDGET(row));

        GtkWidget *audio = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_start(audio, 12); gtk_widget_set_margin_end(audio, 12);
        a->audio_state = GTK_LABEL(gtk_label_new("Kernel audio: checking…"));
        gtk_label_set_xalign(a->audio_state, 0.0);
        gtk_box_append(GTK_BOX(audio), GTK_WIDGET(a->audio_state));
        GtkWidget *warning = gtk_label_new("HDMI audio is enabled by default: 48 kHz stereo LPCM with concurrent video capture. Video stop or HDMI recovery can interrupt audio; restart audio capture if needed. Enabling or disabling reloads HDMI and requires capture apps to be idle.");
        gtk_label_set_xalign(GTK_LABEL(warning), 0.0); gtk_label_set_wrap(GTK_LABEL(warning), TRUE);
        gtk_box_append(GTK_BOX(audio), warning);
        a->audio_consent = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
            "I understand that changing audio mode reconnects HDMI"));
        gtk_check_button_set_active(a->audio_consent, load_audio_consent());
        gtk_box_append(GTK_BOX(audio), GTK_WIDGET(a->audio_consent));
        GtkWidget *arow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        a->audio_enable = GTK_BUTTON(gtk_button_new_with_label("Enable HDMI audio"));
        a->audio_test = GTK_BUTTON(gtk_button_new_with_label("Run 5-second HDMI test"));
        gtk_widget_set_sensitive(GTK_WIDGET(a->audio_enable), FALSE);
        gtk_box_append(GTK_BOX(arow), GTK_WIDGET(a->audio_enable));
        gtk_box_append(GTK_BOX(arow), GTK_WIDGET(a->audio_test));
        gtk_box_append(GTK_BOX(audio), arow);
        a->audio_result = GTK_LABEL(gtk_label_new(""));
        gtk_label_set_xalign(a->audio_result, 0.0); gtk_label_set_wrap(a->audio_result, TRUE);
        gtk_box_append(GTK_BOX(audio), GTK_WIDGET(a->audio_result));
        a->report_button = GTK_BUTTON(gtk_button_new_with_label("Save diagnostic report…"));
        gtk_box_append(GTK_BOX(audio), GTK_WIDGET(a->report_button));
        GtkWidget *issue = gtk_link_button_new_with_label(
            "https://github.com/royakem/OpenLiveGamer4K/issues/new?template=capture-problem.yml",
            "Report a problem on GitHub…");
        gtk_box_append(GTK_BOX(audio), issue);
        GtkWidget *report_help = gtk_label_new(
            "Save a diagnostic report while the problem is present, review it, then attach or paste it into your issue. No report is uploaded automatically. Include the input resolution/refresh rate and steps to reproduce.");
        gtk_label_set_wrap(GTK_LABEL(report_help), TRUE);
        gtk_label_set_xalign(GTK_LABEL(report_help), 0.0);
        gtk_box_append(GTK_BOX(audio), report_help);
        gtk_box_append(root, audio);
        }

        /* error/status line */
        a->lbl_error = GTK_LABEL(gtk_label_new(""));
        gtk_label_set_xalign(a->lbl_error, 0.0);
        gtk_label_set_wrap(a->lbl_error, TRUE);
        gtk_widget_set_margin_start(GTK_WIDGET(a->lbl_error), 12);
        gtk_widget_set_margin_end(GTK_WIDGET(a->lbl_error), 12);
        gtk_widget_set_margin_bottom(GTK_WIDGET(a->lbl_error), 12);
        gtk_box_append(root, GTK_WIDGET(a->lbl_error));
        }

        populate_combos(a);

        gtk_widget_add_css_class(GTK_WIDGET(a->btn_preview), "suggested-action");
        g_signal_connect(a->btn_preview, "clicked",
                         G_CALLBACK(on_preview_clicked), a);
        g_signal_connect(a->audio_enable, "clicked", G_CALLBACK(on_audio_toggle), a);
        g_signal_connect(a->audio_test, "clicked", G_CALLBACK(on_audio_test), a);
        g_signal_connect(a->report_button, "clicked", G_CALLBACK(on_save_report), a);
        g_signal_connect(a->audio_consent, "toggled", G_CALLBACK(on_consent_toggled), a);
        g_signal_connect(a->window, "close-request", G_CALLBACK(on_close_request), a);
        a->audio_status_timer = g_timeout_add_seconds(2, audio_status_tick, a);
        refresh_audio_state(a);

        if (a->device) {
            update_status_labels(a);
            refresh_mode_label(a);
            gtk_label_set_text(a->lbl_error, a->devpath);
        } else {
            gtk_label_set_text(a->lbl_error,
                "No Live Gamer 4K device found. Start the app with "
                "--backend mock for offline use.");
            gtk_widget_set_sensitive(GTK_WIDGET(a->btn_preview), FALSE);
        }
        gtk_window_present(a->window);
}

int main(int argc, char **argv)
{
    GtkApplication *gtk_app;
    struct App app = { 0 };
    const char *which = "auto";
    GOptionEntry opts[] = {
        { "backend", 'b', 0, G_OPTION_ARG_STRING, &which,
          "device backend: auto or mock", "backend" },
        { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
    };
    GOptionContext *ctx;
    int rc;
    ctx = g_option_context_new(NULL);
    g_option_context_add_main_entries(ctx, opts, NULL);
    if (!g_option_context_parse(ctx, &argc, &argv, NULL))
        return 1;

    gtk_app = gtk_application_new("io.github.royakem.OpenLiveGamer4KControl",
                                  G_APPLICATION_DEFAULT_FLAGS);
    app.app = gtk_app;

    discover_device(&app, which);
    app.mock_backend = g_strcmp0(which, "mock") == 0;

    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), &app);
    rc = g_application_run(G_APPLICATION(gtk_app), argc, argv);

    g_mutex_lock(&app.frame_lock);
    app.closing = TRUE;
    if (app.frame_source_id) { g_source_remove(app.frame_source_id); app.frame_source_id = 0; }
    app.frame_source_pending = FALSE;
    g_free(app.pending_rgb); app.pending_rgb = NULL;
    g_mutex_unlock(&app.frame_lock);
    if (app.audio_status_timer) g_source_remove(app.audio_status_timer);
    if (app.audio_process) {
        g_subprocess_wait(app.audio_process, NULL, NULL);
        while (app.audio_process)
            g_main_context_iteration(NULL, TRUE);
    }
    if (app.report_process) {
        g_subprocess_wait(app.report_process, NULL, NULL);
        while (app.report_process)
            g_main_context_iteration(NULL, TRUE);
    }

    if (app.fps_timer_id)
        g_source_remove(app.fps_timer_id);
    if (app.worker) {
        olg4k_worker_stop(app.worker);
        olg4k_worker_free(app.worker);
    }
    if (app.device) {
        olg4k_close(app.device);
        g_free(app.device);
    }
    g_mutex_clear(&app.frame_lock);
    g_object_unref(gtk_app);
    g_option_context_free(ctx);
    return rc;
}
