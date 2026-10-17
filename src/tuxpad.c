/*
 * tuxpad - a Notepad++-style source code editor for Linux
 *
 * Built with GTK+ 3 and GtkSourceView 3.0.
 *
 * Features:
 *   - Tabbed, multi-document interface (reorderable tabs, close buttons)
 *   - Syntax highlighting with automatic language detection
 *   - Line numbers, current-line highlight, bracket matching, auto-indent
 *   - Find / Find Next / Find Previous / Replace (match case, whole word,
 *     regular expressions, wrap around) and Go to Line
 *   - Word wrap, zoom, color themes
 *   - Notepad++-style status bar (length/lines, Ln/Col/Sel, EOL, encoding)
 *   - Drag & drop files to open, full keyboard shortcuts
 *
 * Author:  Martin Hintermayr <mammutandroid@gmail.com>
 * License: MIT
 */

#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <gmodule.h>
#include <glib/gstdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>

#include "tuxpad-plugin.h"

#define APP_NAME "tuxpad"

/* ---- A single open document (one notebook tab) ----------------------- */
typedef struct {
    GtkWidget      *scrolled;      /* notebook page child                 */
    GtkWidget      *view;          /* GtkSourceView                       */
    GtkSourceBuffer*buffer;        /* its buffer                          */
    GtkWidget      *tab_label;     /* label shown on the tab              */
    char           *path;          /* full filesystem path or NULL        */
    gboolean        untitled;      /* never saved yet                     */
    GtkSourceSearchContext *search; /* lazily created search context      */
    GtkSourceCompletionWords *words; /* word-completion provider          */
} Document;

/* ---- Global application state ---------------------------------------- */
typedef struct {
    GtkWidget *window;
    GtkWidget *notebook;
    GtkWidget *lbl_len;            /* status bar segments                 */
    GtkWidget *lbl_pos;
    GtkWidget *lbl_sel;
    GtkWidget *lbl_eol;
    GtkWidget *lbl_enc;
    GtkWidget *lbl_lang;

    GtkSourceLanguageManager    *lm;
    GtkSourceStyleSchemeManager *ssm;
    GtkCssProvider              *font_provider;
    GtkAccelGroup               *accels;

    GtkSourceSearchSettings *search_settings;
    char      *scheme_id;          /* current style scheme id             */
    int        font_size;          /* editor font size in points          */
    int        untitled_count;
    gboolean   word_wrap;
    gboolean   show_line_numbers;
    gboolean   hover_tooltips;     /* show symbol info on mouse-over        */
    gboolean   autocomplete;       /* word auto-completion in the editor    */

    /* Find in Files results panel */
    GtkWidget     *results_panel;  /* bottom pane (hidden until first use) */
    GtkWidget     *results_tree;
    GtkTreeStore  *results_store;
    GtkWidget     *results_summary;

    /* Plugins */
    GtkWidget *plugins_menu;       /* submenu plugins add items to         */
    GList     *plugin_modules;     /* loaded GModule* list                 */
    TuxpadApi  api;                /* function table handed to plugins     */

    /* Recent files */
    GtkWidget *recent_menu;        /* "Open Recent" submenu                */
    GPtrArray *recent;             /* char* paths, most recent first       */
} App;

#define RECENT_MAX 15

static App app = {0};

/* Forward declarations */
static Document *document_new(const char *path);
static Document *current_document(void);
static void      update_status_bar(void);
static void      update_title(void);
static gboolean  save_document(Document *doc, gboolean save_as);
static void      apply_font_css(void);
static void      set_tab_title(Document *doc);
static void      open_path(const char *path);
static void      add_recent(const char *path);
static void      rebuild_recent_menu(void);
static void      save_session(void);
static gboolean  session_restoring = FALSE;
static gboolean  on_query_tooltip(GtkWidget *view, int x, int y, gboolean kbd,
                                  GtkTooltip *tip, gpointer u);
static gboolean  on_view_key(GtkWidget *view, GdkEventKey *ev, gpointer u);

/* ===================================================================== */
/*  Font / zoom                                                           */
/* ===================================================================== */

static void apply_font_css(void)
{
    char *css = g_strdup_printf(
        "textview.tuxpad-editor, textview.tuxpad-editor text {"
        "  font-family: monospace;"
        "  font-size: %dpt;"
        "}", app.font_size);
    gtk_css_provider_load_from_data(app.font_provider, css, -1, NULL);
    g_free(css);
}

/* ===================================================================== */
/*  Editor preferences applied to a freshly created view                  */
/* ===================================================================== */

static void configure_view(Document *doc)
{
    GtkSourceView *sv = GTK_SOURCE_VIEW(doc->view);

    gtk_source_view_set_show_line_numbers(sv, app.show_line_numbers);
    gtk_source_view_set_highlight_current_line(sv, TRUE);
    gtk_source_view_set_auto_indent(sv, TRUE);
    gtk_source_view_set_indent_on_tab(sv, TRUE);
    gtk_source_view_set_tab_width(sv, 4);
    gtk_source_view_set_insert_spaces_instead_of_tabs(sv, FALSE);
    gtk_source_view_set_smart_home_end(sv, GTK_SOURCE_SMART_HOME_END_BEFORE);
    gtk_source_view_set_show_right_margin(sv, FALSE);
    gtk_source_view_set_right_margin_position(sv, 80);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(doc->view),
                                app.word_wrap ? GTK_WRAP_WORD : GTK_WRAP_NONE);

    gtk_source_buffer_set_highlight_matching_brackets(doc->buffer, TRUE);
    gtk_source_buffer_set_highlight_syntax(doc->buffer, TRUE);

    /* hook font css */
    GtkStyleContext *ctx = gtk_widget_get_style_context(doc->view);
    gtk_style_context_add_class(ctx, "tuxpad-editor");
    gtk_style_context_add_provider(ctx, GTK_STYLE_PROVIDER(app.font_provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void apply_scheme(Document *doc)
{
    GtkSourceStyleScheme *scheme =
        gtk_source_style_scheme_manager_get_scheme(app.ssm, app.scheme_id);
    if (scheme)
        gtk_source_buffer_set_style_scheme(doc->buffer, scheme);
}

/* ===================================================================== */
/*  Tab helpers                                                           */
/* ===================================================================== */

static const char *document_basename(Document *doc)
{
    static char buf[256];
    if (doc->path) {
        char *b = g_path_get_basename(doc->path);
        g_strlcpy(buf, b, sizeof buf);
        g_free(b);
    } else {
        g_strlcpy(buf, "new", sizeof buf);
    }
    return buf;
}

static void set_tab_title(Document *doc)
{
    gboolean modified = gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(doc->buffer));
    char *title;
    if (doc->untitled) {
        gint page = gtk_notebook_page_num(GTK_NOTEBOOK(app.notebook), doc->scrolled);
        title = g_strdup_printf("%snew %d", modified ? "*" : "", page + 1);
    } else {
        char *b = g_path_get_basename(doc->path);
        title = g_strdup_printf("%s%s", modified ? "*" : "", b);
        g_free(b);
    }
    gtk_label_set_text(GTK_LABEL(doc->tab_label), title);
    g_free(title);
}

static Document *current_document(void)
{
    gint page = gtk_notebook_get_current_page(GTK_NOTEBOOK(app.notebook));
    if (page < 0) return NULL;
    GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.notebook), page);
    return g_object_get_data(G_OBJECT(child), "document");
}

/* ===================================================================== */
/*  Status bar + window title                                             */
/* ===================================================================== */

static void update_status_bar(void)
{
    Document *doc = current_document();
    if (!doc) {
        gtk_label_set_text(GTK_LABEL(app.lbl_len), "");
        gtk_label_set_text(GTK_LABEL(app.lbl_pos), "");
        gtk_label_set_text(GTK_LABEL(app.lbl_sel), "");
        gtk_label_set_text(GTK_LABEL(app.lbl_lang), "");
        return;
    }
    GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);

    gint chars = gtk_text_buffer_get_char_count(b);
    gint lines = gtk_text_buffer_get_line_count(b);
    char *s = g_strdup_printf("length: %d  lines: %d", chars, lines);
    gtk_label_set_text(GTK_LABEL(app.lbl_len), s);
    g_free(s);

    GtkTextIter cur;
    gtk_text_buffer_get_iter_at_mark(b, &cur, gtk_text_buffer_get_insert(b));
    gint line = gtk_text_iter_get_line(&cur) + 1;
    gint col  = gtk_text_iter_get_line_offset(&cur) + 1;
    s = g_strdup_printf("Ln : %d    Col : %d", line, col);
    gtk_label_set_text(GTK_LABEL(app.lbl_pos), s);
    g_free(s);

    GtkTextIter start, end;
    gint sel = 0;
    if (gtk_text_buffer_get_selection_bounds(b, &start, &end))
        sel = gtk_text_iter_get_offset(&end) - gtk_text_iter_get_offset(&start);
    s = g_strdup_printf("Sel : %d", sel);
    gtk_label_set_text(GTK_LABEL(app.lbl_sel), s);
    g_free(s);

    GtkSourceLanguage *lang = gtk_source_buffer_get_language(doc->buffer);
    gtk_label_set_text(GTK_LABEL(app.lbl_lang),
                       lang ? gtk_source_language_get_name(lang) : "Normal text");
}

static void update_title(void)
{
    Document *doc = current_document();
    if (!doc) {
        gtk_window_set_title(GTK_WINDOW(app.window), APP_NAME);
        return;
    }
    gboolean modified = gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(doc->buffer));
    char *title = g_strdup_printf("%s%s - %s",
                                  modified ? "*" : "",
                                  doc->path ? doc->path : document_basename(doc),
                                  APP_NAME);
    gtk_window_set_title(GTK_WINDOW(app.window), title);
    g_free(title);
}

/* ===================================================================== */
/*  Buffer / view signal callbacks                                        */
/* ===================================================================== */

static void on_cursor_moved(GObject *obj, GParamSpec *p, gpointer data)
{
    (void)obj; (void)p; (void)data;
    update_status_bar();
}

static void on_buffer_changed(GtkTextBuffer *b, gpointer data)
{
    (void)b; (void)data;
    update_status_bar();
}

static void on_modified_changed(GtkTextBuffer *b, gpointer data)
{
    (void)b;
    Document *doc = data;
    set_tab_title(doc);
    if (doc == current_document())
        update_title();
}

static void on_page_switched(GtkNotebook *nb, GtkWidget *page, guint num, gpointer d)
{
    (void)nb; (void)page; (void)num; (void)d;
    update_status_bar();
    update_title();
}

/* ===================================================================== */
/*  Creating / closing documents                                          */
/* ===================================================================== */

static GtkWidget *make_tab_widget(Document *doc);

static Document *document_new(const char *path)
{
    Document *doc = g_new0(Document, 1);
    doc->buffer = gtk_source_buffer_new(NULL);
    doc->view   = gtk_source_view_new_with_buffer(doc->buffer);
    doc->scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(doc->scrolled), doc->view);

    configure_view(doc);
    apply_scheme(doc);

    doc->untitled = (path == NULL);
    if (path) doc->path = g_strdup(path);

    g_object_set_data_full(G_OBJECT(doc->scrolled), "document", doc, g_free);

    doc->tab_label = gtk_label_new(NULL);
    GtkWidget *tab = make_tab_widget(doc);

    gint page = gtk_notebook_append_page(GTK_NOTEBOOK(app.notebook),
                                         doc->scrolled, tab);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(app.notebook),
                                     doc->scrolled, TRUE);
    set_tab_title(doc);

    GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);
    g_signal_connect(b, "notify::cursor-position",
                     G_CALLBACK(on_cursor_moved), NULL);
    g_signal_connect(b, "changed", G_CALLBACK(on_buffer_changed), NULL);
    g_signal_connect(b, "modified-changed",
                     G_CALLBACK(on_modified_changed), doc);
    g_signal_connect(doc->view, "notify::has-focus",
                     G_CALLBACK(on_cursor_moved), NULL);

    gtk_widget_set_has_tooltip(doc->view, app.hover_tooltips);
    g_signal_connect(doc->view, "query-tooltip",
                     G_CALLBACK(on_query_tooltip), NULL);
    g_signal_connect(doc->view, "key-press-event",
                     G_CALLBACK(on_view_key), NULL);

    /* word auto-completion from the document's own text */
    doc->words = gtk_source_completion_words_new("Document", NULL);
    gtk_source_completion_words_register(doc->words,
                                         GTK_TEXT_BUFFER(doc->buffer));
    if (app.autocomplete) {
        GtkSourceCompletion *comp =
            gtk_source_view_get_completion(GTK_SOURCE_VIEW(doc->view));
        gtk_source_completion_add_provider(comp,
            GTK_SOURCE_COMPLETION_PROVIDER(doc->words), NULL);
    }

    gtk_widget_show_all(doc->scrolled);
    gtk_widget_show_all(tab);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app.notebook), page);
    gtk_widget_grab_focus(doc->view);
    return doc;
}

/* Ask to save a modified document. Returns TRUE to proceed, FALSE to cancel. */
static gboolean maybe_save(Document *doc)
{
    if (!gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(doc->buffer)))
        return TRUE;

    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(app.window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
        "Save changes to \"%s\" before closing?", document_basename(doc));
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
                           "Don't Save", GTK_RESPONSE_NO,
                           "Cancel",     GTK_RESPONSE_CANCEL,
                           "Save",       GTK_RESPONSE_YES, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_YES);
    gint r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    if (r == GTK_RESPONSE_CANCEL) return FALSE;
    if (r == GTK_RESPONSE_NO)     return TRUE;
    return save_document(doc, FALSE);
}

static void close_document(Document *doc)
{
    if (!maybe_save(doc)) return;
    gint page = gtk_notebook_page_num(GTK_NOTEBOOK(app.notebook), doc->scrolled);
    if (page >= 0)
        gtk_notebook_remove_page(GTK_NOTEBOOK(app.notebook), page);
    /* doc freed via g_object_set_data_full destroy notify */
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.notebook)) == 0)
        document_new(NULL);
    if (!session_restoring) save_session();
}

static void on_tab_close_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    close_document((Document *)data);
}

static GtkWidget *make_tab_widget(Document *doc)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *btn = gtk_button_new();
    GtkWidget *img = gtk_image_new_from_icon_name("window-close",
                                                  GTK_ICON_SIZE_MENU);
    gtk_button_set_image(GTK_BUTTON(btn), img);
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(btn, FALSE);
    gtk_widget_set_name(btn, "tab-close-button");

    gtk_box_pack_start(GTK_BOX(box), doc->tab_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_tab_close_clicked), doc);
    return box;
}

/* ===================================================================== */
/*  Loading & saving files                                                */
/* ===================================================================== */

/* Pick a syntax-highlighting language for the document.
 *
 * First try the filename (extension). If that yields nothing — e.g. a file
 * with no extension such as "Makefile" already works, but something like a
 * shell script named "build" would not — fall back to content sniffing: ask
 * GIO to guess a MIME type from the first bytes of the buffer and let
 * GtkSourceView match a language to that content type. */
static void guess_language(Document *doc)
{
    GtkSourceLanguage *lang =
        gtk_source_language_manager_guess_language(app.lm, doc->path, NULL);

    if (!lang) {
        GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);
        GtkTextIter s, e;
        gtk_text_buffer_get_start_iter(b, &s);
        e = s;
        gtk_text_iter_forward_chars(&e, 512);
        char *sample = gtk_text_buffer_get_text(b, &s, &e, FALSE);
        if (sample && *sample) {
            gboolean uncertain = FALSE;
            char *ctype = g_content_type_guess(doc->path, (const guchar *)sample,
                                               strlen(sample), &uncertain);
            if (ctype) {
                lang = gtk_source_language_manager_guess_language(
                           app.lm, doc->path, ctype);
                g_free(ctype);
            }
        }
        g_free(sample);
    }

    gtk_source_buffer_set_language(doc->buffer, lang);
}

static void error_dialog(const char *fmt, const char *arg)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app.window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, fmt, arg);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void open_path(const char *path)
{
    /* If already open, just focus it */
    gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.notebook));
    for (gint i = 0; i < n; i++) {
        GtkWidget *ch = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.notebook), i);
        Document *d = g_object_get_data(G_OBJECT(ch), "document");
        if (d && d->path && g_strcmp0(d->path, path) == 0) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(app.notebook), i);
            return;
        }
    }

    char   *contents = NULL;
    gsize   length   = 0;
    GError *err      = NULL;
    if (!g_file_get_contents(path, &contents, &length, &err)) {
        error_dialog("Could not open file:\n%s", err->message);
        g_error_free(err);
        return;
    }

    /* Convert to UTF-8 if needed */
    char *utf8 = NULL;
    if (g_utf8_validate(contents, length, NULL)) {
        utf8 = g_strndup(contents, length);
    } else {
        utf8 = g_convert(contents, length, "UTF-8", "ISO-8859-1",
                         NULL, NULL, NULL);
        if (!utf8) utf8 = g_strndup(contents, length);
    }
    g_free(contents);

    /* Reuse a pristine untitled tab if present */
    Document *doc = current_document();
    gboolean reuse = doc && doc->untitled && doc->path == NULL &&
                     !gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(doc->buffer)) &&
                     gtk_text_buffer_get_char_count(GTK_TEXT_BUFFER(doc->buffer)) == 0;
    if (reuse) {
        doc->path = g_strdup(path);
        doc->untitled = FALSE;
    } else {
        doc = document_new(path);
    }

    GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);
    gtk_source_buffer_begin_not_undoable_action(doc->buffer);
    gtk_text_buffer_set_text(b, utf8, -1);
    gtk_source_buffer_end_not_undoable_action(doc->buffer);
    g_free(utf8);

    GtkTextIter start;
    gtk_text_buffer_get_start_iter(b, &start);
    gtk_text_buffer_place_cursor(b, &start);
    gtk_text_buffer_set_modified(b, FALSE);

    guess_language(doc);
    set_tab_title(doc);
    update_title();
    update_status_bar();
    add_recent(path);
    if (!session_restoring) save_session();
}

static gboolean save_document(Document *doc, gboolean save_as)
{
    char *path = NULL;

    if (doc->path && !save_as) {
        path = g_strdup(doc->path);
    } else {
        GtkWidget *dlg = gtk_file_chooser_dialog_new(
            "Save File", GTK_WINDOW(app.window),
            GTK_FILE_CHOOSER_ACTION_SAVE,
            "Cancel", GTK_RESPONSE_CANCEL,
            "Save",   GTK_RESPONSE_ACCEPT, NULL);
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
        if (doc->path)
            gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), doc->path);
        else
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg),
                                              document_basename(doc));
        if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
            path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_widget_destroy(dlg);
        if (!path) return FALSE;
    }

    GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char *text = gtk_text_buffer_get_text(b, &s, &e, FALSE);

    GError *err = NULL;
    gboolean ok = g_file_set_contents(path, text, -1, &err);
    g_free(text);

    if (!ok) {
        error_dialog("Could not save file:\n%s", err->message);
        g_error_free(err);
        g_free(path);
        return FALSE;
    }

    g_free(doc->path);
    doc->path = path;
    doc->untitled = FALSE;
    gtk_text_buffer_set_modified(b, FALSE);
    guess_language(doc);
    set_tab_title(doc);
    update_title();
    update_status_bar();
    add_recent(path);
    if (!session_restoring) save_session();
    return TRUE;
}

/* ===================================================================== */
/*  Recent files & session                                                */
/* ===================================================================== */

/* Path to a file under ~/.config/tuxpad/, creating the directory. */
static char *config_path(const char *name)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "tuxpad", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *p = g_build_filename(dir, name, NULL);
    g_free(dir);
    return p;
}

static void save_recent(void)
{
    if (!app.recent) return;
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < app.recent->len; i++)
        g_string_append_printf(s, "%s\n", (char *)g_ptr_array_index(app.recent, i));
    char *p = config_path("recent");
    g_file_set_contents(p, s->str, -1, NULL);
    g_free(p);
    g_string_free(s, TRUE);
}

static void load_recent(void)
{
    app.recent = g_ptr_array_new_with_free_func(g_free);
    char *p = config_path("recent");
    char *data = NULL;
    if (g_file_get_contents(p, &data, NULL, NULL)) {
        char **lines = g_strsplit(data, "\n", -1);
        for (int i = 0; lines[i] && app.recent->len < RECENT_MAX; i++)
            if (*lines[i]) g_ptr_array_add(app.recent, g_strdup(lines[i]));
        g_strfreev(lines);
        g_free(data);
    }
    g_free(p);
}

static void on_recent_clicked(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    char *path = data;
    if (g_file_test(path, G_FILE_TEST_EXISTS))
        open_path(path);
    else {
        error_dialog("File no longer exists:\n%s", path);
        /* drop it from the list */
        for (guint i = 0; i < app.recent->len; i++)
            if (!g_strcmp0(g_ptr_array_index(app.recent, i), path)) {
                g_ptr_array_remove_index(app.recent, i); break;
            }
        save_recent();
        rebuild_recent_menu();
    }
}

static void on_recent_clear(GtkMenuItem *mi, gpointer data)
{
    (void)mi;(void)data;
    g_ptr_array_set_size(app.recent, 0);
    save_recent();
    rebuild_recent_menu();
}

static void rebuild_recent_menu(void)
{
    if (!app.recent_menu) return;
    gtk_container_foreach(GTK_CONTAINER(app.recent_menu),
                          (GtkCallback)gtk_widget_destroy, NULL);
    if (!app.recent || app.recent->len == 0) {
        GtkWidget *mi = gtk_menu_item_new_with_label("(no recent files)");
        gtk_widget_set_sensitive(mi, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(app.recent_menu), mi);
    } else {
        for (guint i = 0; i < app.recent->len; i++) {
            char *path = g_ptr_array_index(app.recent, i);
            char *base = g_path_get_basename(path);
            char *label = g_strdup_printf("%u  %s", i + 1, base);
            GtkWidget *mi = gtk_menu_item_new_with_label(label);
            gtk_widget_set_tooltip_text(mi, path);
            g_signal_connect(mi, "activate",
                             G_CALLBACK(on_recent_clicked), path);
            gtk_menu_shell_append(GTK_MENU_SHELL(app.recent_menu), mi);
            g_free(base); g_free(label);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(app.recent_menu),
                              gtk_separator_menu_item_new());
        GtkWidget *clr = gtk_menu_item_new_with_label("Clear Recent List");
        g_signal_connect(clr, "activate", G_CALLBACK(on_recent_clear), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(app.recent_menu), clr);
    }
    gtk_widget_show_all(app.recent_menu);
}

static void add_recent(const char *path)
{
    if (!path || !app.recent) return;
    char *abs = g_canonicalize_filename(path, NULL);
    /* remove existing occurrence */
    for (guint i = 0; i < app.recent->len; i++)
        if (!g_strcmp0(g_ptr_array_index(app.recent, i), abs)) {
            g_ptr_array_remove_index(app.recent, i); break;
        }
    g_ptr_array_insert(app.recent, 0, abs);        /* most recent first */
    while (app.recent->len > RECENT_MAX)
        g_ptr_array_remove_index(app.recent, app.recent->len - 1);
    save_recent();
    rebuild_recent_menu();
}

/* Persist the list of open file paths, and restore them on next launch. */
static void save_session(void)
{
    GString *s = g_string_new(NULL);
    gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.notebook));
    for (gint i = 0; i < n; i++) {
        GtkWidget *ch = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.notebook), i);
        Document *dc = g_object_get_data(G_OBJECT(ch), "document");
        if (dc && dc->path) {
            char *abs = g_canonicalize_filename(dc->path, NULL);
            g_string_append_printf(s, "%s\n", abs);
            g_free(abs);
        }
    }
    char *p = config_path("session");
    g_file_set_contents(p, s->str, -1, NULL);
    g_free(p);
    g_string_free(s, TRUE);
}

static int restore_session(void)
{
    char *p = config_path("session");
    char *data = NULL;
    int opened = 0;
    session_restoring = TRUE;
    if (g_file_get_contents(p, &data, NULL, NULL)) {
        char **lines = g_strsplit(data, "\n", -1);
        for (int i = 0; lines[i]; i++)
            if (*lines[i] && g_file_test(lines[i], G_FILE_TEST_EXISTS)) {
                open_path(lines[i]); opened++;
            }
        g_strfreev(lines);
        g_free(data);
    }
    g_free(p);
    session_restoring = FALSE;
    return opened;
}

/* ===================================================================== */
/*  Menu action callbacks                                                 */
/* ===================================================================== */

static void act_new(GtkWidget *w, gpointer d)   { (void)w;(void)d; document_new(NULL); }

static void act_open(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Open File", GTK_WINDOW(app.window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Open",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dlg));
        for (GSList *l = files; l; l = l->next) {
            open_path((char *)l->data);
            g_free(l->data);
        }
        g_slist_free(files);
    }
    gtk_widget_destroy(dlg);
}

static void act_save(GtkWidget *w, gpointer d)
{ (void)w;(void)d; Document *doc = current_document(); if (doc) save_document(doc, FALSE); }

static void act_save_as(GtkWidget *w, gpointer d)
{ (void)w;(void)d; Document *doc = current_document(); if (doc) save_document(doc, TRUE); }

static void act_save_all(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.notebook));
    for (gint i = 0; i < n; i++) {
        GtkWidget *ch = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.notebook), i);
        Document *doc = g_object_get_data(G_OBJECT(ch), "document");
        if (doc && gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(doc->buffer)))
            save_document(doc, FALSE);
    }
}

static void act_close(GtkWidget *w, gpointer d)
{ (void)w;(void)d; Document *doc = current_document(); if (doc) close_document(doc); }

static void act_close_all(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    while (gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.notebook)) > 0) {
        GtkWidget *ch = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.notebook), 0);
        Document *doc = g_object_get_data(G_OBJECT(ch), "document");
        gint before = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.notebook));
        close_document(doc);
        /* if user cancelled, stop */
        if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.notebook)) >= before)
            break;
    }
}

static gboolean confirm_quit(void)
{
    gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.notebook));
    for (gint i = 0; i < n; i++) {
        GtkWidget *ch = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.notebook), i);
        Document *doc = g_object_get_data(G_OBJECT(ch), "document");
        if (doc && gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(doc->buffer))) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(app.notebook), i);
            if (!maybe_save(doc)) return FALSE;
        }
    }
    return TRUE;
}

static void act_quit(GtkWidget *w, gpointer d)
{ (void)w;(void)d; if (confirm_quit()) { save_session(); gtk_main_quit(); } }

static void act_print(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    Document *doc = current_document();
    if (!doc) return;
    GtkSourcePrintCompositor *comp =
        gtk_source_print_compositor_new_from_view(GTK_SOURCE_VIEW(doc->view));
    gtk_source_print_compositor_set_print_line_numbers(comp, 1);
    GtkPrintOperation *op = gtk_print_operation_new();

    /* run pagination + draw via the compositor */
    g_object_set_data_full(G_OBJECT(op), "compositor", comp, g_object_unref);

    /* Simple synchronous print using the compositor's helpers */
    void begin_print(GtkPrintOperation *o, GtkPrintContext *c, gpointer u);
    void draw_page (GtkPrintOperation *o, GtkPrintContext *c, gint p, gpointer u);
    g_signal_connect(op, "begin-print", G_CALLBACK(begin_print), comp);
    g_signal_connect(op, "draw-page",   G_CALLBACK(draw_page), comp);

    gtk_print_operation_run(op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                            GTK_WINDOW(app.window), NULL);
    g_object_unref(op);
}

void begin_print(GtkPrintOperation *op, GtkPrintContext *ctx, gpointer u)
{
    GtkSourcePrintCompositor *comp = u;
    while (!gtk_source_print_compositor_paginate(comp, ctx))
        ;
    gtk_print_operation_set_n_pages(op,
        gtk_source_print_compositor_get_n_pages(comp));
}

void draw_page(GtkPrintOperation *op, GtkPrintContext *ctx, gint page, gpointer u)
{
    (void)op;
    gtk_source_print_compositor_draw_page(u, ctx, page);
}

/* ---- Edit actions ---- */
static void act_undo(GtkWidget *w, gpointer d)
{ (void)w;(void)d; Document *doc = current_document();
  if (doc && gtk_source_buffer_can_undo(doc->buffer)) gtk_source_buffer_undo(doc->buffer); }
static void act_redo(GtkWidget *w, gpointer d)
{ (void)w;(void)d; Document *doc = current_document();
  if (doc && gtk_source_buffer_can_redo(doc->buffer)) gtk_source_buffer_redo(doc->buffer); }
static void act_cut(GtkWidget *w, gpointer d)
{ (void)w;(void)d; Document *doc = current_document(); if (doc)
  g_signal_emit_by_name(doc->view, "cut-clipboard"); }
static void act_copy(GtkWidget *w, gpointer d)
{ (void)w;(void)d; Document *doc = current_document(); if (doc)
  g_signal_emit_by_name(doc->view, "copy-clipboard"); }
static void act_paste(GtkWidget *w, gpointer d)
{ (void)w;(void)d; Document *doc = current_document(); if (doc)
  g_signal_emit_by_name(doc->view, "paste-clipboard"); }
static void act_delete(GtkWidget *w, gpointer d)
{ (void)w;(void)d; Document *doc = current_document(); if (doc)
  gtk_text_buffer_delete_selection(GTK_TEXT_BUFFER(doc->buffer), TRUE, TRUE); }
static void act_select_all(GtkWidget *w, gpointer d)
{ (void)w;(void)d; Document *doc = current_document(); if (!doc) return;
  GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer); GtkTextIter s,e;
  gtk_text_buffer_get_bounds(b,&s,&e); gtk_text_buffer_select_range(b,&s,&e); }

/* ===================================================================== */
/*  Search context helpers                                                */
/* ===================================================================== */

static GtkSourceSearchContext *doc_search(Document *doc)
{
    if (!doc->search) {
        doc->search = gtk_source_search_context_new(doc->buffer,
                                                    app.search_settings);
        gtk_source_search_context_set_highlight(doc->search, TRUE);
    }
    return doc->search;
}

static void do_find(gboolean backward)
{
    Document *doc = current_document();
    if (!doc) return;
    const char *text =
        gtk_source_search_settings_get_search_text(app.search_settings);
    if (!text || !*text) return;

    GtkSourceSearchContext *sc = doc_search(doc);
    GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);
    GtkTextIter sel_start, sel_end, ms, me;
    gboolean wrapped = FALSE, found;

    gtk_text_buffer_get_selection_bounds(b, &sel_start, &sel_end);
    if (backward)
        found = gtk_source_search_context_backward2(sc, &sel_start, &ms, &me, &wrapped);
    else
        found = gtk_source_search_context_forward2(sc, &sel_end, &ms, &me, &wrapped);

    if (found) {
        gtk_text_buffer_select_range(b, &ms, &me);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(doc->view),
            gtk_text_buffer_get_insert(b), 0.1, FALSE, 0, 0);
    } else {
        gdk_display_beep(gtk_widget_get_display(app.window));
    }
}

/* ---- The combined Find / Replace dialog ---- */
static void toggle_setting(GtkToggleButton *t, gpointer data)
{
    const char *which = data;
    gboolean on = gtk_toggle_button_get_active(t);
    if (!strcmp(which, "case"))
        gtk_source_search_settings_set_case_sensitive(app.search_settings, on);
    else if (!strcmp(which, "word"))
        gtk_source_search_settings_set_at_word_boundaries(app.search_settings, on);
    else if (!strcmp(which, "regex"))
        gtk_source_search_settings_set_regex_enabled(app.search_settings, on);
    else if (!strcmp(which, "wrap"))
        gtk_source_search_settings_set_wrap_around(app.search_settings, on);
}

static void show_find_replace(gboolean replace_mode)
{
    static GtkWidget *dlg = NULL;
    static GtkWidget *find_entry, *repl_entry, *repl_row;

    if (!dlg) {
        dlg = gtk_dialog_new_with_buttons(
            "Find / Replace", GTK_WINDOW(app.window),
            GTK_DIALOG_DESTROY_WITH_PARENT, NULL, NULL);
        gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
        g_signal_connect(dlg, "delete-event",
                         G_CALLBACK(gtk_widget_hide_on_delete), NULL);

        GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
        gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
        gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

        gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Find:"), 0, 0, 1, 1);
        find_entry = gtk_entry_new();
        gtk_entry_set_activates_default(GTK_ENTRY(find_entry), TRUE);
        gtk_widget_set_hexpand(find_entry, TRUE);
        gtk_grid_attach(GTK_GRID(grid), find_entry, 1, 0, 3, 1);

        repl_row = gtk_label_new("Replace:");
        gtk_grid_attach(GTK_GRID(grid), repl_row, 0, 1, 1, 1);
        repl_entry = gtk_entry_new();
        gtk_widget_set_hexpand(repl_entry, TRUE);
        gtk_grid_attach(GTK_GRID(grid), repl_entry, 1, 1, 3, 1);

        GtkWidget *c1 = gtk_check_button_new_with_label("Match case");
        GtkWidget *c2 = gtk_check_button_new_with_label("Whole word");
        GtkWidget *c3 = gtk_check_button_new_with_label("Regular expression");
        GtkWidget *c4 = gtk_check_button_new_with_label("Wrap around");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c4), TRUE);
        gtk_source_search_settings_set_wrap_around(app.search_settings, TRUE);
        g_signal_connect(c1, "toggled", G_CALLBACK(toggle_setting), "case");
        g_signal_connect(c2, "toggled", G_CALLBACK(toggle_setting), "word");
        g_signal_connect(c3, "toggled", G_CALLBACK(toggle_setting), "regex");
        g_signal_connect(c4, "toggled", G_CALLBACK(toggle_setting), "wrap");
        gtk_grid_attach(GTK_GRID(grid), c1, 0, 2, 2, 1);
        gtk_grid_attach(GTK_GRID(grid), c2, 2, 2, 2, 1);
        gtk_grid_attach(GTK_GRID(grid), c3, 0, 3, 2, 1);
        gtk_grid_attach(GTK_GRID(grid), c4, 2, 3, 2, 1);

        gtk_dialog_add_button(GTK_DIALOG(dlg), "Find Next",    101);
        gtk_dialog_add_button(GTK_DIALOG(dlg), "Find Prev",    102);
        gtk_dialog_add_button(GTK_DIALOG(dlg), "Replace",      103);
        gtk_dialog_add_button(GTK_DIALOG(dlg), "Replace All",  104);
        gtk_dialog_add_button(GTK_DIALOG(dlg), "Close",        GTK_RESPONSE_CLOSE);
        gtk_dialog_set_default_response(GTK_DIALOG(dlg), 101);

        g_object_set_data(G_OBJECT(dlg), "find",  find_entry);
        g_object_set_data(G_OBJECT(dlg), "repl",  repl_entry);
        g_object_set_data(G_OBJECT(dlg), "rrow",  repl_row);
    }

    find_entry = g_object_get_data(G_OBJECT(dlg), "find");
    repl_entry = g_object_get_data(G_OBJECT(dlg), "repl");

    /* Prefill with current selection */
    Document *doc = current_document();
    if (doc) {
        GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);
        GtkTextIter s, e;
        if (gtk_text_buffer_get_selection_bounds(b, &s, &e)) {
            char *sel = gtk_text_buffer_get_text(b, &s, &e, FALSE);
            if (sel && !strchr(sel, '\n'))
                gtk_entry_set_text(GTK_ENTRY(find_entry), sel);
            g_free(sel);
        }
    }

    gtk_widget_show_all(dlg);
    gtk_widget_set_visible(repl_entry, replace_mode);
    gtk_widget_set_visible(g_object_get_data(G_OBJECT(dlg), "rrow"), replace_mode);
    gtk_widget_grab_focus(find_entry);

    gint resp;
    while ((resp = gtk_dialog_run(GTK_DIALOG(dlg))) != GTK_RESPONSE_CLOSE &&
            resp != GTK_RESPONSE_DELETE_EVENT) {
        gtk_source_search_settings_set_search_text(app.search_settings,
            gtk_entry_get_text(GTK_ENTRY(find_entry)));
        doc = current_document();
        if (!doc) continue;
        GtkSourceSearchContext *sc = doc_search(doc);
        GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);
        const char *rep = gtk_entry_get_text(GTK_ENTRY(repl_entry));

        if (resp == 101) do_find(FALSE);
        else if (resp == 102) do_find(TRUE);
        else if (resp == 103) {
            GtkTextIter s, e;
            if (gtk_text_buffer_get_selection_bounds(b, &s, &e)) {
                GError *err = NULL;
                gtk_source_search_context_replace2(sc, &s, &e, rep, -1, &err);
                if (err) g_error_free(err);
            }
            do_find(FALSE);
        } else if (resp == 104) {
            GError *err = NULL;
            gint count = gtk_source_search_context_replace_all(sc, rep, -1, &err);
            if (err) g_error_free(err);
            char *msg = g_strdup_printf("%d occurrence(s) replaced.", count);
            gtk_label_set_text(GTK_LABEL(app.lbl_len), msg);
            g_free(msg);
        }
    }
    gtk_widget_hide(dlg);
}

static void act_find(GtkWidget *w, gpointer d)    { (void)w;(void)d; show_find_replace(FALSE); }
static void act_replace(GtkWidget *w, gpointer d) { (void)w;(void)d; show_find_replace(TRUE); }
static void act_find_next(GtkWidget *w, gpointer d){ (void)w;(void)d; do_find(FALSE); }
static void act_find_prev(GtkWidget *w, gpointer d){ (void)w;(void)d; do_find(TRUE); }

static void act_goto(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    Document *doc = current_document();
    if (!doc) return;
    GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);
    gint lines = gtk_text_buffer_get_line_count(b);

    GtkWidget *dlg = gtk_dialog_new_with_buttons("Go to Line",
        GTK_WINDOW(app.window), GTK_DIALOG_MODAL,
        "Cancel", GTK_RESPONSE_CANCEL, "Go", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(hb), 10);
    char *lbl = g_strdup_printf("Line (1 - %d):", lines);
    gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(lbl), FALSE, FALSE, 0);
    g_free(lbl);
    GtkWidget *spin = gtk_spin_button_new_with_range(1, lines, 1);
    gtk_entry_set_activates_default(GTK_ENTRY(spin), TRUE);
    gtk_box_pack_start(GTK_BOX(hb), spin, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), hb, TRUE, TRUE, 0);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gint line = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin)) - 1;
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(b, &it, line);
        gtk_text_buffer_place_cursor(b, &it);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(doc->view),
            gtk_text_buffer_get_insert(b), 0.25, TRUE, 0, 0.5);
        gtk_widget_grab_focus(doc->view);
    }
    gtk_widget_destroy(dlg);
}

/* ===================================================================== */
/*  Find in Files                                                         */
/* ===================================================================== */

/* Open a file (or focus it if already open) and jump to a 1-based line. */
static void open_at_line(const char *path, int line)
{
    open_path(path);
    Document *doc = current_document();
    if (!doc) return;
    GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(b, &it, line > 0 ? line - 1 : 0);
    gtk_text_buffer_place_cursor(b, &it);
    GtkTextIter eol = it;
    if (!gtk_text_iter_ends_line(&eol))
        gtk_text_iter_forward_to_line_end(&eol);
    gtk_text_buffer_select_range(b, &it, &eol);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(doc->view),
        gtk_text_buffer_get_insert(b), 0.25, TRUE, 0, 0.5);
    gtk_widget_grab_focus(doc->view);
}

/* Jump to a 1-based line within the already-active document. */
static void goto_line_current(int line)
{
    Document *doc = current_document();
    if (!doc || line <= 0) return;
    GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(b, &it, line - 1);
    gtk_text_buffer_place_cursor(b, &it);
    GtkTextIter eol = it;
    if (!gtk_text_iter_ends_line(&eol))
        gtk_text_iter_forward_to_line_end(&eol);
    gtk_text_buffer_select_range(b, &it, &eol);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(doc->view),
        gtk_text_buffer_get_insert(b), 0.25, TRUE, 0, 0.5);
    gtk_widget_grab_focus(doc->view);
}

static void on_result_activated(GtkTreeView *tv, GtkTreePath *tp,
                                GtkTreeViewColumn *col, gpointer u)
{
    (void)col; (void)u;
    GtkTreeModel *model = gtk_tree_view_get_model(tv);
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(model, &it, tp)) return;
    char *path = NULL;
    int line = 0;
    gtk_tree_model_get(model, &it, 1, &path, 2, &line, -1);
    if (path && line > 0)
        open_at_line(path, line);     /* result from Find in Files */
    else if (line > 0)
        goto_line_current(line);      /* diagnostic in current doc */
    else                              /* a parent row: expand/collapse */
        gtk_tree_view_expand_row(tv, tp, FALSE);
    g_free(path);
}

static void results_panel_hide(GtkButton *b, gpointer u)
{ (void)b;(void)u; gtk_widget_hide(app.results_panel); }

/* Does `name` match any of the whitespace/;/,-separated glob patterns? */
static gboolean name_matches(const char *name, char **patterns)
{
    if (!patterns || !patterns[0]) return TRUE;
    for (int i = 0; patterns[i]; i++) {
        /* "*" and "*.*" mean "all files" (Notepad++ convention), even those
           without an extension. */
        if (!strcmp(patterns[i], "*") || !strcmp(patterns[i], "*.*"))
            return TRUE;
        if (g_pattern_match_simple(patterns[i], name))
            return TRUE;
    }
    return FALSE;
}

typedef struct {
    GRegex   *re;
    char    **patterns;
    gboolean  recursive;
    int       file_hits;    /* files with at least one match */
    int       total_hits;   /* total matching lines          */
    int       scanned;      /* files scanned                 */
} FifCtx;

static void fif_search_file(FifCtx *c, const char *path)
{
    char  *data = NULL;
    gsize  len  = 0;
    if (!g_file_get_contents(path, &data, &len, NULL)) return;
    c->scanned++;
    /* Skip binary/non-UTF-8 files */
    if (len == 0 || memchr(data, '\0', len) ||
        !g_utf8_validate(data, len, NULL)) { g_free(data); return; }

    GtkTreeIter file_iter;
    gboolean have_file_node = FALSE;
    int hits = 0;

    char **lines = g_strsplit(data, "\n", -1);
    for (int ln = 0; lines[ln]; ln++) {
        if (!g_regex_match(c->re, lines[ln], 0, NULL)) continue;
        if (!have_file_node) {
            gtk_tree_store_append(app.results_store, &file_iter, NULL);
            have_file_node = TRUE;
        }
        char *trimmed = g_strstrip(g_strdup(lines[ln]));
        char *label = g_strdup_printf("Line %d:  %s", ln + 1, trimmed);
        GtkTreeIter child;
        gtk_tree_store_append(app.results_store, &child, &file_iter);
        gtk_tree_store_set(app.results_store, &child,
                           0, label, 1, path, 2, ln + 1, -1);
        g_free(label); g_free(trimmed);
        hits++;
    }
    g_strfreev(lines);
    g_free(data);

    if (have_file_node) {
        char *b = g_path_get_basename(path);
        char *dir = g_path_get_dirname(path);
        char *label = g_strdup_printf("%s   (%d)  —  %s", b, hits, dir);
        gtk_tree_store_set(app.results_store, &file_iter,
                           0, label, 1, path, 2, 0, -1);
        g_free(label); g_free(b); g_free(dir);
        c->file_hits++;
        c->total_hits += hits;
    }
}

static void fif_walk(FifCtx *c, const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        char *full = g_build_filename(dir, name, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            if (c->recursive && name[0] != '.')   /* skip hidden dirs */
                fif_walk(c, full);
        } else if (name_matches(name, c->patterns)) {
            fif_search_file(c, full);
        }
        g_free(full);
    }
    g_dir_close(d);
}

static void do_find_in_files(const char *find_text, const char *dir,
                             const char *filters, gboolean recursive,
                             gboolean match_case, gboolean whole_word,
                             gboolean regex)
{
    if (!find_text || !*find_text || !dir || !*dir) return;

    /* Build one GRegex that honours every criterion. */
    char *pat = regex ? g_strdup(find_text)
                      : g_regex_escape_string(find_text, -1);
    if (whole_word) {
        char *w = g_strdup_printf("\\b%s\\b", pat);
        g_free(pat); pat = w;
    }
    GError *err = NULL;
    GRegex *re = g_regex_new(pat,
        (match_case ? 0 : G_REGEX_CASELESS) | G_REGEX_OPTIMIZE, 0, &err);
    g_free(pat);
    if (!re) {
        error_dialog("Invalid search pattern:\n%s", err->message);
        g_error_free(err);
        return;
    }

    char **patterns = NULL;
    if (filters && *filters) {
        char *norm = g_strdup(filters);
        for (char *p = norm; *p; p++) if (*p == ';' || *p == ',') *p = ' ';
        patterns = g_strsplit_set(norm, " \t", -1);
        /* drop empty tokens */
        int w = 0;
        for (int i = 0; patterns[i]; i++) {
            if (*patterns[i]) patterns[w++] = patterns[i];
            else g_free(patterns[i]);
        }
        patterns[w] = NULL;
        g_free(norm);
    }

    gtk_tree_store_clear(app.results_store);
    FifCtx c = { re, patterns, recursive, 0, 0, 0 };
    fif_walk(&c, dir);
    g_regex_unref(re);
    if (patterns) g_strfreev(patterns);

    char *summary = g_strdup_printf(
        "\"%s\" : %d hit(s) in %d file(s) of %d searched   (in %s%s)",
        find_text, c.total_hits, c.file_hits, c.scanned, dir,
        recursive ? ", recursive" : "");
    gtk_label_set_text(GTK_LABEL(app.results_summary), summary);
    gtk_label_set_text(GTK_LABEL(app.lbl_len), summary);
    g_free(summary);

    gtk_widget_show_all(app.results_panel);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(app.results_tree));
}

static void on_fif_browse(GtkButton *b, gpointer entry)
{
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Choose Directory",
        GTK_WINDOW(app.window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "Cancel", GTK_RESPONSE_CANCEL, "Select", GTK_RESPONSE_ACCEPT, NULL);
    (void)b;
    const char *cur = gtk_entry_get_text(GTK_ENTRY(entry));
    if (cur && *cur)
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), cur);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_entry_set_text(GTK_ENTRY(entry), f);
        g_free(f);
    }
    gtk_widget_destroy(dlg);
}

static void on_fif_response(GtkDialog *dlg, gint resp, gpointer u)
{
    (void)u;
    if (resp != 1) { gtk_widget_hide(GTK_WIDGET(dlg)); return; }
    GtkEntry *find = g_object_get_data(G_OBJECT(dlg), "find");
    GtkEntry *dir  = g_object_get_data(G_OBJECT(dlg), "dir");
    GtkEntry *filt = g_object_get_data(G_OBJECT(dlg), "filt");
    GtkToggleButton *c_case = g_object_get_data(G_OBJECT(dlg), "case");
    GtkToggleButton *c_word = g_object_get_data(G_OBJECT(dlg), "word");
    GtkToggleButton *c_rx   = g_object_get_data(G_OBJECT(dlg), "rx");
    GtkToggleButton *c_sub  = g_object_get_data(G_OBJECT(dlg), "sub");
    do_find_in_files(gtk_entry_get_text(find), gtk_entry_get_text(dir),
                     gtk_entry_get_text(filt),
                     gtk_toggle_button_get_active(c_sub),
                     gtk_toggle_button_get_active(c_case),
                     gtk_toggle_button_get_active(c_word),
                     gtk_toggle_button_get_active(c_rx));
}

static void act_find_in_files(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    static GtkWidget *dlg = NULL;
    if (!dlg) {
        dlg = gtk_dialog_new_with_buttons("Find in Files",
            GTK_WINDOW(app.window), GTK_DIALOG_DESTROY_WITH_PARENT, NULL, NULL);
        gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
        g_signal_connect(dlg, "delete-event",
                         G_CALLBACK(gtk_widget_hide_on_delete), NULL);

        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
        gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
                           grid, TRUE, TRUE, 0);

        GtkWidget *find = gtk_entry_new();
        gtk_entry_set_activates_default(GTK_ENTRY(find), TRUE);
        gtk_widget_set_hexpand(find, TRUE);
        gtk_widget_set_size_request(find, 320, -1);
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Find what:"), 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), find, 1, 0, 2, 1);

        GtkWidget *dir = gtk_entry_new();
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Directory:"), 0, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), dir, 1, 1, 1, 1);
        GtkWidget *browse = gtk_button_new_with_label("Browse…");
        g_signal_connect(browse, "clicked", G_CALLBACK(on_fif_browse), dir);
        gtk_grid_attach(GTK_GRID(grid), browse, 2, 1, 1, 1);

        GtkWidget *filt = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(filt), "*.*");
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Filters:"), 0, 2, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), filt, 1, 2, 2, 1);

        GtkWidget *c_case = gtk_check_button_new_with_label("Match case");
        GtkWidget *c_word = gtk_check_button_new_with_label("Whole word");
        GtkWidget *c_rx   = gtk_check_button_new_with_label("Regular expression");
        GtkWidget *c_sub  = gtk_check_button_new_with_label("In subdirectories");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c_sub), TRUE);
        gtk_grid_attach(GTK_GRID(grid), c_case, 0, 3, 2, 1);
        gtk_grid_attach(GTK_GRID(grid), c_word, 2, 3, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), c_rx,   0, 4, 2, 1);
        gtk_grid_attach(GTK_GRID(grid), c_sub,  2, 4, 1, 1);

        gtk_dialog_add_button(GTK_DIALOG(dlg), "Find All", 1);
        gtk_dialog_add_button(GTK_DIALOG(dlg), "Close", GTK_RESPONSE_CLOSE);
        gtk_dialog_set_default_response(GTK_DIALOG(dlg), 1);

        g_object_set_data(G_OBJECT(dlg), "find", find);
        g_object_set_data(G_OBJECT(dlg), "dir",  dir);
        g_object_set_data(G_OBJECT(dlg), "filt", filt);
        g_object_set_data(G_OBJECT(dlg), "case", c_case);
        g_object_set_data(G_OBJECT(dlg), "word", c_word);
        g_object_set_data(G_OBJECT(dlg), "rx",   c_rx);
        g_object_set_data(G_OBJECT(dlg), "sub",  c_sub);
        g_signal_connect(dlg, "response", G_CALLBACK(on_fif_response), NULL);
    }

    /* Prefill directory from current file / cwd, and find from selection. */
    GtkEntry *dir  = g_object_get_data(G_OBJECT(dlg), "dir");
    GtkEntry *find = g_object_get_data(G_OBJECT(dlg), "find");
    Document *doc = current_document();
    if (!*gtk_entry_get_text(dir)) {
        char *d = NULL;
        if (doc && doc->path) d = g_path_get_dirname(doc->path);
        else d = g_get_current_dir();
        gtk_entry_set_text(dir, d);
        g_free(d);
    }
    if (doc) {
        GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);
        GtkTextIter s, e;
        if (gtk_text_buffer_get_selection_bounds(b, &s, &e)) {
            char *sel = gtk_text_buffer_get_text(b, &s, &e, FALSE);
            if (sel && !strchr(sel, '\n'))
                gtk_entry_set_text(find, sel);
            g_free(sel);
        }
    }
    gtk_widget_show_all(dlg);
    gtk_window_present(GTK_WINDOW(dlg));
    gtk_widget_grab_focus(GTK_WIDGET(find));
}

static GtkWidget *build_results_panel(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(header, 6);
    gtk_widget_set_margin_end(header, 6);
    gtk_widget_set_margin_top(header, 4);
    gtk_widget_set_margin_bottom(header, 4);
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b>Results</b>");
    app.results_summary = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(app.results_summary), PANGO_ELLIPSIZE_END);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close",
                                                     GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    g_signal_connect(close, "clicked", G_CALLBACK(results_panel_hide), NULL);
    gtk_box_pack_start(GTK_BOX(header), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), app.results_summary, TRUE, TRUE, 8);
    gtk_box_pack_end(GTK_BOX(header), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);

    app.results_store = gtk_tree_store_new(3, G_TYPE_STRING, G_TYPE_STRING,
                                           G_TYPE_INT);
    app.results_tree = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(app.results_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app.results_tree), FALSE);
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "Result", r, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.results_tree), col);
    g_signal_connect(app.results_tree, "row-activated",
                     G_CALLBACK(on_result_activated), NULL);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), app.results_tree);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);

    app.results_panel = box;
    return box;
}

/* ===================================================================== */
/*  Plugins                                                               */
/* ===================================================================== */

static GtkTextView *api_active_view(void)
{
    Document *doc = current_document();
    return doc ? GTK_TEXT_VIEW(doc->view) : NULL;
}
static GtkTextBuffer *api_active_buffer(void)
{
    Document *doc = current_document();
    return doc ? GTK_TEXT_BUFFER(doc->buffer) : NULL;
}
static char *api_get_text(void)
{
    GtkTextBuffer *b = api_active_buffer();
    if (!b) return NULL;
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    return gtk_text_buffer_get_text(b, &s, &e, FALSE);
}
static char *api_get_selection(void)
{
    GtkTextBuffer *b = api_active_buffer();
    if (!b) return NULL;
    GtkTextIter s, e;
    if (!gtk_text_buffer_get_selection_bounds(b, &s, &e)) return NULL;
    return gtk_text_buffer_get_text(b, &s, &e, FALSE);
}
static void api_replace_selection(const char *text)
{
    GtkTextBuffer *b = api_active_buffer();
    if (!b) return;
    gtk_text_buffer_begin_user_action(b);
    gtk_text_buffer_delete_selection(b, TRUE, TRUE);
    gtk_text_buffer_insert_at_cursor(b, text, -1);
    gtk_text_buffer_end_user_action(b);
}
static void api_insert_at_cursor(const char *text)
{
    GtkTextBuffer *b = api_active_buffer();
    if (b) gtk_text_buffer_insert_at_cursor(b, text, -1);
}
static const char *api_current_path(void)
{
    Document *doc = current_document();
    return doc ? doc->path : NULL;
}
static void api_message(const char *text)
{
    if (app.lbl_len) gtk_label_set_text(GTK_LABEL(app.lbl_len), text);
}
static void api_add_menu_item(TuxpadApi *api, const char *label,
                              GCallback cb, gpointer user_data)
{
    (void)api;
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    g_signal_connect(mi, "activate", cb, user_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(app.plugins_menu), mi);
    gtk_widget_show(mi);
}

static void load_plugins_from(const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, ".so")) continue;
        char *full = g_build_filename(dir, name, NULL);
        GModule *mod = g_module_open(full, G_MODULE_BIND_LAZY);
        if (!mod) {
            g_warning("tuxpad: could not load plugin %s: %s", full,
                      g_module_error());
            g_free(full);
            continue;
        }
        gpointer sym = NULL;
        if (g_module_symbol(mod, "tuxpad_plugin_register", &sym) && sym) {
            TuxpadPluginRegisterFunc reg = (TuxpadPluginRegisterFunc)sym;
            if (reg(&app.api) == 0) {
                app.plugin_modules = g_list_prepend(app.plugin_modules, mod);
            } else {
                g_warning("tuxpad: plugin %s failed to register", name);
                g_module_close(mod);
            }
        } else {
            g_warning("tuxpad: %s has no tuxpad_plugin_register symbol", name);
            g_module_close(mod);
        }
        g_free(full);
    }
    g_dir_close(d);
}

static void load_all_plugins(void)
{
    app.api.api_version = TUXPAD_PLUGIN_API_VERSION;
    app.api.window      = GTK_WINDOW(app.window);
    app.api.add_menu_item     = api_add_menu_item;
    app.api.active_view       = api_active_view;
    app.api.active_buffer     = api_active_buffer;
    app.api.get_text          = api_get_text;
    app.api.get_selection     = api_get_selection;
    app.api.replace_selection = api_replace_selection;
    app.api.insert_at_cursor  = api_insert_at_cursor;
    app.api.current_path      = api_current_path;
    app.api.message           = api_message;

    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);

    /* 1. next to the executable (build tree)   */
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe) {
        char *base = g_path_get_dirname(exe);
        g_ptr_array_add(dirs, g_build_filename(base, "plugins", NULL));
        g_ptr_array_add(dirs, g_build_filename(base, "..", "lib", "tuxpad",
                                               "plugins", NULL));
        g_free(base); g_free(exe);
    }
    /* 2. user config dir                        */
    g_ptr_array_add(dirs, g_build_filename(g_get_user_config_dir(),
                                           "tuxpad", "plugins", NULL));

    for (guint i = 0; i < dirs->len; i++)
        load_plugins_from(g_ptr_array_index(dirs, i));
    g_ptr_array_free(dirs, TRUE);
}

/* ===================================================================== */
/*  Tools: code formatting, syntax check, flow diagram                   */
/* ===================================================================== */

static gboolean have_tool(const char *name)
{
    char *p = g_find_program_in_path(name);
    gboolean ok = p != NULL;
    g_free(p);
    return ok;
}

/* A sensible file extension for the active document (for temp files and for
   telling external tools which language they are looking at). */
static const char *doc_ext(Document *doc)
{
    if (doc->path) {
        const char *dot = strrchr(doc->path, '.');
        if (dot && dot[1]) return dot + 1;
    }
    GtkSourceLanguage *l = gtk_source_buffer_get_language(doc->buffer);
    const char *id = l ? gtk_source_language_get_id(l) : NULL;
    if (!id) return "txt";
    if (!strcmp(id, "cpp"))    return "cpp";
    if (!strcmp(id, "chdr"))   return "h";
    if (!strcmp(id, "c"))      return "c";
    if (!strcmp(id, "python")) return "py";
    if (!strcmp(id, "js"))     return "js";
    if (!strcmp(id, "java"))   return "java";
    if (!strcmp(id, "json"))   return "json";
    if (!strcmp(id, "css"))    return "css";
    if (!strcmp(id, "html"))   return "html";
    return id;
}

static gboolean ext_is_cpp(const char *ext)
{
    const char *cpp[] = {"cpp","cc","cxx","c++","hpp","hh","hxx","hpp", NULL};
    for (int i = 0; cpp[i]; i++)
        if (!g_ascii_strcasecmp(ext, cpp[i])) return TRUE;
    return FALSE;
}

/* Write `text` to a fresh temp file named code.<ext> in a private temp dir.
   Returns the file path; *dir_out receives the directory (both to free). */
static char *write_temp(const char *text, const char *ext, char **dir_out)
{
    char *dir = g_dir_make_tmp("tuxpad-XXXXXX", NULL);
    if (!dir) { *dir_out = NULL; return NULL; }
    char *name = g_strdup_printf("code.%s", (ext && *ext) ? ext : "txt");
    char *path = g_build_filename(dir, name, NULL);
    g_file_set_contents(path, text, -1, NULL);
    g_free(name);
    *dir_out = dir;
    return path;
}

static void cleanup_temp(char *path, char *dir)
{
    if (path) { g_remove(path); g_free(path); }
    if (dir)  { g_rmdir(dir);   g_free(dir);  }
}

static gboolean run_capture(char **argv, char **out, char **err, int *status)
{
    GError *e = NULL;
    gboolean ok = g_spawn_sync(NULL, argv, NULL,
                               G_SPAWN_SEARCH_PATH, NULL, NULL,
                               out, err, status, &e);
    if (!ok) {
        if (err) *err = g_strdup(e ? e->message : "spawn failed");
        if (e) g_error_free(e);
    }
    return ok;
}

/* ---- Format Document ---- */

static void act_format(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    Document *doc = current_document();
    if (!doc) return;
    GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);

    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char *text = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    const char *ext = doc_ext(doc);

    /* remember cursor line to restore afterwards */
    GtkTextIter cur;
    gtk_text_buffer_get_iter_at_mark(b, &cur, gtk_text_buffer_get_insert(b));
    int cur_line = gtk_text_iter_get_line(&cur);

    char *dir = NULL, *tmp = write_temp(text, ext, &dir);
    g_free(text);
    if (!tmp) return;

    char *out = NULL, *err = NULL, *result = NULL;
    int st = 0;

    if (have_tool("clang-format")) {
        char *argv[] = { "clang-format", tmp, NULL };
        if (run_capture(argv, &out, &err, &st) && out) {
            result = out; out = NULL;
        }
    } else if (have_tool("astyle")) {
        char *argv[] = { "astyle", "--quiet", "--suffix=none",
                         "--style=kr", "--indent=spaces=4", tmp, NULL };
        if (run_capture(argv, &out, &err, &st))
            g_file_get_contents(tmp, &result, NULL, NULL);
    } else if (have_tool("indent") && !ext_is_cpp(ext)) {
        char *argv[] = { "indent", "-st", "-kr", "-i4", tmp, NULL };
        if (run_capture(argv, &out, &err, &st) && out) {
            result = out; out = NULL;
        }
    } else {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(app.window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "No code formatter found.\n\n"
            "Install one, for example:\n"
            "    sudo apt install clang-format\n"
            "    sudo apt install astyle\n"
            "    sudo apt install indent");
        gtk_window_set_title(GTK_WINDOW(dlg), "Format Document");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        cleanup_temp(tmp, dir);
        return;
    }

    cleanup_temp(tmp, dir);
    g_free(out);

    if (result && *result) {
        gtk_text_buffer_begin_user_action(b);
        gtk_text_buffer_get_bounds(b, &s, &e);
        gtk_text_buffer_delete(b, &s, &e);
        gtk_text_buffer_get_start_iter(b, &s);
        gtk_text_buffer_insert(b, &s, result, -1);
        gtk_text_buffer_end_user_action(b);

        int lines = gtk_text_buffer_get_line_count(b);
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(b, &it,
            cur_line < lines ? cur_line : lines - 1);
        gtk_text_buffer_place_cursor(b, &it);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(doc->view),
            gtk_text_buffer_get_insert(b), 0.25, FALSE, 0, 0);
        gtk_label_set_text(GTK_LABEL(app.lbl_len), "Document formatted.");
    } else {
        error_dialog("Formatting failed:\n%s",
                     err && *err ? err : "the formatter produced no output");
    }
    g_free(result);
    g_free(err);
}

/* ---- Syntax Check (C / C++) ---- */

static void act_syntax_check(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    Document *doc = current_document();
    if (!doc) return;
    const char *ext = doc_ext(doc);
    gboolean cpp = ext_is_cpp(ext);
    gboolean c   = !g_ascii_strcasecmp(ext, "c") ||
                   !g_ascii_strcasecmp(ext, "h");
    if (!cpp && !c) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(app.window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "Syntax check supports C and C++ source files.\n"
            "The active document is not recognised as C/C++.");
        gtk_window_set_title(GTK_WINDOW(dlg), "Syntax Check");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        return;
    }
    const char *cc = cpp ? "g++" : "gcc";
    if (!have_tool(cc)) {
        error_dialog("Compiler not found: %s\n"
                     "Install it with: sudo apt install build-essential", cc);
        return;
    }

    GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char *text = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    char *dir = NULL, *tmp = write_temp(text, cpp ? "cpp" : "c", &dir);
    g_free(text);
    if (!tmp) return;

    char *out = NULL, *err = NULL;
    int st = 0;
    char *argv[] = { (char *)cc, "-fsyntax-only", "-Wall", "-Wextra",
                     tmp, NULL };
    run_capture(argv, &out, &err, &st);
    g_free(out);

    /* Parse "<tmp>:line:col: error|warning|note: message" */
    gtk_tree_store_clear(app.results_store);
    GtkTreeIter top;
    gtk_tree_store_append(app.results_store, &top, NULL);

    int errors = 0, warnings = 0;
    GRegex *re = g_regex_new(":(\\d+):(?:(\\d+):)?\\s+"
                             "(error|warning|note|fatal error):\\s+(.*)$",
                             G_REGEX_MULTILINE, 0, NULL);
    if (err && re) {
        char **lines = g_strsplit(err, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            GMatchInfo *mi = NULL;
            if (g_regex_match(re, lines[i], 0, &mi)) {
                char *ln  = g_match_info_fetch(mi, 1);
                char *typ = g_match_info_fetch(mi, 3);
                char *msg = g_match_info_fetch(mi, 4);
                if (!strcmp(typ, "warning")) warnings++;
                else if (!strcmp(typ, "note")) ; /* don't count notes */
                else errors++;
                char *label = g_strdup_printf("Line %s:  [%s]  %s",
                                              ln, typ, msg);
                GtkTreeIter child;
                gtk_tree_store_append(app.results_store, &child, &top);
                gtk_tree_store_set(app.results_store, &child,
                    0, label, 1, doc->path ? doc->path : "", 2, atoi(ln), -1);
                g_free(label); g_free(ln); g_free(typ); g_free(msg);
            }
            if (mi) g_match_info_free(mi);
        }
        g_strfreev(lines);
    }
    if (re) g_regex_unref(re);
    g_free(err);
    cleanup_temp(tmp, dir);

    char *summary;
    if (errors == 0 && warnings == 0)
        summary = g_strdup_printf("Syntax check (%s): no problems found.", cc);
    else
        summary = g_strdup_printf("Syntax check (%s): %d error(s), %d warning(s).",
                                  cc, errors, warnings);
    gtk_tree_store_set(app.results_store, &top, 0, summary, 1, "", 2, 0, -1);
    gtk_label_set_text(GTK_LABEL(app.results_summary), summary);
    gtk_label_set_text(GTK_LABEL(app.lbl_len), summary);
    g_free(summary);

    gtk_widget_show_all(app.results_panel);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(app.results_tree));
}

/* ---- Flow Diagram: a call graph rendered with Cairo ---- */

#define FLOW_MAX_NODES 256

typedef struct {
    char  *name[FLOW_MAX_NODES];    /* bare function name (for edge matching) */
    char  *header[FLOW_MAX_NODES];  /* "rettype name(params)" signature       */
    char  *detail[FLOW_MAX_NODES];  /* multiline: params / locals / returns   */
    char  *file[FLOW_MAX_NODES];    /* source file of the definition, or NULL */
    int    def_line[FLOW_MAX_NODES];/* 1-based line of the definition         */
    int    n;
    /* edges as pairs of node indices */
    int    e_from[FLOW_MAX_NODES * 8];
    int    e_to  [FLOW_MAX_NODES * 8];
    int    ne;
    /* layout */
    int    layer[FLOW_MAX_NODES];
    double x[FLOW_MAX_NODES], y[FLOW_MAX_NODES];
    double w[FLOW_MAX_NODES], h[FLOW_MAX_NODES];
    double width, height;
    /* interaction state (mouse drag / click, zoom) */
    GtkWidget *area;
    int       drag_idx;             /* node under the pointer, or -1          */
    gboolean  moved;                /* became a drag rather than a click      */
    double    press_x, press_y, drag_dx, drag_dy;
    double    zoom;                 /* 1.0 = 100%                             */
    gboolean  compact;              /* show only signatures (cleaner view)    */
    gboolean  panning;              /* dragging to pan the view               */
    gboolean  node_drag;            /* Shift+drag moving a single node         */
    int       click_idx;            /* node under the press, for click-to-jump */
    double    pan_rx, pan_ry, pan_sx, pan_sy;
} FlowGraph;

static gboolean is_c_keyword(const char *s)
{
    static const char *kw[] = {
        "if","for","while","switch","return","sizeof","do","else","catch",
        "case","default","goto","break","continue","typedef","struct","union",
        "enum","static","const","void","int","char","float","double","long",
        "short","unsigned","signed","new","delete","throw","template", NULL };
    for (int i = 0; kw[i]; i++) if (!strcmp(s, kw[i])) return TRUE;
    return FALSE;
}

static int flow_node_index(FlowGraph *g, const char *name)
{
    for (int i = 0; i < g->n; i++)
        if (!strcmp(g->name[i], name)) return i;
    return -1;
}

/* Match the matching '}' for the '{' at text[open]; skips strings/comments. */
static gssize match_brace(const char *t, gssize len, gssize open)
{
    int depth = 0;
    for (gssize i = open; i < len; i++) {
        char c = t[i];
        if (c == '/' && i + 1 < len && t[i+1] == '/') {
            while (i < len && t[i] != '\n') i++;
        } else if (c == '/' && i + 1 < len && t[i+1] == '*') {
            i += 2;
            while (i + 1 < len && !(t[i] == '*' && t[i+1] == '/')) i++;
            i++;
        } else if (c == '"' || c == '\'') {
            char q = c; i++;
            while (i < len && t[i] != q) { if (t[i] == '\\') i++; i++; }
        } else if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) return i; }
    }
    return -1;
}

/* Collapse runs of whitespace to single spaces and trim. Caller frees. */
static char *collapse_ws(const char *s)
{
    GString *o = g_string_new(NULL);
    gboolean sp = FALSE, started = FALSE;
    for (; *s; s++) {
        if (g_ascii_isspace(*s)) { sp = started; }
        else { if (sp) g_string_append_c(o, ' '); g_string_append_c(o, *s);
               sp = FALSE; started = TRUE; }
    }
    return g_string_free(o, FALSE);
}

/* The parameter *names* out of a C parameter list, as "a, b, c". */
static char *param_names(const char *params)
{
    char *p = collapse_ws(params);
    if (!*p || !g_strcmp0(p, "void")) { g_free(p); return NULL; }
    GString *o = g_string_new(NULL);
    char **parts = g_strsplit(p, ",", -1);
    for (int i = 0; parts[i]; i++) {
        /* the name is the last identifier token in the fragment */
        const char *q = parts[i]; const char *last = NULL; const char *run = NULL;
        for (const char *c = q; *c; c++) {
            if (g_ascii_isalnum(*c) || *c == '_') { if (!run) run = c; last = run; }
            else run = NULL;
        }
        if (last) {
            const char *end = last;
            while (*end && (g_ascii_isalnum(*end) || *end == '_')) end++;
            char *nm = g_strndup(last, end - last);
            if (!is_c_keyword(nm)) {
                if (o->len) g_string_append(o, ", ");
                g_string_append(o, nm);
            }
            g_free(nm);
        }
    }
    g_strfreev(parts);
    g_free(p);
    if (!o->len) { g_string_free(o, TRUE); return NULL; }
    return g_string_free(o, FALSE);
}

/* Append `item` to GString `o` (comma separated) if not already present. */
static void add_unique(GString *o, const char *item)
{
    if (!item || !*item) return;
    char **have = g_strsplit(o->str, ", ", -1);
    for (int i = 0; have[i]; i++)
        if (!strcmp(have[i], item)) { g_strfreev(have); return; }
    g_strfreev(have);
    if (o->len) g_string_append(o, ", ");
    g_string_append(o, item);
}

/* The call graph can be built from several files. flow_build_init() starts an
   empty graph, flow_add_file() contributes one file's definitions (recording
   each function's file and line), and flow_build_finish() resolves calls across
   all files, fills in locals/returns, and computes the layering. */
typedef struct {
    FlowGraph *g;
    GString   *det[FLOW_MAX_NODES];   /* detail text under construction */
    int        b_idx[FLOW_MAX_NODES]; /* node index of each stored body */
    char      *b_body[FLOW_MAX_NODES];/* copy of each function body     */
    int        nb;
} FlowBuild;

static void flow_build_init(FlowBuild *fb, FlowGraph *g)
{
    memset(g, 0, sizeof *g);
    g->drag_idx = -1;
    g->zoom = 1.0;
    memset(fb, 0, sizeof *fb);
    fb->g = g;
}

static void flow_add_file(FlowBuild *fb, const char *file, const char *text)
{
    FlowGraph *g = fb->g;
    gssize len = strlen(text);
    GRegex *re_def = g_regex_new(
        "([A-Za-z_]\\w*)\\s*\\(([^()]*)\\)\\s*\\{",
        G_REGEX_MULTILINE, 0, NULL);
    GMatchInfo *mi = NULL;
    g_regex_match(re_def, text, 0, &mi);
    while (g_match_info_matches(mi) && g->n < FLOW_MAX_NODES) {
        char *name   = g_match_info_fetch(mi, 1);
        char *params = g_match_info_fetch(mi, 2);
        int start, end;
        g_match_info_fetch_pos(mi, 0, &start, &end);
        if (!is_c_keyword(name)) {
            gssize brace = end - 1;
            gssize bend = match_brace(text, len, brace);

            /* The text from the start of the line up to the name is the return
               type. A real return type never contains ')', ',', ';', '=' or a
               lone ':'. Rejecting those filters out C++ constructor member
               initializer lists ("Foo::Foo(x) : a_(x), b_(y) {"), ternary
               expressions, labels and bitfields that would otherwise look like
               a definition. */
            gssize ls = start;
            while (ls > 0 && text[ls-1] != '\n') ls--;
            gboolean rt_ok = TRUE;
            for (gssize p = ls; p < start && rt_ok; p++) {
                char c = text[p];
                if (c==')'||c=='('||c==','||c==';'||c=='='||c=='{'||c=='}')
                    rt_ok = FALSE;
                else if (c==':') {
                    gboolean scope = (p+1 < start && text[p+1]==':') ||
                                     (p > ls && text[p-1]==':');
                    if (!scope) rt_ok = FALSE;
                }
            }

            if (rt_ok && bend > brace && flow_node_index(g, name) < 0) {
                int idx = g->n;
                g->name[idx] = g_strdup(name);

                char *rtraw = g_strndup(text + ls, start - ls);
                char *rt = collapse_ws(rtraw);
                g_free(rtraw);
                char *pclean = collapse_ws(params);
                g->header[idx] = g_strdup_printf("%s%s%s(%s)",
                    rt, *rt ? " " : "", name, *pclean ? pclean : "void");
                g_free(rt); g_free(pclean);

                g->file[idx] = file ? g_strdup(file) : NULL;
                int line = 1;
                for (gssize p = 0; p < start; p++) if (text[p] == '\n') line++;
                g->def_line[idx] = line;

                fb->det[idx] = g_string_new(NULL);
                char *pn = param_names(params);
                if (pn) {
                    g_string_append_printf(fb->det[idx], "params: %s", pn);
                    g_free(pn);
                }
                if (fb->nb < FLOW_MAX_NODES) {
                    fb->b_idx[fb->nb] = idx;
                    fb->b_body[fb->nb] = g_strndup(text + brace + 1,
                                                   bend - (brace + 1));
                    fb->nb++;
                }
                g->n++;
            }
        }
        g_free(name); g_free(params);
        g_match_info_next(mi, NULL);
    }
    if (mi) g_match_info_free(mi);
    g_regex_unref(re_def);
}

static void flow_build_finish(FlowBuild *fb)
{
    FlowGraph *g = fb->g;
    GRegex *re_call = g_regex_new("([A-Za-z_]\\w*)\\s*\\(", 0, 0, NULL);
    GRegex *re_decl = g_regex_new(
        "(?m)^[ \\t]*([A-Za-z_][\\w:<> \\t\\*&]*?[ \\t\\*&])"
        "([A-Za-z_]\\w*)\\s*(?:\\[[^\\]]*\\])?\\s*(?:=[^=]|;|,)",
        0, 0, NULL);
    GRegex *re_ret = g_regex_new("\\breturn\\b[ \\t]*([^;]*);", 0, 0, NULL);

    for (int i = 0; i < fb->nb; i++) {
        int idx = fb->b_idx[i];
        char *body = fb->b_body[i];
        GString *det = fb->det[idx];

        /* calls -> edges (resolved against all files' definitions) */
        GMatchInfo *cm = NULL;
        g_regex_match(re_call, body, 0, &cm);
        while (g_match_info_matches(cm)) {
            char *callee = g_match_info_fetch(cm, 1);
            int to = flow_node_index(g, callee);
            if (to >= 0 && to != idx && !is_c_keyword(callee)) {
                gboolean dup = FALSE;
                for (int k = 0; k < g->ne; k++)
                    if (g->e_from[k] == idx && g->e_to[k] == to)
                        { dup = TRUE; break; }
                if (!dup && g->ne < FLOW_MAX_NODES * 8) {
                    g->e_from[g->ne] = idx; g->e_to[g->ne] = to; g->ne++;
                }
            }
            g_free(callee);
            g_match_info_next(cm, NULL);
        }
        if (cm) g_match_info_free(cm);

        /* local variable declarations */
        GString *vars = g_string_new(NULL);
        GMatchInfo *dm = NULL;
        g_regex_match(re_decl, body, 0, &dm);
        while (g_match_info_matches(dm)) {
            char *type = g_match_info_fetch(dm, 1);
            char *nm   = g_match_info_fetch(dm, 2);
            char *ty   = collapse_ws(type);
            char *first = g_strdup(ty);
            char *sp = strchr(first, ' '); if (sp) *sp = 0;
            if (!is_c_keyword(nm) && !is_c_keyword(first) &&
                strcmp(first, "return") && *first)
                add_unique(vars, nm);
            g_free(type); g_free(nm); g_free(ty); g_free(first);
            g_match_info_next(dm, NULL);
        }
        if (dm) g_match_info_free(dm);
        if (vars->len && det) {
            if (det->len) g_string_append_c(det, '\n');
            g_string_append_printf(det, "locals: %s", vars->str);
        }
        g_string_free(vars, TRUE);

        /* return values */
        GString *rets = g_string_new(NULL);
        GMatchInfo *rm = NULL;
        g_regex_match(re_ret, body, 0, &rm);
        while (g_match_info_matches(rm)) {
            char *expr = g_match_info_fetch(rm, 1);
            char *ex = collapse_ws(expr);
            add_unique(rets, *ex ? ex : "(void)");
            g_free(expr); g_free(ex);
            g_match_info_next(rm, NULL);
        }
        if (rm) g_match_info_free(rm);
        if (rets->len && det) {
            if (det->len) g_string_append_c(det, '\n');
            char trunc[120];
            g_strlcpy(trunc, rets->str, sizeof trunc);
            g_string_append_printf(det, "returns: %s%s", trunc,
                strlen(rets->str) > sizeof trunc - 1 ? "…" : "");
        }
        g_string_free(rets, TRUE);

        g_free(body);
    }
    g_regex_unref(re_call);
    g_regex_unref(re_decl);
    g_regex_unref(re_ret);

    for (int i = 0; i < g->n; i++)
        g->detail[i] = fb->det[i] ? g_string_free(fb->det[i], FALSE) : NULL;

    for (int it = 0; it < g->n; it++)
        for (int k = 0; k < g->ne; k++)
            if (g->layer[g->e_to[k]] < g->layer[g->e_from[k]] + 1)
                g->layer[g->e_to[k]] = g->layer[g->e_from[k]] + 1;
}

/* Convenience: build a graph from a single buffer of text. */
static void build_call_graph2(const char *file, const char *text, FlowGraph *g)
{
    FlowBuild fb;
    flow_build_init(&fb, g);
    flow_add_file(&fb, file, text);
    flow_build_finish(&fb);
}

#define FLOW_HEADER_H  26.0
#define FLOW_LINE_H    16.0
#define FLOW_PADX      30.0
#define FLOW_PADY      30.0
#define FLOW_VGAP      22.0

static int count_lines(const char *s)
{
    if (!s || !*s) return 0;
    int n = 1;
    for (; *s; s++) if (*s == '\n') n++;
    return n;
}

static double text_width(cairo_t *cr, const char *s, double size)
{
    cairo_set_font_size(cr, size);
    cairo_text_extents_t te;
    cairo_text_extents(cr, s, &te);
    return te.width;
}

/* Order the nodes of layer L by the average order-position (barycenter) of
   their neighbours in the adjacent layer `adj`, reducing edge crossings. */
static void flow_order_layer(FlowGraph *g, int *lay, int *pos, int *lcount,
                             int n, int L, int adj)
{
    int cnt = lcount[L];
    double bary[FLOW_MAX_NODES];
    for (int k = 0; k < cnt; k++) {
        int v = lay[L * n + k];
        double sum = 0; int deg = 0;
        for (int e = 0; e < g->ne; e++) {
            int u = -1;
            if (g->e_from[e] == v && g->layer[g->e_to[e]]   == adj) u = g->e_to[e];
            else if (g->e_to[e] == v && g->layer[g->e_from[e]] == adj) u = g->e_from[e];
            if (u >= 0) { sum += pos[u]; deg++; }
        }
        bary[k] = deg ? sum / deg : pos[v];
    }
    /* stable insertion sort of the layer by barycenter */
    for (int a = 1; a < cnt; a++) {
        int va = lay[L * n + a]; double ba = bary[a]; int b = a - 1;
        while (b >= 0 && bary[b] > ba) {
            lay[L * n + b + 1] = lay[L * n + b]; bary[b + 1] = bary[b]; b--;
        }
        lay[L * n + b + 1] = va; bary[b + 1] = ba;
    }
    for (int k = 0; k < cnt; k++) pos[lay[L * n + k]] = k;
}

/* Structured layered layout: assign a column per call depth, order nodes to
   reduce crossings, then align each node toward the centre of its neighbours
   while keeping columns non-overlapping. */
static void flow_layout(FlowGraph *g, cairo_t *cr)
{
    int n = g->n;
    int maxlayer = 0;
    for (int i = 0; i < n; i++)
        if (g->layer[i] > maxlayer) maxlayer = g->layer[i];
    int nlayers = maxlayer + 1;

    /* node sizes */
    double maxw = 120;
    for (int i = 0; i < n; i++) {
        double wmax = text_width(cr, g->header[i], 13);
        int nl = g->compact ? 0 : count_lines(g->detail[i]);
        if (nl) {
            char **lines = g_strsplit(g->detail[i], "\n", -1);
            for (int j = 0; lines[j]; j++) {
                double lw = text_width(cr, lines[j], 11);
                if (lw > wmax) wmax = lw;
            }
            g_strfreev(lines);
        }
        g->w[i] = wmax + 24;
        g->h[i] = FLOW_HEADER_H + nl * FLOW_LINE_H + (nl ? 6 : 0);
        if (g->w[i] > maxw) maxw = g->w[i];
    }
    double colw = maxw + (g->compact ? 55 : 70);

    /* order within layers */
    int *pos    = g_new0(int, n);
    int *lcount = g_new0(int, nlayers);
    int *lay    = g_new0(int, (gsize)nlayers * n);
    for (int i = 0; i < n; i++) {
        int L = g->layer[i]; int k = lcount[L]++;
        pos[i] = k; lay[L * n + k] = i;
    }
    for (int sweep = 0; sweep < 6; sweep++) {
        if (sweep % 2 == 0)
            for (int L = 1; L < nlayers; L++)
                flow_order_layer(g, lay, pos, lcount, n, L, L - 1);
        else
            for (int L = nlayers - 2; L >= 0; L--)
                flow_order_layer(g, lay, pos, lcount, n, L, L + 1);
    }

    /* x by layer; initial y by stacking in order */
    for (int L = 0; L < nlayers; L++) {
        double yy = FLOW_PADY;
        for (int k = 0; k < lcount[L]; k++) {
            int v = lay[L * n + k];
            g->x[v] = FLOW_PADX + L * colw;
            g->y[v] = yy;
            yy += g->h[v] + FLOW_VGAP;
        }
    }

    /* alignment passes: pull each node toward its neighbours' centre while
       preserving order and non-overlap within the column */
    for (int iter = 0; iter < 10; iter++) {
        for (int L = 0; L < nlayers; L++) {
            int cnt = lcount[L];
            double desired[FLOW_MAX_NODES];
            for (int k = 0; k < cnt; k++) {
                int v = lay[L * n + k];
                double sum = 0; int deg = 0;
                for (int e = 0; e < g->ne; e++) {
                    int u = -1;
                    if (g->e_from[e] == v) u = g->e_to[e];
                    else if (g->e_to[e] == v) u = g->e_from[e];
                    if (u >= 0) { sum += g->y[u] + g->h[u] / 2; deg++; }
                }
                desired[k] = deg ? sum / deg - g->h[v] / 2 : g->y[v];
            }
            double prev_bottom = FLOW_PADY;
            for (int k = 0; k < cnt; k++) {
                int v = lay[L * n + k];
                double yv = desired[k] < prev_bottom ? prev_bottom : desired[k];
                g->y[v] = yv;
                prev_bottom = yv + g->h[v] + FLOW_VGAP;
            }
        }
    }

    g_free(pos); g_free(lcount); g_free(lay);

    /* shift everything up so the topmost node sits at the top padding */
    double miny = 1e9;
    for (int i = 0; i < n; i++) if (g->y[i] < miny) miny = g->y[i];
    double shift = FLOW_PADY - miny;
    for (int i = 0; i < n; i++) g->y[i] += shift;

    double W = 0, H = 0;
    for (int i = 0; i < n; i++) {
        if (g->x[i] + g->w[i] > W) W = g->x[i] + g->w[i];
        if (g->y[i] + g->h[i] > H) H = g->y[i] + g->h[i];
    }
    g->width  = W + FLOW_PADX;
    g->height = H + FLOW_PADY;
}

static gboolean on_flow_draw(GtkWidget *area, cairo_t *cr, gpointer data)
{
    FlowGraph *g = data;
    (void)area;

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    double z = g->zoom > 0 ? g->zoom : 1.0;
    cairo_scale(cr, z, z);
    cairo_select_font_face(cr, "monospace",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    if (g->width == 0) flow_layout(g, cr);

    /* edges first (anchored at each node's vertical centre) */
    cairo_set_line_width(cr, 1.5);
    for (int k = 0; k < g->ne; k++) {
        int a = g->e_from[k], b = g->e_to[k];
        double ax = g->x[a] + g->w[a], ay = g->y[a] + g->h[a] / 2;
        double bx = g->x[b], by = g->y[b] + g->h[b] / 2;
        double mx = (ax + bx) / 2;
        cairo_set_source_rgb(cr, 0.40, 0.50, 0.60);
        cairo_move_to(cr, ax, ay);
        cairo_curve_to(cr, mx, ay, mx, by, bx, by);
        cairo_stroke(cr);
        double ang = atan2(by - ay, bx - mx);
        cairo_move_to(cr, bx, by);
        cairo_line_to(cr, bx - 10 * cos(ang - 0.4), by - 10 * sin(ang - 0.4));
        cairo_move_to(cr, bx, by);
        cairo_line_to(cr, bx - 10 * cos(ang + 0.4), by - 10 * sin(ang + 0.4));
        cairo_stroke(cr);
    }

    /* nodes */
    for (int i = 0; i < g->n; i++) {
        double x = g->x[i], y = g->y[i], w = g->w[i], h = g->h[i];

        /* body */
        cairo_set_source_rgb(cr, 0.97, 0.98, 1.0);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);

        /* header bar */
        cairo_set_source_rgb(cr, 0.21, 0.42, 0.60);
        cairo_rectangle(cr, x, y, w, FLOW_HEADER_H);
        cairo_fill(cr);

        /* border */
        cairo_set_source_rgb(cr, 0.21, 0.42, 0.60);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, x, y, w, h);
        cairo_stroke(cr);

        /* header text (white) */
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_font_size(cr, 13);
        cairo_text_extents_t te;
        cairo_text_extents(cr, g->header[i], &te);
        cairo_move_to(cr, x + 10, y + (FLOW_HEADER_H - te.height) / 2
                                    - te.y_bearing);
        cairo_show_text(cr, g->header[i]);

        /* detail lines */
        if (!g->compact && g->detail[i] && *g->detail[i]) {
            cairo_set_font_size(cr, 11);
            cairo_set_source_rgb(cr, 0.2, 0.2, 0.25);
            char **lines = g_strsplit(g->detail[i], "\n", -1);
            double ty = y + FLOW_HEADER_H + 4;
            for (int j = 0; lines[j]; j++) {
                ty += FLOW_LINE_H;
                cairo_move_to(cr, x + 10, ty - 4);
                cairo_show_text(cr, lines[j]);
            }
            g_strfreev(lines);
        }
    }
    return FALSE;
}

static void flow_bounds(FlowGraph *g, double *W, double *H)
{
    double w = g->width, h = g->height;
    for (int i = 0; i < g->n; i++) {
        if (g->x[i] + g->w[i] + 20 > w) w = g->x[i] + g->w[i] + 20;
        if (g->y[i] + g->h[i] + 20 > h) h = g->y[i] + g->h[i] + 20;
    }
    *W = w; *H = h;
}

/* Set *zoom to newz and resize the canvas, keeping the content point under
   (px,py) — full-canvas device coords, or <0 for the viewport centre — fixed
   on screen (zoom about the cursor). cw,ch are the unscaled content size. */
static void zoom_about(GtkWidget *area, double *zoom, double newz,
                       double cw, double ch, double px, double py)
{
    double oldz = *zoom > 0 ? *zoom : 1.0;
    GtkWidget *sw = gtk_widget_get_ancestor(area, GTK_TYPE_SCROLLED_WINDOW);
    GtkAdjustment *ha=NULL, *va=NULL;
    if (sw) {
        ha=gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(sw));
        va=gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    }
    if (ha && px<0) px=gtk_adjustment_get_value(ha)+gtk_adjustment_get_page_size(ha)/2;
    if (va && py<0) py=gtk_adjustment_get_value(va)+gtk_adjustment_get_page_size(va)/2;

    *zoom = newz;
    gtk_widget_set_size_request(area, (int)(cw*newz), (int)(ch*newz));

    if (ha) {
        double sx=gtk_adjustment_get_value(ha), ps=gtk_adjustment_get_page_size(ha);
        double nsx=px*(newz/oldz) - (px - sx);
        gtk_adjustment_set_upper(ha, cw*newz);
        gtk_adjustment_set_value(ha, CLAMP(nsx, 0, MAX(cw*newz-ps, 0)));
    }
    if (va) {
        double sy=gtk_adjustment_get_value(va), ps=gtk_adjustment_get_page_size(va);
        double nsy=py*(newz/oldz) - (py - sy);
        gtk_adjustment_set_upper(va, ch*newz);
        gtk_adjustment_set_value(va, CLAMP(nsy, 0, MAX(ch*newz-ps, 0)));
    }
    gtk_widget_queue_draw(area);
}

static void flow_apply_zoom(FlowGraph *g)
{
    if (!g->area) return;
    double W, H; flow_bounds(g, &W, &H);
    gtk_widget_set_size_request(g->area, (int)(W * g->zoom), (int)(H * g->zoom));
    gtk_widget_queue_draw(g->area);
}
static void flow_zoom(FlowGraph *g, double newz, double px, double py)
{
    if (!g->area) return;
    double W, H; flow_bounds(g, &W, &H);
    zoom_about(g->area, &g->zoom, newz, W, H, px, py);
}

static void on_zoom_in(GtkButton *b, gpointer d)
{ (void)b; FlowGraph *g = d; flow_zoom(g, MIN(g->zoom*1.2,4.0), -1, -1); }
static void on_zoom_out(GtkButton *b, gpointer d)
{ (void)b; FlowGraph *g = d; flow_zoom(g, MAX(g->zoom/1.2,0.25), -1, -1); }
static void on_zoom_reset(GtkButton *b, gpointer d)
{ (void)b; FlowGraph *g = d; flow_zoom(g, 1.0, -1, -1); }

static void on_flow_compact(GtkToggleButton *b, gpointer d)
{
    FlowGraph *g = d;
    g->compact = gtk_toggle_button_get_active(b);
    cairo_surface_t *m=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1,1);
    cairo_t *cr=cairo_create(m);
    cairo_select_font_face(cr,"monospace",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr,13);
    flow_layout(g, cr);
    cairo_destroy(cr); cairo_surface_destroy(m);
    flow_apply_zoom(g);
}

/* Scroll the enclosing GtkScrolledWindow explicitly. Returning FALSE from a
   child's scroll handler does not reliably reach the scrolled window, so we
   move its adjustment ourselves (Shift = horizontal). */
static gboolean scroll_enclosing(GtkWidget *area, GdkEventScroll *ev)
{
    GtkWidget *sw = gtk_widget_get_ancestor(area, GTK_TYPE_SCROLLED_WINDOW);
    if (!sw) return FALSE;
    gboolean horiz = (ev->state & GDK_SHIFT_MASK) ||
                     ev->direction == GDK_SCROLL_LEFT ||
                     ev->direction == GDK_SCROLL_RIGHT;
    GtkAdjustment *adj = horiz
        ? gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(sw))
        : gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    /* One wheel notch scrolls ~1/8 of a page (min 60px) — the drawing area's
       step increment is tiny, so basing it on the page size feels natural. */
    double page = gtk_adjustment_get_page_size(adj);
    double notch = MAX(page / 8.0, 60.0);
    double d = 0;
    if (ev->direction == GDK_SCROLL_UP || ev->direction == GDK_SCROLL_LEFT)
        d = -notch;
    else if (ev->direction == GDK_SCROLL_DOWN || ev->direction == GDK_SCROLL_RIGHT)
        d = notch;
    else if (ev->direction == GDK_SCROLL_SMOOTH) {
        double dx, dy;
        gdk_event_get_scroll_deltas((GdkEvent *)ev, &dx, &dy);
        d = (horiz ? dx : dy) * notch;
    }
    double v = gtk_adjustment_get_value(adj) + d;
    double hi = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
    double lo = gtk_adjustment_get_lower(adj);
    v = CLAMP(v, lo, hi);
    gtk_adjustment_set_value(adj, v);
    return TRUE;
}

static void set_area_cursor(GtkWidget *area, const char *name)
{
    GdkWindow *gw = gtk_widget_get_window(area);
    if (!gw) return;
    GdkCursor *c = name
        ? gdk_cursor_new_from_name(gtk_widget_get_display(area), name) : NULL;
    gdk_window_set_cursor(gw, c);
    if (c) g_object_unref(c);
}

/* Pan the scrolled view: keep the grabbed point under the pointer, using
   stable root (screen) coordinates. */
static void pan_view(GtkWidget *area, double sx0, double sy0,
                     double rx0, double ry0, double rx, double ry)
{
    GtkWidget *sw = gtk_widget_get_ancestor(area, GTK_TYPE_SCROLLED_WINDOW);
    if (!sw) return;
    GtkAdjustment *ha = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(sw));
    GtkAdjustment *va = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    double nx = sx0 - (rx - rx0), ny = sy0 - (ry - ry0);
    double hx = gtk_adjustment_get_upper(ha) - gtk_adjustment_get_page_size(ha);
    double hy = gtk_adjustment_get_upper(va) - gtk_adjustment_get_page_size(va);
    gtk_adjustment_set_value(ha, CLAMP(nx, 0, MAX(hx, 0)));
    gtk_adjustment_set_value(va, CLAMP(ny, 0, MAX(hy, 0)));
}

static gboolean on_flow_scroll(GtkWidget *area, GdkEventScroll *ev, gpointer d)
{
    FlowGraph *g = d;
    if (!(ev->state & GDK_CONTROL_MASK)) return scroll_enclosing(area, ev);
    double nz;
    if (ev->direction == GDK_SCROLL_UP) nz = MIN(g->zoom*1.1, 4.0);
    else if (ev->direction == GDK_SCROLL_DOWN) nz = MAX(g->zoom/1.1, 0.25);
    else if (ev->direction == GDK_SCROLL_SMOOTH) {
        double dx,dy; gdk_event_get_scroll_deltas((GdkEvent*)ev,&dx,&dy);
        nz = CLAMP(g->zoom * (1.0 - dy*0.1), 0.25, 4.0);
    } else return TRUE;
    flow_zoom(g, nz, ev->x, ev->y);
    return TRUE;
}

static void on_flow_save(GtkButton *btn, gpointer data)
{
    FlowGraph *g = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Save Diagram as PNG",
        GTK_WINDOW(app.window), GTK_FILE_CHOOSER_ACTION_SAVE,
        "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, NULL);
    (void)btn;
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "flow.png");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        double W, H; flow_bounds(g, &W, &H);
        double z = g->zoom; g->zoom = 1.0;      /* export at 100% */
        cairo_surface_t *surf = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, (int)W, (int)H);
        cairo_t *cr = cairo_create(surf);
        on_flow_draw(NULL, cr, g);
        cairo_surface_write_to_png(surf, path);
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        g->zoom = z;
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

static void on_flow_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    FlowGraph *g = data;
    for (int i = 0; i < g->n; i++) {
        g_free(g->name[i]);
        g_free(g->header[i]);
        g_free(g->detail[i]);
        g_free(g->file[i]);
    }
    g_free(g);
}

/* Which node (if any) is at (x,y)? Topmost (last drawn) wins. */
static int flow_hit(FlowGraph *g, double x, double y)
{
    for (int i = g->n - 1; i >= 0; i--)
        if (x >= g->x[i] && x <= g->x[i] + g->w[i] &&
            y >= g->y[i] && y <= g->y[i] + g->h[i])
            return i;
    return -1;
}

static gboolean on_flow_press(GtkWidget *area, GdkEventButton *ev, gpointer data)
{
    FlowGraph *g = data;
    if (ev->button != 1) return FALSE;
    double mx = ev->x / g->zoom, my = ev->y / g->zoom;
    int i = flow_hit(g, mx, my);
    g->moved = FALSE;
    g->press_x = ev->x; g->press_y = ev->y;
    g->pan_rx = ev->x_root; g->pan_ry = ev->y_root;  /* stable screen coords */
    g->click_idx = i;                 /* for a pure click (jump to code) */

    if ((ev->state & GDK_SHIFT_MASK) && i >= 0) {
        /* Shift+drag moves a single node */
        g->node_drag = TRUE; g->panning = FALSE; g->drag_idx = i;
        g->drag_dx = mx - g->x[i]; g->drag_dy = my - g->y[i];
        set_area_cursor(area, "move");
    } else {
        /* plain drag pans the view (never touches the editor) */
        g->node_drag = FALSE; g->panning = TRUE; g->drag_idx = -1;
        GtkWidget *sw = gtk_widget_get_ancestor(area, GTK_TYPE_SCROLLED_WINDOW);
        if (sw) {
            g->pan_sx = gtk_adjustment_get_value(
                gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(sw)));
            g->pan_sy = gtk_adjustment_get_value(
                gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw)));
        }
        set_area_cursor(area, "grabbing");
    }
    return TRUE;
}

static gboolean on_flow_motion(GtkWidget *area, GdkEventMotion *ev, gpointer data)
{
    FlowGraph *g = data;

    if (!(ev->state & GDK_BUTTON1_MASK)) {
        int over = flow_hit(g, ev->x / g->zoom, ev->y / g->zoom);
        set_area_cursor(area, over >= 0 ? "pointer" : "grab");
        return FALSE;
    }
    /* Use screen coords: panning scrolls the canvas, so widget-relative coords
       barely change even as the pointer moves. */
    if (fabs(ev->x_root - g->pan_rx) > 3 || fabs(ev->y_root - g->pan_ry) > 3)
        g->moved = TRUE;

    if (g->node_drag && g->drag_idx >= 0) {
        int i = g->drag_idx;
        g->x[i] = ev->x / g->zoom - g->drag_dx;
        g->y[i] = ev->y / g->zoom - g->drag_dy;
        if (g->x[i] < 0) g->x[i] = 0;
        if (g->y[i] < 0) g->y[i] = 0;
        int needw = (int)((g->x[i] + g->w[i] + 40) * g->zoom);
        int needh = (int)((g->y[i] + g->h[i] + 40) * g->zoom);
        int curw = 0, curh = 0;
        gtk_widget_get_size_request(area, &curw, &curh);
        if (needw > curw || needh > curh)
            gtk_widget_set_size_request(area, MAX(needw, curw), MAX(needh, curh));
        gtk_widget_queue_draw(area);
    } else if (g->panning) {
        pan_view(area, g->pan_sx, g->pan_sy, g->pan_rx, g->pan_ry,
                 ev->x_root, ev->y_root);
    }
    return TRUE;
}

static gboolean on_flow_release(GtkWidget *area, GdkEventButton *ev, gpointer data)
{
    FlowGraph *g = data;
    if (ev->button != 1) return FALSE;
    set_area_cursor(area, "grab");
    /* a pure click (no drag) on a node jumps to its code */
    if (!g->moved && !g->node_drag && g->click_idx >= 0) {
        int i = g->click_idx;
        if (g->file[i]) open_at_line(g->file[i], g->def_line[i]);
        else            goto_line_current(g->def_line[i]);
        gtk_window_present(GTK_WINDOW(app.window));
    }
    g->panning = FALSE; g->node_drag = FALSE;
    g->drag_idx = -1; g->click_idx = -1;
    g->moved = FALSE;
    return TRUE;
}

/* Build the diagram window around an already-populated graph and show it.
   Takes ownership of `g` (freed when the window is destroyed). */
static void show_flow_window(FlowGraph *g, const char *subject)
{
    if (g->n == 0) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(app.window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "No functions were found to diagram.\n"
            "The flow diagram shows the call graph of C/C++ functions.");
        gtk_window_set_title(GTK_WINDOW(dlg), "Flow Diagram");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        on_flow_destroy(NULL, g);
        return;
    }

    /* measure on a throwaway surface so width/height are known */
    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1,1);
    cairo_t *mcr = cairo_create(tmp);
    cairo_select_font_face(mcr, "monospace",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(mcr, 13);
    flow_layout(g, mcr);
    cairo_destroy(mcr);
    cairo_surface_destroy(tmp);

    /* For a large graph, start zoomed out so the whole structure is visible. */
    double fitw = 1040.0 / g->width, fith = 740.0 / g->height;
    double fit = MIN(fitw, fith);
    if (fit > 1.0) fit = 1.0;
    if (fit < 0.2) fit = 0.2;
    g->zoom = fit;

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char *title = g_strdup_printf("Flow Diagram — %s  (%d functions, %d calls)",
        subject ? subject : "untitled", g->n, g->ne);
    gtk_window_set_title(GTK_WINDOW(win), title);
    g_free(title);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(app.window));
    gtk_window_set_default_size(GTK_WINDOW(win),
        (int)CLAMP(g->width * g->zoom + 40, 400, 1100),
        (int)CLAMP(g->height * g->zoom + 90, 300, 800));

    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), vb);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 6);

    GtkWidget *zin   = gtk_button_new_from_icon_name("zoom-in",
                                                     GTK_ICON_SIZE_BUTTON);
    GtkWidget *zout  = gtk_button_new_from_icon_name("zoom-out",
                                                     GTK_ICON_SIZE_BUTTON);
    GtkWidget *zrst  = gtk_button_new_from_icon_name("zoom-original",
                                                     GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(zin,  "Zoom in (Ctrl+scroll)");
    gtk_widget_set_tooltip_text(zout, "Zoom out (Ctrl+scroll)");
    gtk_widget_set_tooltip_text(zrst, "Reset zoom");
    g_signal_connect(zin,  "clicked", G_CALLBACK(on_zoom_in),    g);
    g_signal_connect(zout, "clicked", G_CALLBACK(on_zoom_out),   g);
    g_signal_connect(zrst, "clicked", G_CALLBACK(on_zoom_reset), g);
    gtk_box_pack_start(GTK_BOX(bar), zout, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), zrst, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), zin,  FALSE, FALSE, 0);

    GtkWidget *compact = gtk_toggle_button_new_with_label("Compact");
    gtk_widget_set_tooltip_text(compact,
        "Show only function signatures for a clearer overview");
    g_signal_connect(compact, "toggled", G_CALLBACK(on_flow_compact), g);
    gtk_box_pack_start(GTK_BOX(bar), compact, FALSE, FALSE, 0);

    GtkWidget *hint = gtk_label_new(
        "drag to pan · click a box to jump · Shift+drag to move a box · "
        "Ctrl+scroll to zoom");
    gtk_box_pack_start(GTK_BOX(bar), hint, FALSE, FALSE, 8);

    GtkWidget *save = gtk_button_new_with_label("Save as PNG…");
    g_signal_connect(save, "clicked", G_CALLBACK(on_flow_save), g);
    gtk_box_pack_end(GTK_BOX(bar), save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vb), bar, FALSE, FALSE, 0);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *area = gtk_drawing_area_new();
    g->area = area;
    gtk_widget_set_size_request(area, (int)(g->width * g->zoom),
                                      (int)(g->height * g->zoom));
    gtk_widget_add_events(area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                GDK_POINTER_MOTION_MASK | GDK_BUTTON1_MOTION_MASK |
                                GDK_SCROLL_MASK);
    g_signal_connect(area, "draw",                 G_CALLBACK(on_flow_draw), g);
    g_signal_connect(area, "button-press-event",   G_CALLBACK(on_flow_press), g);
    g_signal_connect(area, "motion-notify-event",  G_CALLBACK(on_flow_motion), g);
    g_signal_connect(area, "button-release-event", G_CALLBACK(on_flow_release), g);
    g_signal_connect(area, "scroll-event",         G_CALLBACK(on_flow_scroll), g);
    gtk_container_add(GTK_CONTAINER(sw), area);
    gtk_box_pack_start(GTK_BOX(vb), sw, TRUE, TRUE, 0);

    g_signal_connect(win, "destroy", G_CALLBACK(on_flow_destroy), g);
    gtk_widget_show_all(win);
}

static void act_flow_diagram(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    Document *doc = current_document();
    if (!doc) return;
    GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char *text = gtk_text_buffer_get_text(b, &s, &e, FALSE);

    FlowGraph *g = g_new0(FlowGraph, 1);
    build_call_graph2(doc->path, text, g);
    g_free(text);
    show_flow_window(g, doc->path ? document_basename(doc) : "untitled");
}

/* Flow diagram across every C/C++ file in a directory. */
/* Find an open document by path, so the diagram can use its live (possibly
   unsaved) buffer contents instead of what is on disk. */
static Document *find_open_doc(const char *path)
{
    gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.notebook));
    for (gint i = 0; i < n; i++) {
        GtkWidget *ch = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.notebook), i);
        Document *dc = g_object_get_data(G_OBJECT(ch), "document");
        if (dc && dc->path && g_strcmp0(dc->path, path) == 0) return dc;
    }
    return NULL;
}

static void act_flow_diagram_dir(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    Document *doc = current_document();

    /* Use the directory of the currently open file. */
    if (!doc || !doc->path) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(app.window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "Please save the current file first.\n"
            "The directory flow diagram scans the folder of the open file.");
        gtk_window_set_title(GTK_WINDOW(dlg), "Flow Diagram (Directory)");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        return;
    }
    char *dir = g_path_get_dirname(doc->path);

    const char *exts[] = {".c",".h",".cpp",".cc",".cxx",".hpp",".hh",".hxx",NULL};
    FlowGraph *g = g_new0(FlowGraph, 1);
    FlowBuild fb;
    flow_build_init(&fb, g);

    int files = 0;
    GDir *gd = g_dir_open(dir, 0, NULL);
    const char *name;
    while (gd && (name = g_dir_read_name(gd))) {
        gboolean ok = FALSE;
        for (int i = 0; exts[i]; i++)
            if (g_str_has_suffix(name, exts[i])) { ok = TRUE; break; }
        if (!ok) continue;
        char *full = g_build_filename(dir, name, NULL);

        /* prefer the live buffer if this file is open (reflects edits) */
        Document *od = find_open_doc(full);
        if (od) {
            GtkTextBuffer *b = GTK_TEXT_BUFFER(od->buffer);
            GtkTextIter s, e;
            gtk_text_buffer_get_bounds(b, &s, &e);
            char *content = gtk_text_buffer_get_text(b, &s, &e, FALSE);
            flow_add_file(&fb, full, content);
            g_free(content);
            files++;
        } else {
            char *content = NULL;
            if (g_file_get_contents(full, &content, NULL, NULL) &&
                g_utf8_validate(content, -1, NULL)) {
                flow_add_file(&fb, full, content);
                files++;
            }
            g_free(content);
        }
        g_free(full);
    }
    if (gd) g_dir_close(gd);
    flow_build_finish(&fb);

    char *base = g_path_get_basename(dir);
    char *subject = g_strdup_printf("%s/  (%d files)", base, files);
    show_flow_window(g, subject);
    g_free(subject); g_free(base); g_free(dir);
}

/* ===================================================================== */
/*  Detailed flow diagram: control flow + variable processing            */
/* ===================================================================== */

#define SF_MAX 600
#define SF_EDGES (SF_MAX*3)
enum { SF_START, SF_END, SF_PROCESS, SF_ASSIGN, SF_DECISION, SF_LOOP,
       SF_RETURN, SF_LABEL, SF_MERGE };

typedef struct {
    char   *text[SF_MAX];
    int     type[SF_MAX];
    int     line[SF_MAX];
    int     n;
    double  x[SF_MAX], y[SF_MAX], w[SF_MAX], h[SF_MAX];
    /* connectors */
    int     e_from[SF_EDGES], e_to[SF_EDGES], e_back[SF_EDGES];
    char   *e_label[SF_EDGES];
    int     ne;
    double  width, height;
    gboolean built;                /* layout already computed by the AST  */
    char   *file;
    GtkWidget *area;
    double  zoom;
    double  press_x, press_y;
    gboolean panning;
    double  pan_rx, pan_ry, pan_sx, pan_sy;
} StepFlow;

#define SF_VGAP 26.0
#define SF_HGAP 46.0

static gboolean has_assign(const char *s)
{
    for (const char *p = s; *p; p++)
        if (*p == '=') {
            char prev = (p > s) ? p[-1] : 0, next = p[1];
            if (next == '=') { continue; }
            if (prev=='='||prev=='!'||prev=='<'||prev=='>'||prev=='~') continue;
            return TRUE;
        }
    return FALSE;
}

static int classify_stmt(const char *s)
{
    while (*s==' '||*s=='\t') s++;
    char fw[32]; int i = 0; const char *p = s;
    while ((g_ascii_isalnum(*p)||*p=='_') && i < 31) fw[i++] = *p++;
    fw[i] = 0;
    if (!strcmp(fw,"if")||!strcmp(fw,"switch")) return SF_DECISION;
    if (!strcmp(fw,"else")) {
        const char *q = p; while (*q==' '||*q=='\t') q++;
        return !strncmp(q,"if",2) ? SF_DECISION : SF_LABEL;
    }
    if (!strcmp(fw,"for")||!strcmp(fw,"while")||!strcmp(fw,"do")) return SF_LOOP;
    if (!strcmp(fw,"case")||!strcmp(fw,"default")) return SF_LABEL;
    if (!strcmp(fw,"return")) return SF_RETURN;
    if (has_assign(s)) return SF_ASSIGN;
    return SF_PROCESS;
}

static char *sf_trunc(const char *text)
{
    char *t = collapse_ws(text);
    if (g_utf8_strlen(t, -1) > 72) {
        char buf[400]; g_utf8_strncpy(buf, t, 72);
        g_free(t); t = g_strconcat(buf, "…", NULL);
    }
    return t;
}

/* ---- recursive control-flow AST --------------------------------------- */
enum { AK_SEQ, AK_STMT, AK_RETURN, AK_IF, AK_LOOP };
typedef struct AstN {
    int kind, stype, line;
    char *text;
    struct AstN **seq; int nseq;         /* AK_SEQ  */
    struct AstN *thenb, *elseb, *body;   /* AK_IF / AK_LOOP */
    double w, h, sx, sy, nw, nh, rx, ry; /* layout  */
    int entry, exit_[6], nexit;
} AstN;

static AstN *ast_new(int kind){ AstN *n=g_new0(AstN,1); n->kind=kind; return n; }
static void seq_add(AstN *s, AstN *c){
    s->seq=g_realloc(s->seq,sizeof(AstN*)*(s->nseq+1)); s->seq[s->nseq++]=c;
}
static AstN *mk_simple(const char *text, int line){
    int ty=classify_stmt(text);
    AstN *n=ast_new(ty==SF_RETURN?AK_RETURN:AK_STMT);
    n->text=g_strdup(text); n->line=line; n->stype=ty; return n;
}
static void free_ast(AstN *n){
    if(!n)return;
    for(int i=0;i<n->nseq;i++) free_ast(n->seq[i]);
    g_free(n->seq); free_ast(n->thenb); free_ast(n->elseb); free_ast(n->body);
    g_free(n->text); g_free(n);
}

typedef struct { const char *t; int pos, len, line; } P;
static void p_skip(P *p){
    while(p->pos<p->len){
        char c=p->t[p->pos];
        if(c=='\n'){p->line++;p->pos++;}
        else if(g_ascii_isspace(c))p->pos++;
        else if(c=='/'&&p->pos+1<p->len&&p->t[p->pos+1]=='/'){ while(p->pos<p->len&&p->t[p->pos]!='\n')p->pos++; }
        else if(c=='/'&&p->pos+1<p->len&&p->t[p->pos+1]=='*'){ p->pos+=2; while(p->pos+1<p->len&&!(p->t[p->pos]=='*'&&p->t[p->pos+1]=='/')){if(p->t[p->pos]=='\n')p->line++;p->pos++;} p->pos+=2; }
        else break;
    }
}
/* read a statement up to a top-level ; { or } (consumed); returns its text */
static char *p_stmt(P *p, char *term, int *sline){
    p_skip(p); *sline=p->line;
    GString *b=g_string_new(NULL); int paren=0;
    while(p->pos<p->len){
        char c=p->t[p->pos];
        if(c=='/'&&p->pos+1<p->len&&p->t[p->pos+1]=='/'){ while(p->pos<p->len&&p->t[p->pos]!='\n')p->pos++; continue; }
        if(c=='/'&&p->pos+1<p->len&&p->t[p->pos+1]=='*'){ p->pos+=2; while(p->pos+1<p->len&&!(p->t[p->pos]=='*'&&p->t[p->pos+1]=='/')){if(p->t[p->pos]=='\n')p->line++;p->pos++;} p->pos+=2; continue; }
        if(c=='"'||c=='\''){ char q=c; g_string_append_c(b,c); p->pos++;
            while(p->pos<p->len&&p->t[p->pos]!=q){ if(p->t[p->pos]=='\\'){g_string_append_c(b,p->t[p->pos]);p->pos++; if(p->pos<p->len){g_string_append_c(b,p->t[p->pos]);p->pos++;} continue;} if(p->t[p->pos]=='\n')p->line++; g_string_append_c(b,p->t[p->pos]); p->pos++; }
            if(p->pos<p->len){g_string_append_c(b,p->t[p->pos]);p->pos++;} continue; }
        if(c=='\n')p->line++;
        if(c=='(')paren++; else if(c==')'){if(paren>0)paren--;}
        if(paren==0&&(c=='{'||c=='}'||c==';')){ *term=c; p->pos++; char*r=g_strstrip(g_strdup(b->str)); g_string_free(b,TRUE); return r; }
        g_string_append_c(b,c); p->pos++;
    }
    *term=0; char*r=g_strstrip(g_strdup(b->str)); g_string_free(b,TRUE); return r;
}
static AstN *parse_seq(P *p);
static gboolean p_peek_word(P *p, const char *w){
    p_skip(p); int l=strlen(w);
    if(p->pos+l>p->len) return FALSE;
    if(strncmp(p->t+p->pos,w,l)) return FALSE;
    char after = (p->pos+l<p->len)?p->t[p->pos+l]:0;
    return !(g_ascii_isalnum(after)||after=='_');
}
static AstN *parse_if(P *p, const char *header, int line){
    AstN *node=ast_new(AK_IF); node->text=g_strdup(header); node->line=line;
    node->thenb=parse_seq(p);
    int save=p->pos, sl=p->line;
    if(p_peek_word(p,"else")){
        p->pos+=4; p_skip(p);
        if(p->pos<p->len && p->t[p->pos]=='{'){ p->pos++; node->elseb=parse_seq(p); }
        else {
            char t2; int l2; char *st2=p_stmt(p,&t2,&l2);
            AstN *es=ast_new(AK_SEQ);
            if(t2=='{' && classify_stmt(st2)==SF_DECISION) seq_add(es, parse_if(p,st2,l2));
            else if(t2=='{'){ if(*st2)seq_add(es,mk_simple(st2,l2)); AstN*in=parse_seq(p); for(int k=0;k<in->nseq;k++)seq_add(es,in->seq[k]); g_free(in->seq); g_free(in); }
            else if(*st2) seq_add(es,mk_simple(st2,l2));
            g_free(st2); node->elseb=es;
        }
    } else { p->pos=save; p->line=sl; }
    return node;
}
static AstN *parse_seq(P *p){
    AstN *seq=ast_new(AK_SEQ);
    for(;;){
        p_skip(p); if(p->pos>=p->len) break;
        if(p->t[p->pos]=='}'){ p->pos++; break; }
        char term; int line; char *st=p_stmt(p,&term,&line);
        if(term=='}'){ if(*st) seq_add(seq,mk_simple(st,line)); g_free(st); break; }
        if(term=='{'){
            int ty=classify_stmt(st);
            if(ty==SF_DECISION) seq_add(seq, parse_if(p,st,line));
            else if(ty==SF_LOOP){ AstN*n=ast_new(AK_LOOP); n->text=g_strdup(st); n->line=line; n->body=parse_seq(p); seq_add(seq,n); }
            else { if(*st)seq_add(seq,mk_simple(st,line)); AstN*in=parse_seq(p); for(int k=0;k<in->nseq;k++)seq_add(seq,in->seq[k]); g_free(in->seq); g_free(in); }
            g_free(st);
        } else { if(*st) seq_add(seq,mk_simple(st,line)); g_free(st); }
        if(seq->nseq > SF_MAX-8) break;
    }
    return seq;
}

static void ast_measure(AstN *n, cairo_t *cr){
    switch(n->kind){
      case AK_STMT: case AK_RETURN: {
        double w=text_width(cr,n->text,13)+34; if(w<90)w=90; if(w>560)w=560;
        n->nw=w; n->nh=34; n->w=w; n->h=34; break; }
      case AK_SEQ: {
        if(n->nseq==0){ n->w=12; n->h=0; break; }
        double w=0,y=0;
        for(int i=0;i<n->nseq;i++){ ast_measure(n->seq[i],cr); if(n->seq[i]->w>w)w=n->seq[i]->w; }
        for(int i=0;i<n->nseq;i++){ n->seq[i]->rx=(w-n->seq[i]->w)/2; n->seq[i]->ry=y; y+=n->seq[i]->h+SF_VGAP; }
        n->w=w; n->h=y-SF_VGAP; break; }
      case AK_IF: {
        double dnw=text_width(cr,n->text,13)+44; if(dnw<120)dnw=120; if(dnw>560)dnw=560;
        n->nw=dnw; n->nh=44;
        ast_measure(n->thenb,cr); if(n->elseb) ast_measure(n->elseb,cr);
        double leftW=n->thenb->w, rightW=n->elseb?n->elseb->w:70;
        double bodyW=leftW+SF_HGAP+rightW, w=MAX(dnw,bodyW), bodyX=(w-bodyW)/2;
        n->thenb->rx=bodyX; n->thenb->ry=n->nh+SF_VGAP*2;
        double Tentry=n->thenb->rx+n->thenb->w/2, Eentry;
        if(n->elseb){ n->elseb->rx=bodyX+leftW+SF_HGAP; n->elseb->ry=n->nh+SF_VGAP*2; Eentry=n->elseb->rx+n->elseb->w/2; }
        else Eentry=bodyX+leftW+SF_HGAP+rightW/2;
        double dcx=(Tentry+Eentry)/2; n->sx=dcx-dnw/2; n->sy=0;
        double bodyH=MAX(n->thenb->h, n->elseb?n->elseb->h:0);
        n->w=w; n->h=n->nh+SF_VGAP*2+bodyH+SF_VGAP*2; break; }
      case AK_LOOP: {
        double hnw=text_width(cr,n->text,13)+50; if(hnw<120)hnw=120; if(hnw>560)hnw=560;
        n->nw=hnw; n->nh=44; ast_measure(n->body,cr);
        double w=MAX(hnw,n->body->w)+34;
        n->body->rx=0; n->body->ry=n->nh+SF_VGAP*2;
        double dcx=MAX(hnw/2, n->body->rx+n->body->w/2);
        n->sx=dcx-hnw/2; n->sy=0;
        n->w=w; n->h=n->nh+SF_VGAP*2+n->body->h+SF_VGAP*2; break; }
    }
}

static int sf_mk(StepFlow *sf,int type,const char *text,int line,double x,double y,double w,double h){
    if(sf->n>=SF_MAX) return sf->n-1;
    int i=sf->n++;
    sf->text[i]=text?sf_trunc(text):g_strdup("");
    sf->type[i]=type; sf->line[i]=line; sf->x[i]=x; sf->y[i]=y; sf->w[i]=w; sf->h[i]=h;
    return i;
}
static void sf_edge(StepFlow *sf,int a,int b,const char *lab,int back){
    if(a<0||b<0||sf->ne>=SF_EDGES)return; int i=sf->ne++;
    sf->e_from[i]=a; sf->e_to[i]=b; sf->e_label[i]=lab?g_strdup(lab):NULL; sf->e_back[i]=back;
}
static void ast_place(AstN *n,double ax,double ay,StepFlow *sf){
    switch(n->kind){
      case AK_STMT: { int i=sf_mk(sf,n->stype,n->text,n->line,ax,ay,n->nw,n->nh); n->entry=i; n->exit_[0]=i; n->nexit=1; break; }
      case AK_RETURN: { int i=sf_mk(sf,SF_RETURN,n->text,n->line,ax,ay,n->nw,n->nh); n->entry=i; n->nexit=0; break; }
      case AK_SEQ: {
        if(n->nseq==0){ int i=sf_mk(sf,SF_MERGE,"",0,ax+n->w/2-5,ay,10,10); n->entry=i; n->exit_[0]=i; n->nexit=1; break; }
        for(int k=0;k<n->nseq;k++) ast_place(n->seq[k],ax+n->seq[k]->rx,ay+n->seq[k]->ry,sf);
        n->entry=n->seq[0]->entry;
        for(int k=0;k+1<n->nseq;k++){ AstN*a=n->seq[k],*b=n->seq[k+1]; for(int e=0;e<a->nexit;e++) sf_edge(sf,a->exit_[e],b->entry,NULL,0); }
        AstN*last=n->seq[n->nseq-1]; n->nexit=last->nexit;
        for(int e=0;e<last->nexit;e++) n->exit_[e]=last->exit_[e];
        break; }
      case AK_IF: {
        int d=sf_mk(sf,SF_DECISION,n->text,n->line,ax+n->sx,ay+n->sy,n->nw,n->nh); n->entry=d; n->nexit=0;
        ast_place(n->thenb,ax+n->thenb->rx,ay+n->thenb->ry,sf);
        sf_edge(sf,d,n->thenb->entry,"T",0);
        for(int e=0;e<n->thenb->nexit&&n->nexit<6;e++) n->exit_[n->nexit++]=n->thenb->exit_[e];
        if(n->elseb){ ast_place(n->elseb,ax+n->elseb->rx,ay+n->elseb->ry,sf); sf_edge(sf,d,n->elseb->entry,"F",0);
            for(int e=0;e<n->elseb->nexit&&n->nexit<6;e++) n->exit_[n->nexit++]=n->elseb->exit_[e]; }
        else if(n->nexit<6) n->exit_[n->nexit++]=d;   /* false path continues */
        break; }
      case AK_LOOP: {
        int hidx=sf_mk(sf,SF_LOOP,n->text,n->line,ax+n->sx,ay+n->sy,n->nw,n->nh); n->entry=hidx;
        ast_place(n->body,ax+n->body->rx,ay+n->body->ry,sf);
        sf_edge(sf,hidx,n->body->entry,"T",0);
        for(int e=0;e<n->body->nexit;e++) sf_edge(sf,n->body->exit_[e],hidx,NULL,1);
        n->exit_[0]=hidx; n->nexit=1; break; }
    }
}

static void build_detailed(StepFlow *sf, const char *body, int base_line, cairo_t *cr)
{
    P p={body,0,(int)strlen(body),base_line};
    AstN *prog=parse_seq(&p);
    ast_measure(prog,cr);

    double margin=30, startH=30;
    int startIdx=sf_mk(sf,SF_START,"START",base_line,margin,margin,90,startH);
    double progY=margin+startH+SF_VGAP;
    ast_place(prog, margin, progY, sf);
    sf_edge(sf,startIdx,prog->entry,NULL,0);

    double entryx=sf->x[prog->entry]+sf->w[prog->entry]/2;
    sf->x[startIdx]=entryx-45;
    double maxy=0; for(int i=0;i<sf->n;i++) if(sf->y[i]+sf->h[i]>maxy)maxy=sf->y[i]+sf->h[i];
    int endIdx=sf_mk(sf,SF_END,"END",base_line,entryx-45,maxy+SF_VGAP,90,30);
    for(int e=0;e<prog->nexit;e++) sf_edge(sf,prog->exit_[e],endIdx,NULL,0);

    double W=0,H=0; for(int i=0;i<sf->n;i++){ if(sf->x[i]+sf->w[i]>W)W=sf->x[i]+sf->w[i]; if(sf->y[i]+sf->h[i]>H)H=sf->y[i]+sf->h[i]; }
    sf->width=W+margin; sf->height=H+margin; sf->built=TRUE;
    free_ast(prog);
}

static void sf_shape(cairo_t *cr, int type, double x, double y, double w, double h)
{
    if (type==SF_DECISION) {
        cairo_move_to(cr,x,y+h/2); cairo_line_to(cr,x+w/2,y);
        cairo_line_to(cr,x+w,y+h/2); cairo_line_to(cr,x+w/2,y+h);
        cairo_close_path(cr);
    } else if (type==SF_LOOP) {
        double in=16;
        cairo_move_to(cr,x+in,y); cairo_line_to(cr,x+w-in,y);
        cairo_line_to(cr,x+w,y+h/2); cairo_line_to(cr,x+w-in,y+h);
        cairo_line_to(cr,x+in,y+h); cairo_line_to(cr,x,y+h/2);
        cairo_close_path(cr);
    } else {   /* rounded rectangle (pill for START/END/RETURN) */
        double r = (type==SF_START||type==SF_END||type==SF_RETURN) ? h/2 : 6;
        cairo_new_sub_path(cr);
        cairo_arc(cr,x+w-r,y+r,r,-G_PI_2,0);
        cairo_arc(cr,x+w-r,y+h-r,r,0,G_PI_2);
        cairo_arc(cr,x+r,y+h-r,r,G_PI_2,G_PI);
        cairo_arc(cr,x+r,y+r,r,G_PI,1.5*G_PI);
        cairo_close_path(cr);
    }
}

static gboolean sf_draw(GtkWidget *area, cairo_t *cr, gpointer data)
{
    StepFlow *sf = data; (void)area;
    cairo_set_source_rgb(cr,1,1,1); cairo_paint(cr);
    double z = sf->zoom>0?sf->zoom:1.0; cairo_scale(cr,z,z);
    cairo_select_font_face(cr,"monospace",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr,13);

    /* connectors (from the edge list) */
    cairo_set_line_width(cr,1.5);
    for (int k=0;k<sf->ne;k++) {
        int a=sf->e_from[k], b=sf->e_to[k];
        double acx=sf->x[a]+sf->w[a]/2, aby=sf->y[a]+sf->h[a];
        double bcx=sf->x[b]+sf->w[b]/2, bty=sf->y[b];
        cairo_set_source_rgb(cr,0.45,0.5,0.55);
        if (sf->e_back[k]) {
            /* loop back-edge: route along the right side up to the header */
            double lane = sf->x[b]+sf->w[b] + 20;
            double bry = sf->y[b]+sf->h[b]/2;
            double afx = sf->x[a]+sf->w[a]/2, afy=sf->y[a]+sf->h[a];
            cairo_move_to(cr,afx,afy);
            cairo_line_to(cr,lane,afy);
            cairo_line_to(cr,lane,bry);
            cairo_line_to(cr,sf->x[b]+sf->w[b],bry);
            cairo_stroke(cr);
            double ex=sf->x[b]+sf->w[b];
            cairo_move_to(cr,ex,bry); cairo_line_to(cr,ex+7,bry-4);
            cairo_move_to(cr,ex,bry); cairo_line_to(cr,ex+7,bry+4); cairo_stroke(cr);
        } else {
            cairo_move_to(cr,acx,aby);
            if (fabs(acx-bcx)<1) cairo_line_to(cr,bcx,bty);
            else { double my=(aby+bty)/2; cairo_line_to(cr,acx,my); cairo_line_to(cr,bcx,my); cairo_line_to(cr,bcx,bty); }
            cairo_stroke(cr);
            cairo_move_to(cr,bcx,bty); cairo_line_to(cr,bcx-4,bty-7);
            cairo_move_to(cr,bcx,bty); cairo_line_to(cr,bcx+4,bty-7); cairo_stroke(cr);
        }
        if (sf->e_label[k]) {
            cairo_set_source_rgb(cr,0.3,0.3,0.35);
            double lx = (acx+bcx)/2 + 5, ly = aby + 13;
            cairo_move_to(cr, lx, ly);
            cairo_show_text(cr, sf->e_label[k]);
        }
    }

    for (int i=0;i<sf->n;i++) {
        if (sf->type[i]==SF_MERGE) continue;
        int t=sf->type[i];
        double x=sf->x[i],y=sf->y[i],w=sf->w[i],h=sf->h[i];
        double fr,fg,fb_,sr,sg,sb;
        switch(t){
          case SF_START: case SF_END: fr=0.85;fg=0.88;fb_=0.92; sr=0.3;sg=0.35;sb=0.4; break;
          case SF_ASSIGN:  fr=0.88;fg=0.96;fb_=0.88; sr=0.2;sg=0.55;sb=0.3; break;
          case SF_DECISION:fr=0.99;fg=0.95;fb_=0.80; sr=0.75;sg=0.55;sb=0.1; break;
          case SF_LOOP:    fr=0.99;fg=0.90;fb_=0.80; sr=0.80;sg=0.45;sb=0.1; break;
          case SF_RETURN:  fr=0.99;fg=0.88;fb_=0.88; sr=0.75;sg=0.2;sb=0.2; break;
          case SF_LABEL:   fr=1;fg=1;fb_=1; sr=0.6;sg=0.6;sb=0.6; break;
          default:         fr=0.90;fg=0.94;fb_=0.99; sr=0.21;sg=0.42;sb=0.6; break;
        }
        if (t!=SF_LABEL) {
            sf_shape(cr,t,x,y,w,h);
            cairo_set_source_rgb(cr,fr,fg,fb_); cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr,sr,sg,sb); cairo_set_line_width(cr,1.5); cairo_stroke(cr);
        }
        cairo_set_source_rgb(cr, t==SF_LABEL?0.5:0.12, t==SF_LABEL?0.5:0.12, t==SF_LABEL?0.5:0.15);
        cairo_text_extents_t te; cairo_text_extents(cr,sf->text[i],&te);
        cairo_move_to(cr, x+(w-te.width)/2-te.x_bearing, y+(h-te.height)/2-te.y_bearing);
        cairo_show_text(cr,sf->text[i]);
    }
    return FALSE;
}

static int sf_hit(StepFlow *sf, double x, double y)
{
    for (int i=sf->n-1;i>=0;i--)
        if (x>=sf->x[i]&&x<=sf->x[i]+sf->w[i]&&y>=sf->y[i]&&y<=sf->y[i]+sf->h[i])
            return i;
    return -1;
}
static gboolean sf_press(GtkWidget *a, GdkEventButton *ev, gpointer d)
{
    StepFlow *sf=d; if(ev->button!=1)return FALSE;
    sf->press_x=ev->x; sf->press_y=ev->y;
    sf->panning=TRUE; sf->pan_rx=ev->x_root; sf->pan_ry=ev->y_root;
    GtkWidget *sw=gtk_widget_get_ancestor(a,GTK_TYPE_SCROLLED_WINDOW);
    if(sw){ sf->pan_sx=gtk_adjustment_get_value(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(sw)));
            sf->pan_sy=gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw))); }
    set_area_cursor(a,"grabbing");
    return TRUE;
}
static gboolean sf_motion(GtkWidget *a, GdkEventMotion *ev, gpointer d)
{
    StepFlow *sf=d;
    if (!(ev->state & GDK_BUTTON1_MASK)) {
        int over=sf_hit(sf,ev->x/sf->zoom,ev->y/sf->zoom);
        set_area_cursor(a, over>=0 ? "pointer" : "grab");
        return FALSE;
    }
    if (sf->panning) {
        pan_view(a, sf->pan_sx, sf->pan_sy, sf->pan_rx, sf->pan_ry,
                 ev->x_root, ev->y_root);
        return TRUE;
    }
    return FALSE;
}
static gboolean sf_release(GtkWidget *a, GdkEventButton *ev, gpointer d)
{
    StepFlow *sf=d; if(ev->button!=1)return FALSE;
    sf->panning=FALSE; set_area_cursor(a,"grab");
    if (fabs(ev->x_root-sf->pan_rx)<4 && fabs(ev->y_root-sf->pan_ry)<4) {
        int i=sf_hit(sf,ev->x/sf->zoom,ev->y/sf->zoom);
        if (i>=0) {
            if (sf->file) open_at_line(sf->file, sf->line[i]);
            else          goto_line_current(sf->line[i]);
            gtk_window_present(GTK_WINDOW(app.window));
        }
    }
    return TRUE;
}
static void sf_zoom(StepFlow *sf, double newz, double px, double py)
{ if(sf->area) zoom_about(sf->area, &sf->zoom, newz, sf->width, sf->height, px, py); }
static void sf_zin (GtkButton *b,gpointer d){(void)b;StepFlow*sf=d;sf_zoom(sf,MIN(sf->zoom*1.2,4.0),-1,-1);}
static void sf_zout(GtkButton *b,gpointer d){(void)b;StepFlow*sf=d;sf_zoom(sf,MAX(sf->zoom/1.2,0.25),-1,-1);}
static void sf_zrst(GtkButton *b,gpointer d){(void)b;StepFlow*sf=d;sf_zoom(sf,1.0,-1,-1);}
static gboolean sf_scroll(GtkWidget *a,GdkEventScroll *ev,gpointer d)
{ StepFlow*sf=d; if(!(ev->state&GDK_CONTROL_MASK))return scroll_enclosing(a,ev);
  double nz;
  if(ev->direction==GDK_SCROLL_UP)nz=MIN(sf->zoom*1.1,4.0);
  else if(ev->direction==GDK_SCROLL_DOWN)nz=MAX(sf->zoom/1.1,0.25);
  else if(ev->direction==GDK_SCROLL_SMOOTH){ double dx,dy; gdk_event_get_scroll_deltas((GdkEvent*)ev,&dx,&dy); nz=CLAMP(sf->zoom*(1.0-dy*0.1),0.25,4.0); }
  else return TRUE;
  sf_zoom(sf,nz,ev->x,ev->y); return TRUE; }
static void sf_save(GtkButton *b,gpointer d)
{
    (void)b; StepFlow *sf=d;
    GtkWidget *dlg=gtk_file_chooser_dialog_new("Save Flowchart as PNG",
        GTK_WINDOW(app.window),GTK_FILE_CHOOSER_ACTION_SAVE,
        "Cancel",GTK_RESPONSE_CANCEL,"Save",GTK_RESPONSE_ACCEPT,NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg),"flowchart.png");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg),TRUE);
    if (gtk_dialog_run(GTK_DIALOG(dlg))==GTK_RESPONSE_ACCEPT) {
        char *path=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        double zp=sf->zoom; sf->zoom=1.0;
        cairo_surface_t *s=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,(int)sf->width,(int)sf->height);
        cairo_t *cr=cairo_create(s); sf_draw(NULL,cr,sf);
        cairo_surface_write_to_png(s,path); cairo_destroy(cr); cairo_surface_destroy(s);
        sf->zoom=zp; g_free(path);
    }
    gtk_widget_destroy(dlg);
}
static void sf_destroy(GtkWidget *w,gpointer d)
{ (void)w; StepFlow *sf=d;
  for(int i=0;i<sf->n;i++)g_free(sf->text[i]);
  for(int i=0;i<sf->ne;i++)g_free(sf->e_label[i]);
  g_free(sf->file); g_free(sf); }

static void show_stepflow_window(StepFlow *sf, const char *fname)
{
    double fit=MIN(1040.0/sf->width,740.0/sf->height);
    if(fit>1.0)fit=1.0; if(fit<0.2)fit=0.2; sf->zoom=fit;

    GtkWidget *win=gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char *title=g_strdup_printf("Detailed Flow — %s()  (%d steps)", fname, sf->n);
    gtk_window_set_title(GTK_WINDOW(win),title); g_free(title);
    gtk_window_set_transient_for(GTK_WINDOW(win),GTK_WINDOW(app.window));
    gtk_window_set_default_size(GTK_WINDOW(win),
        (int)CLAMP(sf->width*sf->zoom+40,420,900),
        (int)CLAMP(sf->height*sf->zoom+90,320,820));

    GtkWidget *vb=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
    gtk_container_add(GTK_CONTAINER(win),vb);
    GtkWidget *bar=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
    gtk_container_set_border_width(GTK_CONTAINER(bar),6);
    GtkWidget *zo=gtk_button_new_from_icon_name("zoom-out",GTK_ICON_SIZE_BUTTON);
    GtkWidget *zr=gtk_button_new_from_icon_name("zoom-original",GTK_ICON_SIZE_BUTTON);
    GtkWidget *zi=gtk_button_new_from_icon_name("zoom-in",GTK_ICON_SIZE_BUTTON);
    g_signal_connect(zo,"clicked",G_CALLBACK(sf_zout),sf);
    g_signal_connect(zr,"clicked",G_CALLBACK(sf_zrst),sf);
    g_signal_connect(zi,"clicked",G_CALLBACK(sf_zin),sf);
    gtk_box_pack_start(GTK_BOX(bar),zo,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(bar),zr,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(bar),zi,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(bar),gtk_label_new(
        "green = variable processing · yellow = decision · orange = loop · "
        "red = return · click a step to jump to code"),FALSE,FALSE,8);
    GtkWidget *sv=gtk_button_new_with_label("Save as PNG…");
    g_signal_connect(sv,"clicked",G_CALLBACK(sf_save),sf);
    gtk_box_pack_end(GTK_BOX(bar),sv,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(vb),bar,FALSE,FALSE,0);

    GtkWidget *sw=gtk_scrolled_window_new(NULL,NULL);
    GtkWidget *area=gtk_drawing_area_new(); sf->area=area;
    gtk_widget_set_size_request(area,(int)(sf->width*sf->zoom),(int)(sf->height*sf->zoom));
    gtk_widget_add_events(area,GDK_BUTTON_PRESS_MASK|GDK_BUTTON_RELEASE_MASK|
                               GDK_POINTER_MOTION_MASK|GDK_SCROLL_MASK);
    g_signal_connect(area,"draw",G_CALLBACK(sf_draw),sf);
    g_signal_connect(area,"button-press-event",G_CALLBACK(sf_press),sf);
    g_signal_connect(area,"motion-notify-event",G_CALLBACK(sf_motion),sf);
    g_signal_connect(area,"button-release-event",G_CALLBACK(sf_release),sf);
    g_signal_connect(area,"scroll-event",G_CALLBACK(sf_scroll),sf);
    gtk_container_add(GTK_CONTAINER(sw),area);
    gtk_box_pack_start(GTK_BOX(vb),sw,TRUE,TRUE,0);
    g_signal_connect(win,"destroy",G_CALLBACK(sf_destroy),sf);
    gtk_widget_show_all(win);
}

static void act_detailed_flow(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    Document *doc = current_document();
    if (!doc) return;
    GtkTextBuffer *b = GTK_TEXT_BUFFER(doc->buffer);
    GtkTextIter s,e; gtk_text_buffer_get_bounds(b,&s,&e);
    char *text = gtk_text_buffer_get_text(b,&s,&e,FALSE);
    GtkTextIter cur;
    gtk_text_buffer_get_iter_at_mark(b,&cur,gtk_text_buffer_get_insert(b));
    int cur_line = gtk_text_iter_get_line(&cur);   /* 0-based */

    gssize len = strlen(text);
    GRegex *re = g_regex_new("([A-Za-z_]\\w*)\\s*\\(([^()]*)\\)\\s*\\{",
                             G_REGEX_MULTILINE, 0, NULL);
    GMatchInfo *mi = NULL;
    g_regex_match(re, text, 0, &mi);
    gssize best_brace=-1, best_bend=-1; char *best_name=NULL;
    while (g_match_info_matches(mi)) {
        char *nm=g_match_info_fetch(mi,1);
        int st,en; g_match_info_fetch_pos(mi,0,&st,&en);
        if (!is_c_keyword(nm)) {
            gssize ls=st; while(ls>0&&text[ls-1]!='\n')ls--;
            gboolean okrt=TRUE;
            for(gssize p=ls;p<st&&okrt;p++){char c=text[p];
                if(c==')'||c=='('||c==','||c==';'||c=='=')okrt=FALSE;
                else if(c==':'){gboolean sc=(p+1<st&&text[p+1]==':')||(p>ls&&text[p-1]==':'); if(!sc)okrt=FALSE;}}
            gssize brace=en-1, bend=match_brace(text,len,brace);
            if (okrt && bend>brace) {
                int sline=0; for(gssize p=0;p<brace;p++) if(text[p]=='\n')sline++;
                int eline=0; for(gssize p=0;p<bend;p++) if(text[p]=='\n')eline++;
                if (cur_line>=sline && cur_line<=eline) {
                    best_brace=brace; best_bend=bend;
                    g_free(best_name); best_name=g_strdup(nm);
                }
            }
        }
        g_free(nm); g_match_info_next(mi,NULL);
    }
    if(mi)g_match_info_free(mi); g_regex_unref(re);

    if (best_brace<0) {
        g_free(text); g_free(best_name);
        GtkWidget *dlg=gtk_message_dialog_new(GTK_WINDOW(app.window),
            GTK_DIALOG_MODAL,GTK_MESSAGE_INFO,GTK_BUTTONS_OK,
            "Place the cursor inside a C/C++ function.\n"
            "The detailed flow diagram shows that function's control flow "
            "and variable processing.");
        gtk_window_set_title(GTK_WINDOW(dlg),"Detailed Flow");
        gtk_dialog_run(GTK_DIALOG(dlg)); gtk_widget_destroy(dlg);
        return;
    }

    int base_line=1; for(gssize p=0;p<best_brace;p++) if(text[p]=='\n')base_line++;
    char *body=g_strndup(text+best_brace+1, best_bend-(best_brace+1));
    StepFlow *sf=g_new0(StepFlow,1);
    sf->zoom=1.0;
    sf->file = doc->path ? g_strdup(doc->path) : NULL;
    cairo_surface_t *ms=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1,1);
    cairo_t *mcr=cairo_create(ms);
    cairo_select_font_face(mcr,"monospace",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(mcr,13);
    build_detailed(sf, body, base_line, mcr);
    cairo_destroy(mcr); cairo_surface_destroy(ms);
    g_free(body); g_free(text);
    show_stepflow_window(sf, best_name);
    g_free(best_name);
}

/* ===================================================================== */
/*  Symbol tooltips (mouse-over)                                           */
/* ===================================================================== */

/* Look up `word` in the buffer: if it is a function defined there, return its
   signature; if it is a declared variable, return its declaration. NULL if
   nothing is found. Caller frees. */
static char *lookup_symbol_info(GtkTextBuffer *b, const char *word)
{
    if (!word || !*word || is_c_keyword(word)) return NULL;
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char *text = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    char *esc = g_regex_escape_string(word, -1);
    char *info = NULL;

    /* function definition: <rettype> word ( params ) { */
    char *pat = g_strdup_printf(
        "([A-Za-z_][\\w \\t\\*&:<>]*?)\\b%s\\s*\\(([^;{}]*)\\)\\s*\\{", esc);
    GRegex *re = g_regex_new(pat, G_REGEX_MULTILINE, 0, NULL);
    GMatchInfo *mi = NULL;
    if (re && g_regex_match(re, text, 0, &mi)) {
        char *rtf = g_match_info_fetch(mi, 1);
        char *pf  = g_match_info_fetch(mi, 2);
        char *rt = collapse_ws(rtf);
        char *pp = collapse_ws(pf);
        int st, en; g_match_info_fetch_pos(mi, 0, &st, &en);
        int line = 1; for (int i = 0; i < st && text[i]; i++) if (text[i]=='\n') line++;
        info = g_strdup_printf("Function  (line %d)\n%s%s%s(%s)",
            line, rt, *rt ? " " : "", word, *pp ? pp : "void");
        g_free(rtf); g_free(pf); g_free(rt); g_free(pp);
    }
    if (mi) g_match_info_free(mi);
    if (re) g_regex_unref(re);
    g_free(pat);

    /* variable declaration: <type> word (= | ; | [ | ,) */
    if (!info) {
        char *pat2 = g_strdup_printf(
            "(?m)^[ \\t]*([A-Za-z_][\\w \\t\\*&:<>]*?[ \\t\\*&])%s"
            "\\s*(?:\\[[^\\]]*\\])?\\s*(?:=[^=]|;|,)", esc);
        GRegex *re2 = g_regex_new(pat2, 0, 0, NULL);
        GMatchInfo *mi2 = NULL;
        if (re2 && g_regex_match(re2, text, 0, &mi2)) {
            char *tf = g_match_info_fetch(mi2, 1);
            char *ty = collapse_ws(tf);
            char first[32]; g_strlcpy(first, ty, sizeof first);
            char *sp = strchr(first, ' '); if (sp) *sp = 0;
            int st, en; g_match_info_fetch_pos(mi2, 0, &st, &en);
            int line = 1; for (int i = 0; i < st && text[i]; i++) if (text[i]=='\n') line++;
            if (!is_c_keyword(first) && strcmp(first, "return"))
                info = g_strdup_printf("Variable  (line %d)\n%s %s", line, ty, word);
            g_free(tf); g_free(ty);
        }
        if (mi2) g_match_info_free(mi2);
        if (re2) g_regex_unref(re2);
        g_free(pat2);
    }

    g_free(esc);
    g_free(text);
    return info;
}

static gboolean on_query_tooltip(GtkWidget *view, int x, int y, gboolean kbd,
                                 GtkTooltip *tip, gpointer u)
{
    (void)u;
    if (!app.hover_tooltips || kbd) return FALSE;
    GtkTextView *tv = GTK_TEXT_VIEW(view);
    int bx, by;
    gtk_text_view_window_to_buffer_coords(tv, GTK_TEXT_WINDOW_WIDGET,
                                          x, y, &bx, &by);
    GtkTextIter it;
    gtk_text_view_get_iter_at_location(tv, &it, bx, by);
    if (!gtk_text_iter_inside_word(&it) && !gtk_text_iter_ends_word(&it))
        return FALSE;

    GtkTextIter s = it, e = it;
    if (!gtk_text_iter_starts_word(&s)) gtk_text_iter_backward_word_start(&s);
    if (!gtk_text_iter_ends_word(&e))   gtk_text_iter_forward_word_end(&e);
    GtkTextBuffer *b = gtk_text_view_get_buffer(tv);
    char *word = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    char *info = lookup_symbol_info(b, word);
    g_free(word);
    if (!info) return FALSE;

    gtk_tooltip_set_text(tip, info);
    g_free(info);
    return TRUE;
}

/* Ctrl+Shift+Space pops up the auto-completion list. Once it is open, the
   highlighted suggestion is accepted with Enter or Tab (which still work while
   Ctrl+Shift are held). */
static gboolean on_view_key(GtkWidget *view, GdkEventKey *ev, gpointer u)
{
    (void)u;
    guint mods = ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK);
    if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) &&
        (ev->keyval == GDK_KEY_space || ev->keyval == GDK_KEY_Return ||
         ev->keyval == GDK_KEY_KP_Enter)) {
        if (app.autocomplete)
            g_signal_emit_by_name(view, "show-completion");
        return TRUE;
    }
    return FALSE;
}

/* ===================================================================== */
/*  View actions                                                          */
/* ===================================================================== */

static void foreach_view(void (*fn)(Document *doc))
{
    gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.notebook));
    for (gint i = 0; i < n; i++) {
        GtkWidget *ch = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.notebook), i);
        Document *doc = g_object_get_data(G_OBJECT(ch), "document");
        if (doc) fn(doc);
    }
}

static void _apply_wrap(Document *doc)
{
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(doc->view),
        app.word_wrap ? GTK_WRAP_WORD : GTK_WRAP_NONE);
}
static void act_word_wrap(GtkWidget *w, gpointer d)
{ (void)d; app.word_wrap = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(w));
  foreach_view(_apply_wrap); }

static void _apply_lnum(Document *doc)
{ gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(doc->view),
                                        app.show_line_numbers); }
static void act_line_numbers(GtkWidget *w, gpointer d)
{ (void)d; app.show_line_numbers = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(w));
  foreach_view(_apply_lnum); }

static void _apply_tooltips(Document *doc)
{ gtk_widget_set_has_tooltip(doc->view, app.hover_tooltips); }
static void act_tooltips(GtkWidget *w, gpointer d)
{ (void)d; app.hover_tooltips = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(w));
  foreach_view(_apply_tooltips); }

static void _apply_autocomplete(Document *doc)
{
    GtkSourceCompletion *comp =
        gtk_source_view_get_completion(GTK_SOURCE_VIEW(doc->view));
    GtkSourceCompletionProvider *p =
        GTK_SOURCE_COMPLETION_PROVIDER(doc->words);
    if (app.autocomplete) gtk_source_completion_add_provider(comp, p, NULL);
    else                  gtk_source_completion_remove_provider(comp, p, NULL);
}
static void act_autocomplete(GtkWidget *w, gpointer d)
{ (void)d; app.autocomplete = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(w));
  foreach_view(_apply_autocomplete); }

static void act_zoom_in(GtkWidget *w, gpointer d)
{ (void)w;(void)d; if (app.font_size < 72) app.font_size++; apply_font_css(); }
static void act_zoom_out(GtkWidget *w, gpointer d)
{ (void)w;(void)d; if (app.font_size > 4) app.font_size--; apply_font_css(); }
static void act_zoom_reset(GtkWidget *w, gpointer d)
{ (void)w;(void)d; app.font_size = 11; apply_font_css(); }

/* ===================================================================== */
/*  Language and theme menus                                              */
/* ===================================================================== */

static void on_lang_selected(GtkWidget *w, gpointer data)
{
    (void)w;
    Document *doc = current_document();
    if (!doc) return;
    const char *id = data;
    if (id == NULL) {
        gtk_source_buffer_set_language(doc->buffer, NULL);
    } else {
        GtkSourceLanguage *lang =
            gtk_source_language_manager_get_language(app.lm, id);
        gtk_source_buffer_set_language(doc->buffer, lang);
    }
    update_status_bar();
}

static int cmp_str(const void *a, const void *b)
{ return g_strcmp0(*(const char **)a, *(const char **)b); }

static GtkWidget *build_language_menu(void)
{
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *none = gtk_menu_item_new_with_label("Normal text (None)");
    g_signal_connect(none, "activate", G_CALLBACK(on_lang_selected), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), none);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    const gchar * const *ids = gtk_source_language_manager_get_language_ids(app.lm);
    GPtrArray *arr = g_ptr_array_new();
    for (int i = 0; ids && ids[i]; i++)
        g_ptr_array_add(arr, (gpointer)ids[i]);
    qsort(arr->pdata, arr->len, sizeof(gpointer), cmp_str);

    /* Group into A-F / G-L / M-R / S-Z submenus to stay manageable */
    GtkWidget *sub[4] = {0};
    const char *labels[4] = {"A - F", "G - L", "M - R", "S - Z"};
    for (int i = 0; i < 4; i++) {
        GtkWidget *mi = gtk_menu_item_new_with_label(labels[i]);
        sub[i] = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), sub[i]);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    for (guint i = 0; i < arr->len; i++) {
        const char *id = arr->pdata[i];
        GtkSourceLanguage *lang =
            gtk_source_language_manager_get_language(app.lm, id);
        const char *name = gtk_source_language_get_name(lang);
        char c = g_ascii_toupper(name[0]);
        int grp = (c <= 'F') ? 0 : (c <= 'L') ? 1 : (c <= 'R') ? 2 : 3;
        GtkWidget *mi = gtk_menu_item_new_with_label(name);
        g_signal_connect(mi, "activate", G_CALLBACK(on_lang_selected), (gpointer)id);
        gtk_menu_shell_append(GTK_MENU_SHELL(sub[grp]), mi);
    }
    g_ptr_array_free(arr, TRUE);
    return menu;
}

static void on_scheme_selected(GtkWidget *w, gpointer data)
{
    (void)w;
    g_free(app.scheme_id);
    app.scheme_id = g_strdup((const char *)data);
    foreach_view(apply_scheme);
}

static GtkWidget *build_theme_menu(void)
{
    GtkWidget *menu = gtk_menu_new();
    const gchar * const *ids =
        gtk_source_style_scheme_manager_get_scheme_ids(app.ssm);
    GSList *group = NULL;
    for (int i = 0; ids && ids[i]; i++) {
        GtkSourceStyleScheme *sch =
            gtk_source_style_scheme_manager_get_scheme(app.ssm, ids[i]);
        GtkWidget *mi = gtk_radio_menu_item_new_with_label(group,
            gtk_source_style_scheme_get_name(sch));
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(mi));
        if (g_strcmp0(ids[i], app.scheme_id) == 0)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), TRUE);
        g_signal_connect(mi, "activate", G_CALLBACK(on_scheme_selected),
                         (gpointer)ids[i]);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    return menu;
}

/* The complete in-app documentation, shown by Help > Help Contents (F1). */
static const char *HELP_MARKUP =
"<big><b>tuxpad — Help</b></big>\n"
"A Notepad++-style source code editor for Linux, built with GTK+ 3 and "
"GtkSourceView.\n\n"

"<b>Overview</b>\n"
"tuxpad edits several documents in reorderable tabs. A <tt>*</tt> on a tab "
"marks unsaved changes. The status bar shows the text length and line count, "
"the cursor position (Ln/Col), the selection length, the end-of-line style, "
"the encoding, and the detected language.\n\n"

"<b>File</b>\n"
"New (Ctrl+N), Open (Ctrl+O), Save (Ctrl+S), Save As (Ctrl+Shift+S), "
"Save All (Ctrl+Alt+S), Close tab (Ctrl+W), Close All, Print (Ctrl+P), "
"Exit (Ctrl+Q). You can also drag &amp; drop files onto the window, or pass "
"them on the command line.\n\n"

"<b>Edit</b>\n"
"Undo (Ctrl+Z), Redo (Ctrl+Y), Cut (Ctrl+X), Copy (Ctrl+C), Paste (Ctrl+V), "
"Delete, Select All (Ctrl+A).\n\n"

"<b>Search</b>\n"
"• <b>Find</b> (Ctrl+F) and <b>Replace</b> (Ctrl+H) with options: match case, "
"whole word, regular expression, and wrap around.\n"
"• <b>Find Next</b> (F3) / <b>Find Previous</b> (Shift+F3).\n"
"• <b>Find in Files</b> (Ctrl+Shift+F): search a directory (optionally its "
"subdirectories) using the same options and a file filter such as "
"<tt>*.c *.h</tt>. Matches appear in the panel at the bottom; double-click a "
"hit to jump to it.\n"
"• <b>Go to Line</b> (Ctrl+G).\n\n"

"<b>View</b>\n"
"Word wrap, show/hide line numbers, zoom in (Ctrl++), zoom out (Ctrl+-), "
"reset zoom (Ctrl+0), color themes, optional <b>symbol tooltips</b> "
"(hover a function or variable) and optional <b>auto-completion</b> "
"(Ctrl+Space, word completion from the document).\n\n"

"<b>Recent files &amp; session</b>\n"
"File &gt; Open Recent lists the last files you opened. When you start tuxpad "
"with no file arguments, the files from your previous session are reopened.\n\n"

"<b>Tools</b>\n"
"• <b>Format Document</b> (Ctrl+Shift+I): reformats the file using "
"<tt>clang-format</tt>, <tt>astyle</tt>, or <tt>indent</tt> if one is "
"installed.\n"
"• <b>Syntax Check</b> (F7): compiles the current C/C++ buffer with "
"<tt>gcc</tt>/<tt>g++ -fsyntax-only</tt> and lists errors and warnings in the "
"bottom panel; double-click a message to jump to the line.\n"
"• <b>Flow Diagram</b> (F8): draws the call graph of the C/C++ functions. Each "
"box shows the function's signature (return type, name, parameters) and, below "
"it, the parameters, detected local variables, and return values; arrows show "
"which function calls which. The layout is arranged automatically. In the "
"diagram window you can:\n"
"    – <b>drag</b> a box to move it,\n"
"    – <b>click</b> a box to jump to that function in the editor,\n"
"    – <b>zoom</b> with the toolbar buttons or Ctrl+scroll,\n"
"    – <b>Save as PNG</b>.\n"
"• <b>Flow Diagram (Directory)</b> (Shift+F8): the same, but combining every "
"C/C++ file in the folder of the currently open file (cross-file calls "
"included; open files use their live, unsaved contents).\n"
"• <b>Detailed Flow</b> (F9): a flowchart of the function at the cursor showing "
"the control-flow structure (decisions, loops, returns) and variable "
"processing (assignments in green). Click a step to jump to its line.\n\n"

"<b>Language</b>\n"
"The language is detected automatically from the file name or its content; you "
"can also set it manually from the Language menu.\n\n"

"<b>Plugins</b>\n"
"Plugins add commands to the Plugins menu. They are shared objects loaded at "
"startup from <tt>plugins/</tt> next to the binary, "
"<tt>&lt;prefix&gt;/lib/tuxpad/plugins/</tt>, and "
"<tt>~/.config/tuxpad/plugins/</tt>. The bundled <i>sample_tools</i> plugin "
"adds UPPERCASE/lowercase conversion, Insert Date &amp; Time, and Word Count.\n\n"

"<b>Command line</b>\n"
"<tt>tuxpad [files…]</tt> opens files (a non-existent path starts a new named "
"tab). <tt>tuxpad --flow-png &lt;source&gt; &lt;out.png&gt;</tt> writes a flow "
"diagram to a PNG without opening a window.\n\n"

"<b>Keyboard shortcuts</b>\n"
"<tt>"
"Ctrl+N New        Ctrl+F Find         F3     Find Next\n"
"Ctrl+O Open       Ctrl+H Replace      F7     Syntax Check\n"
"Ctrl+S Save       Ctrl+Shift+F Find in Files  F8  Flow Diagram\n"
"Ctrl+W Close      Ctrl+G Go to Line   Shift+F8 Flow (Directory)\n"
"Ctrl+P Print      Ctrl+Shift+I Format  F9     Detailed Flow\n"
"Ctrl+Z Undo       Ctrl+Y Redo         Ctrl+Space Auto-complete\n"
"Ctrl+A Select All Ctrl+Q Exit         Ctrl++/- /0 Zoom\n"
"F1 Help\n"
"</tt>\n\n"

"<b>Author</b>\n"
"Martin Hintermayr &lt;mammutandroid@gmail.com&gt;\n"
"<tt>https://github.com/hintermayr/tuxpad</tt> — MIT License\n";

static void act_help(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    static GtkWidget *win = NULL;
    if (win) { gtk_window_present(GTK_WINDOW(win)); return; }

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "tuxpad — Help");
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(app.window));
    gtk_window_set_default_size(GTK_WINDOW(win), 720, 640);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_widget_destroyed), &win);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(win), sw);

    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), HELP_MARKUP);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_yalign(GTK_LABEL(label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_set_margin_start(label, 16);
    gtk_widget_set_margin_end(label, 16);
    gtk_widget_set_margin_top(label, 12);
    gtk_widget_set_margin_bottom(label, 12);

    gtk_container_add(GTK_CONTAINER(sw), label);
    gtk_widget_show_all(win);
}

static void act_about(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    GtkWidget *a = gtk_about_dialog_new();
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(a), APP_NAME);
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(a), "1.0");
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(a),
        "A Notepad++-style source code editor for Linux.\n"
        "Built with GTK+ 3 and GtkSourceView.");
    const char *authors[] = { "Martin Hintermayr <mammutandroid@gmail.com>", NULL };
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(a), authors);
    gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(a),
        "Copyright © 2026 Martin Hintermayr");
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(a),
        "https://github.com/hintermayr/tuxpad");
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(a), GTK_LICENSE_MIT_X11);
    gtk_window_set_transient_for(GTK_WINDOW(a), GTK_WINDOW(app.window));
    gtk_dialog_run(GTK_DIALOG(a));
    gtk_widget_destroy(a);
}

/* ===================================================================== */
/*  Menu building                                                         */
/* ===================================================================== */

static GtkWidget *add_item(GtkWidget *menu, const char *label,
                           GCallback cb, guint key, GdkModifierType mods)
{
    GtkWidget *mi = gtk_menu_item_new_with_mnemonic(label);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    if (cb) g_signal_connect(mi, "activate", cb, NULL);
    if (key)
        gtk_widget_add_accelerator(mi, "activate", app.accels, key, mods,
                                   GTK_ACCEL_VISIBLE);
    return mi;
}

static void add_sep(GtkWidget *menu)
{ gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new()); }

static GtkWidget *add_submenu(GtkWidget *bar, const char *label)
{
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), item);
    return menu;
}

static GtkWidget *build_menubar(void)
{
    GtkWidget *bar = gtk_menu_bar_new();

    /* File */
    GtkWidget *m = add_submenu(bar, "_File");
    add_item(m, "_New",          G_CALLBACK(act_new),     GDK_KEY_n, GDK_CONTROL_MASK);
    add_item(m, "_Open...",      G_CALLBACK(act_open),    GDK_KEY_o, GDK_CONTROL_MASK);
    GtkWidget *recent_item = gtk_menu_item_new_with_mnemonic("Open _Recent");
    app.recent_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(recent_item), app.recent_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), recent_item);
    rebuild_recent_menu();
    add_sep(m);
    add_item(m, "_Save",         G_CALLBACK(act_save),    GDK_KEY_s, GDK_CONTROL_MASK);
    add_item(m, "Save _As...",   G_CALLBACK(act_save_as), GDK_KEY_s, GDK_CONTROL_MASK|GDK_SHIFT_MASK);
    add_item(m, "Save A_ll",     G_CALLBACK(act_save_all),GDK_KEY_s, GDK_CONTROL_MASK|GDK_MOD1_MASK);
    add_sep(m);
    add_item(m, "_Close",        G_CALLBACK(act_close),   GDK_KEY_w, GDK_CONTROL_MASK);
    add_item(m, "Close All",     G_CALLBACK(act_close_all),0,0);
    add_sep(m);
    add_item(m, "_Print...",     G_CALLBACK(act_print),   GDK_KEY_p, GDK_CONTROL_MASK);
    add_sep(m);
    add_item(m, "E_xit",         G_CALLBACK(act_quit),    GDK_KEY_q, GDK_CONTROL_MASK);

    /* Edit */
    m = add_submenu(bar, "_Edit");
    add_item(m, "_Undo",         G_CALLBACK(act_undo),    GDK_KEY_z, GDK_CONTROL_MASK);
    add_item(m, "_Redo",         G_CALLBACK(act_redo),    GDK_KEY_y, GDK_CONTROL_MASK);
    add_sep(m);
    add_item(m, "Cu_t",          G_CALLBACK(act_cut),     GDK_KEY_x, GDK_CONTROL_MASK);
    add_item(m, "_Copy",         G_CALLBACK(act_copy),    GDK_KEY_c, GDK_CONTROL_MASK);
    add_item(m, "_Paste",        G_CALLBACK(act_paste),   GDK_KEY_v, GDK_CONTROL_MASK);
    add_item(m, "_Delete",       G_CALLBACK(act_delete),  0, 0);
    add_sep(m);
    add_item(m, "Select _All",   G_CALLBACK(act_select_all), GDK_KEY_a, GDK_CONTROL_MASK);

    /* Search */
    m = add_submenu(bar, "_Search");
    add_item(m, "_Find...",      G_CALLBACK(act_find),      GDK_KEY_f, GDK_CONTROL_MASK);
    add_item(m, "Find _Next",    G_CALLBACK(act_find_next), GDK_KEY_F3, 0);
    add_item(m, "Find _Previous",G_CALLBACK(act_find_prev), GDK_KEY_F3, GDK_SHIFT_MASK);
    add_item(m, "_Replace...",   G_CALLBACK(act_replace),   GDK_KEY_h, GDK_CONTROL_MASK);
    add_item(m, "Find in Files...", G_CALLBACK(act_find_in_files),
             GDK_KEY_f, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    add_sep(m);
    add_item(m, "_Go to Line...",G_CALLBACK(act_goto),      GDK_KEY_g, GDK_CONTROL_MASK);

    /* View */
    m = add_submenu(bar, "_View");
    GtkWidget *wrap = gtk_check_menu_item_new_with_mnemonic("_Word Wrap");
    g_signal_connect(wrap, "toggled", G_CALLBACK(act_word_wrap), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), wrap);
    GtkWidget *lnum = gtk_check_menu_item_new_with_mnemonic("Show _Line Numbers");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(lnum), TRUE);
    g_signal_connect(lnum, "toggled", G_CALLBACK(act_line_numbers), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), lnum);
    GtkWidget *ttip = gtk_check_menu_item_new_with_mnemonic(
        "Symbol _Tooltips (mouse-over)");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(ttip), app.hover_tooltips);
    g_signal_connect(ttip, "toggled", G_CALLBACK(act_tooltips), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), ttip);
    GtkWidget *acmp = gtk_check_menu_item_new_with_mnemonic(
        "_Auto-completion (Ctrl+Space)");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(acmp), app.autocomplete);
    g_signal_connect(acmp, "toggled", G_CALLBACK(act_autocomplete), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), acmp);
    add_sep(m);
    add_item(m, "Zoom _In",      G_CALLBACK(act_zoom_in),    GDK_KEY_plus,  GDK_CONTROL_MASK);
    add_item(m, "Zoom _Out",     G_CALLBACK(act_zoom_out),   GDK_KEY_minus, GDK_CONTROL_MASK);
    add_item(m, "_Reset Zoom",   G_CALLBACK(act_zoom_reset), GDK_KEY_0,     GDK_CONTROL_MASK);
    add_sep(m);
    GtkWidget *theme_item = gtk_menu_item_new_with_mnemonic("_Theme");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(theme_item), build_theme_menu());
    gtk_menu_shell_append(GTK_MENU_SHELL(m), theme_item);

    /* Tools */
    m = add_submenu(bar, "_Tools");
    add_item(m, "_Format Document",  G_CALLBACK(act_format),
             GDK_KEY_i, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    add_item(m, "_Syntax Check (C/C++)", G_CALLBACK(act_syntax_check),
             GDK_KEY_F7, 0);
    add_item(m, "Flow _Diagram",     G_CALLBACK(act_flow_diagram),
             GDK_KEY_F8, 0);
    add_item(m, "Flow Diagram (_Directory of current file)",
             G_CALLBACK(act_flow_diagram_dir), GDK_KEY_F8, GDK_SHIFT_MASK);
    add_item(m, "Detailed Flow (current function)",
             G_CALLBACK(act_detailed_flow), GDK_KEY_F9, 0);

    /* Language */
    GtkWidget *lang_item = gtk_menu_item_new_with_mnemonic("_Language");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(lang_item), build_language_menu());
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), lang_item);

    /* Plugins (items are appended by loaded plugins) */
    GtkWidget *plugins_item = gtk_menu_item_new_with_mnemonic("_Plugins");
    app.plugins_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(plugins_item), app.plugins_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), plugins_item);

    /* Help */
    m = add_submenu(bar, "_Help");
    add_item(m, "_Help Contents", G_CALLBACK(act_help), GDK_KEY_F1, 0);
    add_sep(m);
    add_item(m, "_About",        G_CALLBACK(act_about), 0, 0);

    return bar;
}

/* ===================================================================== */
/*  Toolbar                                                               */
/* ===================================================================== */

static GtkToolItem *tbtn(const char *icon, const char *tip, GCallback cb)
{
    GtkToolItem *b = gtk_tool_button_new(NULL, NULL);
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(b), icon);
    gtk_tool_item_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static GtkWidget *build_toolbar(void)
{
    GtkWidget *tb = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(tb), GTK_TOOLBAR_ICONS);
    GtkToolbar *t = GTK_TOOLBAR(tb);
    int i = 0;
    gtk_toolbar_insert(t, tbtn("document-new",   "New (Ctrl+N)",  G_CALLBACK(act_new)), i++);
    gtk_toolbar_insert(t, tbtn("document-open",  "Open (Ctrl+O)", G_CALLBACK(act_open)), i++);
    gtk_toolbar_insert(t, tbtn("document-save",  "Save (Ctrl+S)", G_CALLBACK(act_save)), i++);
    gtk_toolbar_insert(t, tbtn("document-save-as","Save As",      G_CALLBACK(act_save_as)), i++);
    gtk_toolbar_insert(t, gtk_separator_tool_item_new(), i++);
    gtk_toolbar_insert(t, tbtn("edit-undo",      "Undo (Ctrl+Z)", G_CALLBACK(act_undo)), i++);
    gtk_toolbar_insert(t, tbtn("edit-redo",      "Redo (Ctrl+Y)", G_CALLBACK(act_redo)), i++);
    gtk_toolbar_insert(t, gtk_separator_tool_item_new(), i++);
    gtk_toolbar_insert(t, tbtn("edit-cut",       "Cut",           G_CALLBACK(act_cut)), i++);
    gtk_toolbar_insert(t, tbtn("edit-copy",      "Copy",          G_CALLBACK(act_copy)), i++);
    gtk_toolbar_insert(t, tbtn("edit-paste",     "Paste",         G_CALLBACK(act_paste)), i++);
    gtk_toolbar_insert(t, gtk_separator_tool_item_new(), i++);
    gtk_toolbar_insert(t, tbtn("edit-find",      "Find (Ctrl+F)", G_CALLBACK(act_find)), i++);
    gtk_toolbar_insert(t, tbtn("edit-find-replace","Replace (Ctrl+H)", G_CALLBACK(act_replace)), i++);
    gtk_toolbar_insert(t, gtk_separator_tool_item_new(), i++);
    gtk_toolbar_insert(t, tbtn("zoom-in",        "Zoom In",       G_CALLBACK(act_zoom_in)), i++);
    gtk_toolbar_insert(t, tbtn("zoom-out",       "Zoom Out",      G_CALLBACK(act_zoom_out)), i++);
    return tb;
}

/* ===================================================================== */
/*  Status bar                                                            */
/* ===================================================================== */

static GtkWidget *seg(const char *initial, gboolean expand)
{
    GtkWidget *lbl = gtk_label_new(initial);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_widget_set_margin_start(lbl, 8);
    gtk_widget_set_margin_end(lbl, 8);
    (void)expand;
    return lbl;
}

static GtkWidget *build_statusbar(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    app.lbl_len = seg("", TRUE);
    app.lbl_pos = seg("Ln : 1    Col : 1", FALSE);
    app.lbl_sel = seg("Sel : 0", FALSE);
    app.lbl_eol = seg("Unix (LF)", FALSE);
    app.lbl_enc = seg("UTF-8", FALSE);
    app.lbl_lang= seg("Normal text", FALSE);

    gtk_box_pack_start(GTK_BOX(box), app.lbl_len, TRUE, TRUE, 0);
    GtkWidget *fields[] = {app.lbl_pos, app.lbl_sel, app.lbl_lang,
                           app.lbl_eol, app.lbl_enc};
    for (int i = 0; i < 5; i++) {
        gtk_box_pack_start(GTK_BOX(box),
            gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), fields[i], FALSE, FALSE, 0);
    }
    return box;
}

/* ===================================================================== */
/*  Drag & drop                                                           */
/* ===================================================================== */

static void on_drag_received(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                             GtkSelectionData *data, guint info, guint time,
                             gpointer u)
{
    (void)w;(void)x;(void)y;(void)info;(void)u;
    gchar **uris = gtk_selection_data_get_uris(data);
    if (uris) {
        for (int i = 0; uris[i]; i++) {
            char *path = g_filename_from_uri(uris[i], NULL, NULL);
            if (path) { open_path(path); g_free(path); }
        }
        g_strfreev(uris);
    }
    gtk_drag_finish(ctx, TRUE, FALSE, time);
}

/* ===================================================================== */
/*  Window close                                                          */
/* ===================================================================== */

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w;(void)e;(void)d;
    if (!confirm_quit()) return TRUE;   /* cancelled: stop the destroy */
    save_session();
    return FALSE;
}

/* ===================================================================== */
/*  main                                                                  */
/* ===================================================================== */

/*
 * Work around a GTK3 / GNOME-schema incompatibility.
 *
 * GTK 3.24 reads the font keys (antialiasing, hinting, rgba-order) from the
 * "org.gnome.settings-daemon.plugins.xsettings" GSettings schema and aborts
 * if they are missing. GNOME >= 47 moved those keys into a ".deprecated"
 * schema, so on such systems every GTK3 program crashes at startup unless a
 * running settings daemon feeds the values over X/D-Bus.
 *
 * We ship a corrected copy of that schema in a "schemas" directory next to the
 * executable (and under <prefix>/share/tuxpad/schemas when installed). If the
 * key is absent, point GSETTINGS_SCHEMA_DIR at our copy and re-exec once.
 */
static void ensure_font_schema(char **argv)
{
    if (g_getenv("TUXPAD_SCHEMA_FIXED"))
        return;                       /* already re-exec'd; avoid a loop */

    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *sc = src
        ? g_settings_schema_source_lookup(
              src, "org.gnome.settings-daemon.plugins.xsettings", TRUE)
        : NULL;
    gboolean ok = sc && g_settings_schema_has_key(sc, "antialiasing");
    if (sc) g_settings_schema_unref(sc);
    if (ok) return;                   /* system schema is fine */

    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (!exe) return;
    char *dir = g_path_get_dirname(exe);

    char *cands[] = {
        g_build_filename(dir, "schemas", NULL),                   /* build dir */
        g_build_filename(dir, "..", "share", "tuxpad", "schemas", NULL),
        NULL
    };
    char *found = NULL;
    for (int i = 0; cands[i]; i++) {
        char *compiled = g_build_filename(cands[i], "gschemas.compiled", NULL);
        if (!found && g_file_test(compiled, G_FILE_TEST_EXISTS))
            found = g_strdup(cands[i]);
        g_free(compiled);
    }

    if (found) {
        const char *old = g_getenv("GSETTINGS_SCHEMA_DIR");
        char *val = old && *old
            ? g_strdup_printf("%s:%s", found, old)
            : g_strdup(found);
        g_setenv("GSETTINGS_SCHEMA_DIR", val, TRUE);
        g_setenv("TUXPAD_SCHEMA_FIXED", "1", TRUE);
        g_free(val);
        execv(exe, argv);             /* restart with the fixed schema path */
        /* if execv fails we fall through and try to run anyway */
    } else {
        g_warning("tuxpad: font schema key missing and no bundled schema found; "
                  "the GTK toolkit may abort. See README.");
    }

    g_free(found);
    for (int i = 0; cands[i]; i++) g_free(cands[i]);
    g_free(dir);
    g_free(exe);
}

/* Headless export: tuxpad --flow-png <source> <out.png>
   Renders the flow diagram of a C/C++ file to a PNG without opening a window
   (no display required). Returns 0 on success. */
static int flow_png_export(const char *src, const char *out)
{
    char *content = NULL;
    if (!g_file_get_contents(src, &content, NULL, NULL)) {
        g_printerr("tuxpad: cannot read %s\n", src);
        return 1;
    }
    FlowGraph *g = g_new0(FlowGraph, 1);
    build_call_graph2(src, content, g);
    g_free(content);
    if (g->n == 0) {
        g_printerr("tuxpad: no functions found in %s\n", src);
        on_flow_destroy(NULL, g);
        return 2;
    }
    cairo_surface_t *m = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *mcr = cairo_create(m);
    cairo_select_font_face(mcr, "monospace",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    flow_layout(g, mcr);
    cairo_destroy(mcr);
    cairo_surface_destroy(m);

    double W, H; flow_bounds(g, &W, &H);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                    (int)W, (int)H);
    cairo_t *cr = cairo_create(s);
    on_flow_draw(NULL, cr, g);
    cairo_status_t st = cairo_surface_write_to_png(s, out);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    int rc = (st == CAIRO_STATUS_SUCCESS) ? 0 : 3;
    int nn = g->n, ne = g->ne;
    on_flow_destroy(NULL, g);
    if (rc == 0) g_print("Wrote %s (%dx%d, %d functions, %d calls)\n",
                         out, (int)W, (int)H, nn, ne);
    return rc;
}

/* Headless export of the detailed flowchart of the first function in a file. */
static int stepflow_png_export(const char *src, const char *out)
{
    char *text = NULL;
    if (!g_file_get_contents(src, &text, NULL, NULL)) {
        g_printerr("tuxpad: cannot read %s\n", src); return 1;
    }
    gssize len = strlen(text);
    GRegex *re = g_regex_new("([A-Za-z_]\\w*)\\s*\\(([^()]*)\\)\\s*\\{",
                             G_REGEX_MULTILINE, 0, NULL);
    GMatchInfo *mi = NULL; g_regex_match(re, text, 0, &mi);
    gssize brace=-1, bend=-1; char *fname=NULL;
    while (g_match_info_matches(mi) && brace<0) {
        char *nm=g_match_info_fetch(mi,1); int st,en;
        g_match_info_fetch_pos(mi,0,&st,&en);
        if (!is_c_keyword(nm)) {
            gssize ls=st; while(ls>0&&text[ls-1]!='\n')ls--;
            gboolean okrt=TRUE;
            for(gssize p=ls;p<st&&okrt;p++){char c=text[p];
                if(c==')'||c=='('||c==','||c==';'||c=='=')okrt=FALSE;}
            gssize br=en-1, be=match_brace(text,len,br);
            if(okrt&&be>br){ brace=br; bend=be; fname=g_strdup(nm); }
        }
        g_free(nm); g_match_info_next(mi,NULL);
    }
    if(mi)g_match_info_free(mi); g_regex_unref(re);
    if (brace<0){ g_printerr("tuxpad: no function found\n"); g_free(text); return 2; }

    int base_line=1; for(gssize p=0;p<brace;p++) if(text[p]=='\n')base_line++;
    char *body=g_strndup(text+brace+1,bend-(brace+1));
    StepFlow *sf=g_new0(StepFlow,1); sf->zoom=1.0;
    cairo_surface_t *m=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1,1);
    cairo_t *mc=cairo_create(m);
    cairo_select_font_face(mc,"monospace",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
    build_detailed(sf, body, base_line, mc);
    cairo_destroy(mc); cairo_surface_destroy(m);
    g_free(body); g_free(text);
    cairo_surface_t *s=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,(int)sf->width,(int)sf->height);
    cairo_t *cr=cairo_create(s); sf_draw(NULL,cr,sf);
    cairo_surface_write_to_png(s,out); cairo_destroy(cr); cairo_surface_destroy(s);
    g_print("Wrote %s (%dx%d, %d steps, function %s)\n",
            out,(int)sf->width,(int)sf->height,sf->n,fname);
    g_free(fname); sf_destroy(NULL,sf);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "--flow-png"))
        return flow_png_export(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "--stepflow-png"))
        return stepflow_png_export(argv[2], argv[3]);

    ensure_font_schema(argv);
    gtk_init(&argc, &argv);

    app.lm  = gtk_source_language_manager_get_default();
    app.ssm = gtk_source_style_scheme_manager_get_default();
    app.search_settings = gtk_source_search_settings_new();
    app.font_size = 11;
    app.word_wrap = FALSE;
    app.show_line_numbers = TRUE;
    app.hover_tooltips = TRUE;
    app.autocomplete = TRUE;
    load_recent();

    /* Choose a pleasant default scheme if available */
    const char *prefer[] = {"classic", "tango", "kate", NULL};
    for (int i = 0; prefer[i]; i++)
        if (gtk_source_style_scheme_manager_get_scheme(app.ssm, prefer[i])) {
            app.scheme_id = g_strdup(prefer[i]); break;
        }
    if (!app.scheme_id) {
        const gchar * const *ids =
            gtk_source_style_scheme_manager_get_scheme_ids(app.ssm);
        if (ids && ids[0]) app.scheme_id = g_strdup(ids[0]);
    }

    app.font_provider = gtk_css_provider_new();
    apply_font_css();

    g_set_application_name(APP_NAME);
    gtk_window_set_default_icon_name(APP_NAME);

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1000, 700);
    gtk_window_set_title(GTK_WINDOW(app.window), APP_NAME);
    gtk_window_set_icon_name(GTK_WINDOW(app.window), APP_NAME);
    g_signal_connect(app.window, "delete-event", G_CALLBACK(on_delete), NULL);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    app.accels = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(app.window), app.accels);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app.window), vbox);

    /* Create the notebook first: menu construction toggles the line-number
       check item, whose handler reaches into app.notebook. */
    app.notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(app.notebook), TRUE);
    g_signal_connect(app.notebook, "switch-page",
                     G_CALLBACK(on_page_switched), NULL);

    gtk_box_pack_start(GTK_BOX(vbox), build_menubar(),  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_toolbar(),  FALSE, FALSE, 0);

    /* Editor on top, Find-in-Files results in a resizable pane below. */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(paned), app.notebook, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), build_results_panel(), FALSE, TRUE);
    gtk_paned_set_position(GTK_PANED(paned), 480);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_statusbar(), FALSE, FALSE, 2);

    /* Drag & drop target */
    gtk_drag_dest_set(app.window, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY);
    gtk_drag_dest_add_uri_targets(app.window);
    g_signal_connect(app.window, "drag-data-received",
                     G_CALLBACK(on_drag_received), NULL);

    gtk_widget_show_all(app.window);
    gtk_widget_hide(app.results_panel);   /* shown on first Find in Files */

    load_all_plugins();

    /* Open files given on the command line, else start with a blank tab */
    gboolean opened = FALSE;
    for (int i = 1; i < argc; i++) {
        if (g_file_test(argv[i], G_FILE_TEST_EXISTS)) {
            open_path(argv[i]); opened = TRUE;
        } else {
            /* allow creating a new named file */
            Document *doc = document_new(NULL);
            g_free(doc->path);
            doc->path = g_strdup(argv[i]);
            doc->untitled = FALSE;
            set_tab_title(doc);
            opened = TRUE;
        }
    }
    /* No files on the command line: restore the previous session. */
    if (!opened && restore_session() > 0) opened = TRUE;
    if (!opened) document_new(NULL);

    update_status_bar();
    update_title();

    gtk_main();
    return 0;
}
