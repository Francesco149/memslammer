/*
 * memslammer lite for Linux: a small GTK 3 window around the core.
 * SPDX-License-Identifier: MIT
 */
#include "lite.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

static struct {
    GtkApplication *app;
    GtkWidget *win, *mb, *threads, *duration, *stop_err, *options;
    GtkWidget *start, *copy, *headline, *stat[5], *progress, *log;
    GtkTextBuffer *buf;
    lite_run run;
    int running;
    guint timer, inhibit;
} ui;

static const char *stat_names[5] = { "Elapsed", "Coverage", "Throughput", "Errors", "Tested" };

static void log_line(void *ud, const char *line)
{
    (void)ud;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(ui.buf, &end);
    gtk_text_buffer_insert(ui.buf, &end, line, -1);
    gtk_text_buffer_insert(ui.buf, &end, "\n", 1);
    GtkTextMark *m = gtk_text_buffer_get_mark(ui.buf, "end");
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(ui.log), m);
}

static void set_headline(const char *text, int tone)
{
    const char *color = tone == LITE_TONE_GOOD ? "#2e9d4a" : tone == LITE_TONE_BAD ? "#d03030" : NULL;
    char *m = color ? g_markup_printf_escaped("<span size='x-large' weight='bold' foreground='%s'>%s</span>", color, text)
                    : g_markup_printf_escaped("<span size='x-large' weight='bold'>%s</span>", text);
    gtk_label_set_markup(GTK_LABEL(ui.headline), m);
    g_free(m);
}

static void set_form_sensitive(gboolean on)
{
    GtkWidget *w[] = { ui.mb, ui.threads, ui.duration, ui.stop_err, ui.options };
    for (unsigned i = 0; i < G_N_ELEMENTS(w); i++) gtk_widget_set_sensitive(w[i], on);
}

static void set_button(int running)
{
    GtkStyleContext *sc = gtk_widget_get_style_context(ui.start);
    gtk_button_set_label(GTK_BUTTON(ui.start), running ? "Stop" : "Start");
    gtk_style_context_remove_class(sc, running ? "suggested-action" : "destructive-action");
    gtk_style_context_add_class(sc, running ? "destructive-action" : "suggested-action");
}

static void finish(void)
{
    if (ui.timer) { g_source_remove(ui.timer); ui.timer = 0; }
    if (ui.inhibit) { gtk_application_uninhibit(ui.app, ui.inhibit); ui.inhibit = 0; }
    lite_free(&ui.run);
    ui.running = 0;
    set_form_sensitive(TRUE);
    set_button(0);
    gtk_widget_set_sensitive(ui.start, TRUE);
}

static gboolean tick(gpointer data)
{
    (void)data;
    ms_status s;
    char head[96], stats[5][64];
    lite_poll(&ui.run, &s, log_line, NULL);
    int tone = lite_headline(&ui.run, &s, head, sizeof head);
    set_headline(head, tone);
    lite_stats(&ui.run, &s, stats);
    for (int i = 0; i < 5; i++) gtk_label_set_text(GTK_LABEL(ui.stat[i]), stats[i]);
    if (ui.run.cfg.seconds > 0) {
        double f = s.elapsed / ui.run.cfg.seconds;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress), f > 1 ? 1 : f);
    } else if (s.state == MS_STATE_RUNNING) {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(ui.progress));
    }
    if (s.state == MS_STATE_DONE || s.state == MS_STATE_FAILED) {
        if (s.state == MS_STATE_DONE && ui.run.cfg.seconds <= 0)
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress), 0);
        if (s.errors) gtk_window_set_urgency_hint(GTK_WINDOW(ui.win), TRUE);
        ui.timer = 0;
        finish();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void on_start(GtkButton *b, gpointer data)
{
    (void)b; (void)data;
    if (ui.running) {
        lite_stop(&ui.run);
        gtk_widget_set_sensitive(ui.start, FALSE);
        return;
    }
    char mb[32], th[32], err[1500];
    snprintf(mb, sizeof mb, "%.0f", gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui.mb)));
    snprintf(th, sizeof th, "%.0f", gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui.threads)));
    int d = gtk_combo_box_get_active(GTK_COMBO_BOX(ui.duration));
    ms_config c;
    if (lite_make_config(&c, mb, th, lite_durations[d < 0 ? 0 : d],
                         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.stop_err)),
                         gtk_entry_get_text(GTK_ENTRY(ui.options)), err, sizeof err) ||
        lite_start(&ui.run, &c, log_line, NULL, err, sizeof err)) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(ui.win), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                                GTK_BUTTONS_CLOSE, "%s", err);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        return;
    }
    ui.running = 1;
    gtk_window_set_urgency_hint(GTK_WINDOW(ui.win), FALSE);
    set_form_sensitive(FALSE);
    set_button(1);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress), 0);
    ui.inhibit = gtk_application_inhibit(ui.app, GTK_WINDOW(ui.win),
                                         GTK_APPLICATION_INHIBIT_SUSPEND | GTK_APPLICATION_INHIBIT_IDLE,
                                         "Memory test running");
    ui.timer = g_timeout_add(200, tick, NULL);
    tick(NULL);
}

static void on_copy(GtkButton *b, gpointer data)
{
    (void)b; (void)data;
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(ui.buf, &s, &e);
    char *text = gtk_text_buffer_get_text(ui.buf, &s, &e, FALSE);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
    g_free(text);
}

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer data)
{
    (void)w; (void)e; (void)data;
    if (ui.running) finish();  /* stops and waits for the workers */
    return FALSE;
}

static GtkWidget *label(const char *text, float xalign)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    return l;
}

static void activate(GtkApplication *app, gpointer data)
{
    (void)data;
    ms_sysinfo si;
    ms_get_sysinfo(&si);
    char title[64], buf[160];
    snprintf(title, sizeof title, "memslammer lite %s", ms_version());

    ui.win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ui.win), title);
    gtk_window_set_default_size(GTK_WINDOW(ui.win), 560, 620);
    g_signal_connect(ui.win, "delete-event", G_CALLBACK(on_delete), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    gtk_container_add(GTK_CONTAINER(ui.win), box);

    snprintf(buf, sizeof buf, "%s, %d threads, %llu MB RAM (%llu MB free)%s", si.cpu, si.logical_cpus,
             (unsigned long long)si.total_mb, (unsigned long long)si.avail_mb, si.avx2 ? "" : ", no AVX2!");
    GtkWidget *sys = label(buf, 0);
    gtk_label_set_ellipsize(GTK_LABEL(sys), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(sys), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), sys, FALSE, FALSE, 0);

    /* form */
    GtkWidget *form = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(form), 6);
    gtk_grid_set_column_spacing(GTK_GRID(form), 10);
    gtk_box_pack_start(GTK_BOX(box), form, FALSE, FALSE, 0);

    ui.mb = gtk_spin_button_new_with_range(64, (double)(si.total_mb ? si.total_mb : 1 << 20), 256);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.mb), (double)(si.avail_mb * 9 / 10));
    gtk_widget_set_hexpand(ui.mb, TRUE);
    gtk_widget_set_tooltip_text(ui.mb, "Default: 90% of free RAM");
    gtk_grid_attach(GTK_GRID(form), label("Memory (MB)", 0), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(form), ui.mb, 1, 0, 1, 1);

    ui.threads = gtk_spin_button_new_with_range(1, 4096, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.threads), si.logical_cpus);
    gtk_grid_attach(GTK_GRID(form), label("Threads", 0), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(form), ui.threads, 1, 1, 1, 1);

    ui.duration = gtk_combo_box_text_new();
    for (int i = 0; i < lite_n_durations; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.duration), lite_duration_names[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.duration), 0);
    gtk_grid_attach(GTK_GRID(form), label("Duration", 0), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(form), ui.duration, 1, 2, 1, 1);

    ui.options = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.options), "extra CLI flags, e.g. -C flush or -M chain (-h lists them)");
    gtk_grid_attach(GTK_GRID(form), label("Options", 0), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(form), ui.options, 1, 3, 1, 1);

    ui.stop_err = gtk_check_button_new_with_label("Stop on first error");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.stop_err), TRUE);
    gtk_grid_attach(GTK_GRID(form), ui.stop_err, 1, 4, 1, 1);

    /* buttons + headline */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui.headline = label("", 0);
    set_headline("Ready", LITE_TONE_NORMAL);
    gtk_box_pack_start(GTK_BOX(row), ui.headline, TRUE, TRUE, 0);
    ui.copy = gtk_button_new_with_label("Copy log");
    g_signal_connect(ui.copy, "clicked", G_CALLBACK(on_copy), NULL);
    gtk_box_pack_end(GTK_BOX(row), ui.copy, FALSE, FALSE, 0);
    ui.start = gtk_button_new_with_label("Start");
    gtk_widget_set_size_request(ui.start, 96, -1);
    g_signal_connect(ui.start, "clicked", G_CALLBACK(on_start), NULL);
    gtk_box_pack_end(GTK_BOX(row), ui.start, FALSE, FALSE, 0);
    set_button(0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 4);

    /* stats */
    GtkWidget *stats = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(stats), 2);
    gtk_grid_set_column_spacing(GTK_GRID(stats), 16);
    for (int i = 0; i < 5; i++) {
        GtkWidget *n = label(stat_names[i], 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(n), "dim-label");
        ui.stat[i] = label("-", 0);
        gtk_label_set_selectable(GTK_LABEL(ui.stat[i]), TRUE);
        gtk_grid_attach(GTK_GRID(stats), n, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(stats), ui.stat[i], 1, i, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(box), stats, FALSE, FALSE, 0);

    ui.progress = gtk_progress_bar_new();
    gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(ui.progress), 0.04);
    gtk_box_pack_start(GTK_BOX(box), ui.progress, FALSE, FALSE, 0);

    /* log */
    ui.log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(ui.log), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(ui.log), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ui.log), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(ui.log), 6);
    ui.buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui.log));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(ui.buf, &end);
    gtk_text_buffer_create_mark(ui.buf, "end", &end, FALSE);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), ui.log);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);

    if (!si.avx2) {
        set_headline("This CPU has no AVX2", LITE_TONE_BAD);
        gtk_widget_set_sensitive(ui.start, FALSE);
    }
    gtk_widget_show_all(ui.win);
    gtk_widget_grab_focus(ui.start);
}

int main(int argc, char **argv)
{
    ui.app = gtk_application_new(NULL, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(ui.app, "activate", G_CALLBACK(activate), NULL);
    int r = g_application_run(G_APPLICATION(ui.app), argc, argv);
    if (ui.running) finish();
    g_object_unref(ui.app);
    return r;
}
