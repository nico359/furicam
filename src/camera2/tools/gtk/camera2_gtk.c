/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * camera2_gtk — a minimal GTK4 live-preview prototype driving the shared
 * furicam2_camera engine through its backend-neutral C API.  A GtkGLArea hosts
 * the GLES context; each frame fc2_render_preview() draws the latest camera
 * buffer (zero-copy AHardwareBuffer -> EGLImage -> external-OES texture).
 *
 * This exists to compare GTK4 vs Qt Quick as the app frontend — same engine,
 * different toolkit.
 */

#include "furicam2_camera_c.h"

#include <gtk/gtk.h>

static fc2_camera* g_cam      = NULL;
static int         g_rotation = 90;
static GDBusProxy* g_sensor   = NULL;   /* net.hadess.SensorProxy (auto-rotate) */

static void noop_frame_cb(void* user) { (void)user; }   /* disables engine-side frame draining */

/* iio-sensor-proxy AccelerometerOrientation -> device rotation (clockwise from
 * natural portrait), then counter-rotate the preview so it stays world-upright. */
static int device_rotation_for(const char* o)
{
    if (g_strcmp0(o, "left-up")   == 0) return 90;
    if (g_strcmp0(o, "bottom-up") == 0) return 180;
    if (g_strcmp0(o, "right-up")  == 0) return 270;
    return 0;   /* "normal" / unknown */
}

static void refresh_rotation(void)
{
    if (!g_cam)
        return;
    int dev = 0;
    if (g_sensor) {
        GVariant* v = g_dbus_proxy_get_cached_property(g_sensor, "AccelerometerOrientation");
        if (v) {
            dev = device_rotation_for(g_variant_get_string(v, NULL));
            g_variant_unref(v);
        }
    }
    g_rotation = ((fc2_sensor_orientation(g_cam) - dev + 180) % 360 + 360) % 360;
}

static void on_sensor_props(GDBusProxy* p, GVariant* changed, char** inval, gpointer u)
{
    (void)p; (void)changed; (void)inval; (void)u;
    refresh_rotation();
}

static void setup_orientation(void)
{
    g_sensor = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "net.hadess.SensorProxy", "/net/hadess/SensorProxy", "net.hadess.SensorProxy",
        NULL, NULL);
    if (!g_sensor)
        return;
    g_dbus_proxy_call_sync(g_sensor, "ClaimAccelerometer", NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    g_signal_connect(g_sensor, "g-properties-changed", G_CALLBACK(on_sensor_props), NULL);
    refresh_rotation();
}

static void teardown_orientation(void)
{
    if (g_sensor) {
        g_dbus_proxy_call_sync(g_sensor, "ReleaseAccelerometer", NULL,
                               G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
        g_object_unref(g_sensor);
        g_sensor = NULL;
    }
}

static gboolean tick_cb(GtkWidget* area, GdkFrameClock* clock, gpointer user)
{
    (void)clock;
    (void)user;
    gtk_gl_area_queue_render(GTK_GL_AREA(area));   /* redraw at display refresh */
    return G_SOURCE_CONTINUE;
}

static void on_realize(GtkGLArea* area, gpointer user)
{
    (void)user;
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != NULL) {
        g_printerr("camera2_gtk: GtkGLArea context error\n");
        return;
    }
    g_cam = fc2_open(FC2_FACING_BACK);
    if (!g_cam) {
        g_printerr("camera2_gtk: fc2_open failed (camera unavailable?)\n");
        return;
    }
    fc2_set_frame_callback(g_cam, noop_frame_cb, NULL);
    if (!fc2_start_preview(g_cam, 1280, 720))
        g_printerr("camera2_gtk: fc2_start_preview failed\n");
    setup_orientation();   /* device-orientation-aware preview rotation */
    gtk_widget_add_tick_callback(GTK_WIDGET(area), tick_cb, NULL, NULL);
}

static void on_unrealize(GtkGLArea* area, gpointer user)
{
    (void)area;
    (void)user;
    teardown_orientation();
    if (g_cam) {
        fc2_stop_preview(g_cam);
        fc2_close(g_cam);
        g_cam = NULL;
    }
}

static gboolean on_render(GtkGLArea* area, GdkGLContext* ctx, gpointer user)
{
    (void)ctx;
    (void)user;
    if (g_cam) {
        /* GtkGLArea's framebuffer is in device pixels (logical size x scale),
         * so scale the GL viewport to match or it draws into a corner. */
        int scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));
        if (scale < 1)
            scale = 1;
        int w = gtk_widget_get_width(GTK_WIDGET(area)) * scale;
        int h = gtk_widget_get_height(GTK_WIDGET(area)) * scale;
        fc2_render_preview(g_cam, w, h, g_rotation);
    }
    return TRUE;
}

static void on_activate(GtkApplication* app, gpointer user)
{
    (void)user;
    GtkWidget* win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Camera2 GTK preview");
    gtk_window_set_default_size(GTK_WINDOW(win), 720, 1440);

    GtkWidget* area = gtk_gl_area_new();
    gtk_gl_area_set_allowed_apis(GTK_GL_AREA(area), GDK_GL_API_GLES);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(area), FALSE);
    g_signal_connect(area, "realize",   G_CALLBACK(on_realize),   NULL);
    g_signal_connect(area, "unrealize", G_CALLBACK(on_unrealize), NULL);
    g_signal_connect(area, "render",    G_CALLBACK(on_render),    NULL);

    gtk_window_set_child(GTK_WINDOW(win), area);
    gtk_window_fullscreen(GTK_WINDOW(win));
    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char** argv)
{
    GtkApplication* app = gtk_application_new("org.furios.camera2gtk", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
