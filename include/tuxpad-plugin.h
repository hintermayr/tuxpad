/*
 * tuxpad plugin API
 * =================
 *
 * A tuxpad plugin is a shared object (.so) that exports a single function:
 *
 *     int tuxpad_plugin_register(TuxpadApi *api);
 *
 * It returns 0 on success. During the call the plugin typically adds one or
 * more items to the Plugins menu with api->add_menu_item(). The callbacks it
 * registers may use the rest of the TuxpadApi to inspect and modify the
 * document that is currently active.
 *
 * Plugins are compiled against GTK+ 3, e.g.:
 *
 *     gcc -shared -fPIC myplugin.c -o myplugin.so \
 *         $(pkg-config --cflags --libs gtk+-3.0) -I/path/to/tuxpad/include
 *
 * and dropped into one of the plugin directories:
 *
 *     ./plugins/                         (next to the binary)
 *     ~/.config/tuxpad/plugins/
 *     <prefix>/lib/tuxpad/plugins/       (after "make install")
 */
#ifndef TUXPAD_PLUGIN_H
#define TUXPAD_PLUGIN_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define TUXPAD_PLUGIN_API_VERSION 1

typedef struct _TuxpadApi TuxpadApi;

struct _TuxpadApi {
    int        api_version;   /* == TUXPAD_PLUGIN_API_VERSION               */
    GtkWindow *window;        /* main application window                    */

    /* Add an item to the Plugins menu. */
    void (*add_menu_item)(TuxpadApi *api, const char *label,
                          GCallback callback, gpointer user_data);

    /* Active document accessors. Return NULL when no document is open. */
    GtkTextView   *(*active_view)(void);
    GtkTextBuffer *(*active_buffer)(void);

    /* Text helpers. Returned strings are owned by the caller (g_free). */
    char *(*get_text)(void);            /* entire buffer, or NULL           */
    char *(*get_selection)(void);       /* selected text, or NULL           */
    void  (*replace_selection)(const char *text);
    void  (*insert_at_cursor)(const char *text);
    const char *(*current_path)(void);  /* path of active file, or NULL     */

    /* Show a short message in the status bar. */
    void  (*message)(const char *text);
};

/* The symbol every plugin must export. */
typedef int (*TuxpadPluginRegisterFunc)(TuxpadApi *api);

G_END_DECLS

#endif /* TUXPAD_PLUGIN_H */
