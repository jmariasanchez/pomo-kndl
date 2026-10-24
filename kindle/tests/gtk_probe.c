/* Minimal GTK 2 probe for the Kindle: if this shows a window, the toolchain and GTK linkage are good. */
#include <gtk/gtk.h>
#include <stdio.h>
static gboolean quit(gpointer d) { (void)d; gtk_main_quit(); return FALSE; }
int main(int argc, char **argv) {
    fprintf(stderr, "probe: glib %u.%u.%u gtk %u.%u.%u\n", glib_major_version, glib_minor_version,
            glib_micro_version, gtk_major_version, gtk_minor_version, gtk_micro_version);
    gtk_init(&argc, &argv);
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w), "L:A_N:application_PC:T_ID:com.pomokndl.probe");
    gtk_container_add(GTK_CONTAINER(w), gtk_label_new("pomo-kndl GTK probe OK"));
    gtk_widget_show_all(w);
    g_timeout_add_seconds(5, quit, NULL);
    gtk_main();
    fprintf(stderr, "probe: ok\n");
    return 0;
}
