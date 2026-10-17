/*
 * sample_tools.c - an example tuxpad plugin.
 *
 * Demonstrates the plugin API: it adds several items to the Plugins menu that
 * operate on the active document. Build it with:
 *
 *     make plugins
 *
 * or by hand:
 *
 *     gcc -shared -fPIC -Iinclude plugins/sample_tools.c -o plugins/sample_tools.so \
 *         $(pkg-config --cflags --libs gtk+-3.0)
 */
#include "tuxpad-plugin.h"

static TuxpadApi *api;

static void to_upper(GtkMenuItem *mi, gpointer u)
{
    (void)mi; (void)u;
    char *sel = api->get_selection();
    if (!sel) { api->message("Uppercase: select some text first."); return; }
    char *up = g_utf8_strup(sel, -1);
    api->replace_selection(up);
    g_free(up); g_free(sel);
}

static void to_lower(GtkMenuItem *mi, gpointer u)
{
    (void)mi; (void)u;
    char *sel = api->get_selection();
    if (!sel) { api->message("Lowercase: select some text first."); return; }
    char *down = g_utf8_strdown(sel, -1);
    api->replace_selection(down);
    g_free(down); g_free(sel);
}

static void insert_datetime(GtkMenuItem *mi, gpointer u)
{
    (void)mi; (void)u;
    GDateTime *now = g_date_time_new_now_local();
    char *s = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    api->insert_at_cursor(s);
    g_free(s);
    g_date_time_unref(now);
}

static void word_count(GtkMenuItem *mi, gpointer u)
{
    (void)mi; (void)u;
    char *text = api->get_text();
    if (!text) { api->message("Word Count: no document."); return; }

    glong chars = g_utf8_strlen(text, -1);
    long lines = 1, words = 0;
    gboolean in_word = FALSE;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') lines++;
        if (g_ascii_isspace(*p)) in_word = FALSE;
        else if (!in_word) { in_word = TRUE; words++; }
    }
    g_free(text);

    GtkWidget *d = gtk_message_dialog_new(api->window, GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "Characters: %ld\nWords: %ld\nLines: %ld", chars, words, lines);
    gtk_window_set_title(GTK_WINDOW(d), "Word Count");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

int tuxpad_plugin_register(TuxpadApi *a)
{
    if (a->api_version != TUXPAD_PLUGIN_API_VERSION)
        return 1;
    api = a;
    a->add_menu_item(a, "To UPPERCASE",       G_CALLBACK(to_upper), NULL);
    a->add_menu_item(a, "to lowercase",       G_CALLBACK(to_lower), NULL);
    a->add_menu_item(a, "Insert Date & Time", G_CALLBACK(insert_datetime), NULL);
    a->add_menu_item(a, "Word Count…",        G_CALLBACK(word_count), NULL);
    return 0;
}
