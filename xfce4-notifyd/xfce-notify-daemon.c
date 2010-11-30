/*
 *  xfce4-notifyd
 *
 *  Copyright (c) 2008 Brian Tarricone <bjt23@cornell.edu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License ONLY.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <libxfce4util/libxfce4util.h>
#include <libxfcegui4/libxfcegui4.h>

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <xfconf/xfconf.h>

#include "xfce-notify-daemon.h"
#include "xfce-notify-window.h"
#include "xfce-notify-marshal.h"

struct _XfceNotifyDaemon
{
    GObject parent;

    gint expire_timeout;
    gboolean fade_transparency;
    gdouble initial_opacity;
    GtkCornerType notify_location;

    DBusGConnection *dbus_conn;
    XfconfChannel *settings;

    GTree *active_notifications;

    guint32 last_notification_id;
};

typedef struct
{
    GObjectClass parent;
} XfceNotifyDaemonClass;

enum
{
    SIG_NOTIFICATION_CLOSED = 0,
    SIG_ACTION_INVOKED,
    N_SIGS,
};

enum
{
    URGENCY_LOW = 0,
    URGENCY_NORMAL,
    URGENCY_CRITICAL,
};

static void xfce_notify_daemon_class_init(XfceNotifyDaemonClass *klass);
static void xfce_notify_daemon_init(XfceNotifyDaemon *daemon);

static void xfce_notify_daemon_finalize(GObject *obj);

static gboolean notify_get_capabilities(XfceNotifyDaemon *daemon,
                                        gchar ***OUT_capabilities,
                                        GError *error);
static gboolean notify_notify(XfceNotifyDaemon *daemon,
                              const gchar *app_name,
                              guint replaces_id,
                              const gchar *app_icon,
                              const gchar *summary,
                              const gchar *body,
                              const gchar **actions,
                              GHashTable *hints,
                              gint expire_timeout,
                              guint *OUT_id,
                              GError **error);
static gboolean notify_close_notification(XfceNotifyDaemon *daemon,
                                          guint id,
                                          GError **error);
static gboolean notify_get_server_information(XfceNotifyDaemon *daemon,
                                              gchar **OUT_name,
                                              gchar **OUT_vendor,
                                              gchar **OUT_version,
                                              gchar **OUT_spec_version,
                                              GError **error);

static gboolean notify_quit(XfceNotifyDaemon *daemon,
                            GError **error);

static GdkPixbuf *notify_pixbuf_from_image_data(const GValue *image_data);

#include "notify-dbus.h"

static guint signals[N_SIGS] = { 0, };


G_DEFINE_TYPE(XfceNotifyDaemon, xfce_notify_daemon, G_TYPE_OBJECT)


static void
xfce_notify_daemon_class_init(XfceNotifyDaemonClass *klass)
{
    GObjectClass *gobject_class = (GObjectClass *)klass;

    gobject_class->finalize = xfce_notify_daemon_finalize;

    signals[SIG_NOTIFICATION_CLOSED] = g_signal_new("notification-closed",
                                                    XFCE_TYPE_NOTIFY_DAEMON,
                                                    G_SIGNAL_RUN_LAST,
                                                    0,
                                                    NULL, NULL,
                                                    g_cclosure_marshal_VOID__UINT,
                                                    G_TYPE_NONE, 1,
                                                    G_TYPE_UINT);
                                                    xfce_notify_marshal_VOID__UINT_UINT,
                                                    G_TYPE_NONE, 2,
                                                    G_TYPE_UINT,
                                                    G_TYPE_UINT);
    signals[SIG_ACTION_INVOKED] = g_signal_new("action-invoked",
                                               XFCE_TYPE_NOTIFY_DAEMON,
                                               G_SIGNAL_RUN_LAST,
                                               0,
                                               NULL, NULL,
                                               xfce_notify_marshal_VOID__UINT_STRING,
                                               G_TYPE_NONE, 2,
                                               G_TYPE_UINT,
                                               G_TYPE_STRING);

    dbus_g_object_type_install_info(G_TYPE_FROM_CLASS(klass),
                                    &dbus_glib_notify_object_info);
}

static gint
xfce_direct_compare(gconstpointer a,
                    gconstpointer b,
                    gpointer user_data)
{
    return (gint)(a - b);
}

static void
xfce_notify_daemon_init(XfceNotifyDaemon *daemon)
{
    daemon->active_notifications = g_tree_new_full(xfce_direct_compare,
                                                   NULL, NULL,
                                                   (GDestroyNotify)gtk_widget_destroy);

    daemon->last_notification_id = 1;
}

static void
xfce_notify_daemon_finalize(GObject *obj)
{
    XfceNotifyDaemon *daemon = XFCE_NOTIFY_DAEMON(obj);
    
    g_tree_destroy(daemon->active_notifications);

    if(daemon->settings)
        g_object_unref(daemon->settings);

    if(daemon->dbus_conn)
        dbus_g_connection_unref(daemon->dbus_conn);

    G_OBJECT_CLASS(xfce_notify_daemon_parent_class)->finalize(obj);
}



static guint32
xfce_notify_daemon_generate_id(XfceNotifyDaemon *daemon)
{
    if(G_UNLIKELY(daemon->last_notification_id == 0))
        daemon->last_notification_id = 1;

    return daemon->last_notification_id++;
}

static void
xfce_notify_daemon_window_action_invoked(XfceNotifyWindow *window,
                                         const gchar *action,
                                         gpointer user_data)
{
    XfceNotifyDaemon *daemon = user_data;
    guint id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(window),
                                                  "--notify-id"));
    g_signal_emit(G_OBJECT(daemon), signals[SIG_ACTION_INVOKED], 0,
                  id, action);
}

static void
xfce_notify_daemon_window_closed(XfceNotifyWindow *window,
                                 XfceNotifyCloseReason reason,
                                 gpointer user_data)
{
    XfceNotifyDaemon *daemon = user_data;
    gpointer id_p = g_object_get_data(G_OBJECT(window), "--notify-id");

    g_tree_remove(daemon->active_notifications, id_p);
    g_signal_emit(G_OBJECT(daemon), signals[SIG_NOTIFICATION_CLOSED], 0,
                  GPOINTER_TO_UINT(id_p), reason);
}

static void
xfce_notify_daemon_window_size_allocate(GtkWidget *widget,
                                        GtkAllocation *allocation,
                                        gpointer user_data)
{
    XfceNotifyDaemon *daemon = user_data;
    GdkScreen *screen = NULL;
    gint x, y, monitor;
    GdkRectangle geom;

    gdk_display_get_pointer(gdk_display_get_default(), &screen, &x, &y, NULL);
    monitor = gdk_screen_get_monitor_at_point(screen, x, y);
    gdk_screen_get_monitor_geometry(screen, monitor, &geom);

    gtk_window_set_screen(GTK_WINDOW(widget), screen);

    switch(daemon->notify_location) {
        case GTK_CORNER_TOP_LEFT:
            x = geom.x + 32;
            y = geom.y + 32;
            break;
        case GTK_CORNER_BOTTOM_LEFT:
            x = geom.x + 32;
            y = geom.height - allocation->height - 32;
            break;
        case GTK_CORNER_TOP_RIGHT:
            x = geom.width - allocation->width - 32;
            y = geom.y + 32;
            break;
        case GTK_CORNER_BOTTOM_RIGHT:
            x = geom.width - allocation->width - 32;
            y = geom.height - allocation->height - 32;
            break;
        default:
            g_warning("Invalid notify location: %d", daemon->notify_location);
            return;
    }

    gtk_window_move(GTK_WINDOW(widget), x, y);
}



static gboolean
notify_get_capabilities(XfceNotifyDaemon *daemon,
                        gchar ***OUT_capabilities,
                        GError *error)
{
    gint i = 0;

    *OUT_capabilities = g_new(gchar *, 6);
    (*OUT_capabilities)[i++] = g_strdup("actions");
    (*OUT_capabilities)[i++] = g_strdup("body");
    (*OUT_capabilities)[i++] = g_strdup("body-markup");
#ifdef HAVE_LIBSEXY
    (*OUT_capabilities)[i++] = g_strdup("body-hyperlinks");
#endif
    (*OUT_capabilities)[i++] = g_strdup("icon-static");
    (*OUT_capabilities)[i++] = NULL;

    return TRUE;
}

static gboolean
notify_notify(XfceNotifyDaemon *daemon,
              const gchar *app_name,
              guint replaces_id,
              const gchar *app_icon,
              const gchar *summary,
              const gchar *body,
              const gchar **actions,
              GHashTable *hints,
              gint expire_timeout,
              guint *OUT_id,
              GError **error)
{
    XfceNotifyWindow *window;
    GdkPixbuf *pix;
    GValue *urgency_data;

    if((urgency_data = g_hash_table_lookup(hints, "urgency"))
       && G_VALUE_HOLDS(urgency_data, G_TYPE_UCHAR)
       && g_value_get_uchar(urgency_data) == URGENCY_CRITICAL)
    {
        /* don't expire urgent notifications */
        expire_timeout = 0;
    }

    if(expire_timeout == -1)
        expire_timeout = daemon->expire_timeout;

    if(replaces_id
       && (window = g_tree_lookup(daemon->active_notifications,
                                  GUINT_TO_POINTER(replaces_id))))
    {
        xfce_notify_window_set_icon_name(window, app_icon);
        xfce_notify_window_set_summary(window, summary);
        xfce_notify_window_set_body(window, body);
        xfce_notify_window_set_actions(window, actions);
        xfce_notify_window_set_expire_timeout(window, expire_timeout);
        xfce_notify_window_set_fade_transparent(window,
                                                daemon->fade_transparency);
        xfce_notify_window_set_opacity(window, daemon->initial_opacity);

        *OUT_id = replaces_id;
    } else {
        window = XFCE_NOTIFY_WINDOW(xfce_notify_window_new_with_actions(summary, body,
                                                                        app_icon,
                                                                        expire_timeout,
                                                                        actions));
        xfce_notify_window_set_fade_transparent(window,
                                                daemon->fade_transparency);
        xfce_notify_window_set_opacity(window, daemon->initial_opacity);

        *OUT_id = xfce_notify_daemon_generate_id(daemon);
        g_object_set_data(G_OBJECT(window), "--notify-id",
                          GUINT_TO_POINTER(*OUT_id));

        g_tree_insert(daemon->active_notifications,
                      GUINT_TO_POINTER(*OUT_id), window);

        g_signal_connect(G_OBJECT(window), "action-invoked",
                         G_CALLBACK(xfce_notify_daemon_window_action_invoked),
                         daemon);
        g_signal_connect(G_OBJECT(window), "closed",
                         G_CALLBACK(xfce_notify_daemon_window_closed),
                         daemon);
        g_signal_connect(G_OBJECT(window), "size-allocate",
                         G_CALLBACK(xfce_notify_daemon_window_size_allocate),
                         daemon);

        gtk_widget_show(GTK_WIDGET(window));
    }

    if(!app_icon || !*app_icon) {
        GValue *image_data = g_hash_table_lookup(hints, "image_data");
        if(!image_data)
            image_data = g_hash_table_lookup(hints, "icon_data");
        if(image_data) {
            pix = notify_pixbuf_from_image_data(image_data);
            if(pix) {
                xfce_notify_window_set_icon_pixbuf(window, pix);
                g_object_unref(G_OBJECT(pix));
            }
        } else {
            GValue *desktop_id = g_hash_table_lookup(hints, "desktop_id");
            if(desktop_id) {
                gchar *resource = g_strdup_printf("applications%c%s.desktop",
                                                  G_DIR_SEPARATOR,
                                                  g_value_get_string(desktop_id));
                XfceRc *rcfile = xfce_rc_config_open(XFCE_RESOURCE_DATA,
                                                     resource, TRUE);
                if(rcfile) {
                    if(xfce_rc_has_group(rcfile, "Desktop Entry")) {
                        const gchar *icon_file;
                        xfce_rc_set_group(rcfile, "Desktop Entry");
                        icon_file = xfce_rc_read_entry(rcfile, "Icon", NULL);
                        if(icon_file) {
                            pix = xfce_themed_icon_load(icon_file, 32);
                            if(pix) {
                                xfce_notify_window_set_icon_pixbuf(window, pix);
                                g_object_unref(G_OBJECT(pix));
                            }
                        }
                    }
                    xfce_rc_close(rcfile);
                }
                g_free(resource);
            }
        }
    }

    gtk_widget_realize(GTK_WIDGET(window));
    xfce_notify_daemon_window_size_allocate(GTK_WIDGET(window),
                                            &GTK_WIDGET(window)->allocation,
                                            daemon);

    return TRUE;
}

static gboolean
notify_close_notification(XfceNotifyDaemon *daemon,
                          guint id,
                          GError **error)
{
    XfceNotifyWindow *window = g_tree_lookup(daemon->active_notifications,
                                             GUINT_TO_POINTER(id));

    if(window)
        xfce_notify_window_closed(window, XFCE_NOTIFY_CLOSE_REASON_CLIENT);

    return TRUE;
}

static gboolean
notify_get_server_information(XfceNotifyDaemon *daemon,
                              gchar **OUT_name,
                              gchar **OUT_vendor,
                              gchar **OUT_version,
                              gchar **OUT_spec_version,
                              GError **error)
{
    *OUT_name = g_strdup("Xfce Notify Daemon");
    *OUT_vendor = g_strdup("Xfce");
    *OUT_version = g_strdup(VERSION);
    *OUT_spec_version = g_strdup(NOTIFICATIONS_SPEC_VERSION);

    return TRUE;
}

static gboolean
notify_quit(XfceNotifyDaemon *daemon,
            GError **error)
{
    gint i, main_level = gtk_main_level();
    for(i = 0; i < main_level; ++i)
        gtk_main_quit();
    return TRUE;
}



static GdkPixbuf *
notify_pixbuf_from_image_data(const GValue *image_data)
{
    GdkPixbuf *pix = NULL;
    GType struct_gtype;
    gint32 width, height, rowstride, bits_per_sample, channels;
    gboolean has_alpha;
    GArray *pixel_array;
    gsize correct_len;

    struct_gtype = dbus_g_type_get_struct("GValueArray", G_TYPE_INT,
                                          G_TYPE_INT, G_TYPE_INT,
                                          G_TYPE_BOOLEAN, G_TYPE_INT,
                                          G_TYPE_INT,
                                          DBUS_TYPE_G_UCHAR_ARRAY,
                                          G_TYPE_INVALID);
    if(!G_VALUE_HOLDS(image_data, struct_gtype)) {
        g_message("Image data is not the correct type");
        return NULL;
    }

    if(!dbus_g_type_struct_get(image_data,
                               0, &width,
                               1, &height,
                               2, &rowstride,
                               3, &has_alpha,
                               4, &bits_per_sample,
                               5, &channels,
                               6, &pixel_array,
                               G_MAXUINT))
    {
        g_message("Unable to retrieve image data struct members");
        return NULL;
    }

    correct_len = (height - 1) * rowstride + width
                  * ((channels * bits_per_sample + 7) / 8);
    if(correct_len != pixel_array->len) {
        g_message("Pixel data length (%d) did not match expected value (%u)",
                  pixel_array->len, (guint)correct_len);
        return NULL;
    }

    pix = gdk_pixbuf_new_from_data(g_memdup(pixel_array->data,
                                            pixel_array->len),
                                   GDK_COLORSPACE_RGB, has_alpha,
                                   bits_per_sample, width, height,
                                   rowstride,
                                   (GdkPixbufDestroyNotify)g_free, NULL);
    return pix;
}

static void
xfce_notify_daemon_set_theme(XfceNotifyDaemon *daemon,
                             const gchar *theme)
{
    gchar *file, **files;
    
    /* old-style ~/.themes ... */
    file = g_build_filename(xfce_get_homedir(), ".themes", theme,
                            "xfce-notify-4.0", "gtkrc", NULL);
    if(g_file_test(file, G_FILE_TEST_EXISTS)) {
        gtk_rc_parse(file);
        g_free(file);
        return;
    }
    g_free(file);

    file = g_strconcat("themes/", theme, "/xfce-notify-4.0/gtkrc", NULL);
    files = xfce_resource_lookup_all(XFCE_RESOURCE_DATA, file);
    if(files[0])
        gtk_rc_parse(files[0]);
    
    g_free(file);
    g_strfreev(files);
}



static void
xfce_notify_daemon_settings_changed(XfconfChannel *channel,
                                    const gchar *property,
                                    const GValue *value,
                                    gpointer user_data)
{
    XfceNotifyDaemon *daemon = user_data;

    if(!strcmp(property, "/expire-timeout")) {
        daemon->expire_timeout = G_VALUE_TYPE(value)
                                 ? g_value_get_int(value) : -1;
        if(daemon->expire_timeout != -1)
            daemon->expire_timeout *= 1000;
    } else if(!strcmp(property, "/fade-transparency")) {
        daemon->fade_transparency = G_VALUE_TYPE(value)
                                    ? g_value_get_boolean(value) : TRUE;
    } else if(!strcmp(property, "/initial-opacity")) {
        daemon->initial_opacity = G_VALUE_TYPE(value)
                                  ? g_value_get_double(value) : 0.9;
    } else if(!strcmp(property, "/theme")) {
        xfce_notify_daemon_set_theme(daemon,
                                     G_VALUE_TYPE(value)
                                     ? g_value_get_string(value)
                                     : "Default");
    } else if(!strcmp(property, "/notify-location")) {
        daemon->notify_location = G_VALUE_TYPE(value)
                                  ? g_value_get_uint(value)
                                  : GTK_CORNER_TOP_RIGHT;
    }
}

static gboolean
xfce_notify_daemon_start(XfceNotifyDaemon *daemon,
                         GError **error)
{
    int ret;
    DBusError derror;
    
    daemon->dbus_conn = dbus_g_bus_get(DBUS_BUS_SESSION, error);
    if(G_UNLIKELY(!daemon->dbus_conn)) {
        if(error && !*error) {
            g_set_error(error, DBUS_GERROR, DBUS_GERROR_FAILED,
                        _("Unable to connect to D-Bus session bus"));
        }
        return FALSE;
    }
   
    dbus_error_init(&derror);
    ret = dbus_bus_request_name(dbus_g_connection_get_connection(daemon->dbus_conn),
                                "org.freedesktop.Notifications",
                                DBUS_NAME_FLAG_DO_NOT_QUEUE,
                                &derror);
    if(DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER != ret) {
        if(dbus_error_is_set(&derror)) {
            if(error)
                dbus_set_g_error(error, &derror);
            dbus_error_free(&derror);
        } else if(error) {
            g_set_error(error, DBUS_GERROR, DBUS_GERROR_FAILED,
                        _("Another notification daemon is already running"));
        }
        
        return FALSE;
    }

    dbus_g_connection_register_g_object(daemon->dbus_conn,
                                        "/org/freedesktop/Notifications",
                                        G_OBJECT(daemon));

    return TRUE;
}

static gboolean
xfce_notify_daemon_load_config(XfceNotifyDaemon *daemon,
                               GError **error)
{
    gchar *theme;

    daemon->settings = xfconf_channel_new("xfce4-notifyd");

    daemon->expire_timeout = xfconf_channel_get_int(daemon->settings,
                                                    "/expire-timeout",
                                                    -1);
    if(daemon->expire_timeout != -1)
        daemon->expire_timeout *= 1000;

    daemon->fade_transparency = xfconf_channel_get_bool(daemon->settings,
                                                        "/fade-transparency",
                                                        TRUE);
    daemon->initial_opacity = xfconf_channel_get_double(daemon->settings,
                                                        "/initial-opacity",
                                                        0.9);

    theme = xfconf_channel_get_string(daemon->settings,
                                      "/theme", "Default");
    xfce_notify_daemon_set_theme(daemon, theme);
    g_free(theme);

    daemon->notify_location = xfconf_channel_get_uint(daemon->settings,
                                                      "/notify-location",
                                                      GTK_CORNER_TOP_RIGHT);

    g_signal_connect(G_OBJECT(daemon->settings), "property-changed",
                     G_CALLBACK(xfce_notify_daemon_settings_changed),
                     daemon);

    return TRUE;
}





XfceNotifyDaemon *
xfce_notify_daemon_new_unique(GError **error)
{
    XfceNotifyDaemon *daemon = g_object_new(XFCE_TYPE_NOTIFY_DAEMON, NULL);

    if(!xfce_notify_daemon_start(daemon, error)
       || !xfce_notify_daemon_load_config(daemon, error))
    {
        g_object_unref(G_OBJECT(daemon));
        return NULL;
    }

    return daemon;
}
