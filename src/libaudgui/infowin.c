/*
 * libaudgui/infowin.c
 * Copyright 2006-2012 William Pitcock, Tomasz Moń, Eugene Zagidullin,
 *                     John Lindgren, and Thomas Lange
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include <gtk/gtk.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <audacious/i18n.h>
#include <audacious/misc.h>
#include <audacious/playlist.h>
#include <libaudcore/audstrings.h>
#include <libaudcore/hook.h>

#include "libaudgui.h"
#include "libaudgui-gtk.h"

#define AUDGUI_STATUS_TIMEOUT 3000
#define IMAGE_SIZE 150

enum {
    CODEC_FORMAT,
    CODEC_QUALITY,
    CODEC_BITRATE,
    CODEC_ITEMS
};

static const char * codec_labels[CODEC_ITEMS] = {
    N_("Format:"),
    N_("Quality:"),
    N_("Bitrate:")
};

static GtkWidget * infowin = NULL;

static struct {
    GtkWidget * location;
    GtkWidget * title;
    GtkWidget * artist;
    GtkWidget * album;
    GtkWidget * comment;
    GtkWidget * year;
    GtkWidget * track;
    GtkWidget * genre;
    GtkWidget * image;
    GtkWidget * codec[3];
    GtkWidget * apply;
    GtkWidget * ministatus;
} widgets;

static char * current_file = NULL;
static PluginHandle * current_decoder = NULL;
static bool_t can_write = FALSE;

/* This is by no means intended to be a complete list.  If it is not short, it
 * is useless: scrolling through ten pages of dropdown list is more work than
 * typing out the genre. */

static const char * genre_table[] = {
 N_("Acid Jazz"),
 N_("Acid Rock"),
 N_("Ambient"),
 N_("Bebop"),
 N_("Bluegrass"),
 N_("Blues"),
 N_("Chamber Music"),
 N_("Classical"),
 N_("Country"),
 N_("Death Metal"),
 N_("Disco"),
 N_("Easy Listening"),
 N_("Folk"),
 N_("Funk"),
 N_("Gangsta Rap"),
 N_("Gospel"),
 N_("Grunge"),
 N_("Hard Rock"),
 N_("Heavy Metal"),
 N_("Hip-hop"),
 N_("House"),
 N_("Jazz"),
 N_("Jungle"),
 N_("Metal"),
 N_("New Age"),
 N_("New Wave"),
 N_("Noise"),
 N_("Pop"),
 N_("Punk Rock"),
 N_("Rap"),
 N_("Reggae"),
 N_("Rock"),
 N_("Rock and Roll"),
 N_("Rhythm and Blues"),
 N_("Ska"),
 N_("Soul"),
 N_("Swing"),
 N_("Techno"),
 N_("Trip-hop")};

static GtkWidget * small_label_new (const char * text)
{
    static PangoAttrList * attrs = NULL;

    if (! attrs)
    {
        attrs = pango_attr_list_new ();
        pango_attr_list_insert (attrs, pango_attr_scale_new (PANGO_SCALE_SMALL));
    }

    GtkWidget * label = gtk_label_new (text);
    gtk_label_set_attributes ((GtkLabel *) label, attrs);
    gtk_misc_set_alignment ((GtkMisc *) label, 0, 0.5);

    return label;
}

static void set_entry_str_from_field (GtkWidget * widget, const Tuple * tuple,
 int fieldn, bool_t editable)
{
    char * text = tuple_get_str (tuple, fieldn, NULL);

    gtk_entry_set_text ((GtkEntry *) widget, text != NULL ? text : "");
    gtk_editable_set_editable ((GtkEditable *) widget, editable);

    str_unref (text);
}

static void set_entry_int_from_field (GtkWidget * widget, const Tuple * tuple,
 int fieldn, bool_t editable)
{
    char scratch[32];

    if (tuple_get_value_type (tuple, fieldn, NULL) == TUPLE_INT)
        snprintf (scratch, sizeof scratch, "%d", tuple_get_int (tuple, fieldn,
         NULL));
    else
        scratch[0] = 0;

    gtk_entry_set_text ((GtkEntry *) widget, scratch);
    gtk_editable_set_editable ((GtkEditable *) widget, editable);
}

static void set_field_str_from_entry (Tuple * tuple, int fieldn, GtkWidget *
 widget)
{
    const char * text = gtk_entry_get_text ((GtkEntry *) widget);

    if (text[0])
        tuple_set_str (tuple, fieldn, NULL, text);
    else
        tuple_unset (tuple, fieldn, NULL);
}

static void set_field_int_from_entry (Tuple * tuple, int fieldn, GtkWidget *
 widget)
{
    const char * text = gtk_entry_get_text ((GtkEntry *) widget);

    if (text[0])
        tuple_set_int (tuple, fieldn, NULL, atoi (text));
    else
        tuple_unset (tuple, fieldn, NULL);
}

static void entry_changed (GtkEditable * editable, void * unused)
{
    if (can_write)
        gtk_widget_set_sensitive (widgets.apply, TRUE);
}

static bool_t ministatus_timeout_proc (void)
{
    gtk_label_set_text ((GtkLabel *) widgets.ministatus, NULL);
    return FALSE;
}

static void ministatus_display_message (const char * text)
{
    gtk_label_set_text ((GtkLabel *) widgets.ministatus, text);
    g_timeout_add (AUDGUI_STATUS_TIMEOUT, (GSourceFunc) ministatus_timeout_proc, NULL);
}

static void infowin_update_tuple (void * unused)
{
    Tuple * tuple = tuple_new_from_filename (current_file);

    set_field_str_from_entry (tuple, FIELD_TITLE, widgets.title);
    set_field_str_from_entry (tuple, FIELD_ARTIST, widgets.artist);
    set_field_str_from_entry (tuple, FIELD_ALBUM, widgets.album);
    set_field_str_from_entry (tuple, FIELD_COMMENT, widgets.comment);
    set_field_str_from_entry (tuple, FIELD_GENRE, gtk_bin_get_child ((GtkBin *)
     widgets.genre));
    set_field_int_from_entry (tuple, FIELD_YEAR, widgets.year);
    set_field_int_from_entry (tuple, FIELD_TRACK_NUMBER, widgets.track);

    if (aud_file_write_tuple (current_file, current_decoder, tuple))
    {
        ministatus_display_message (_("Save successful"));
        gtk_widget_set_sensitive (widgets.apply, FALSE);
    }
    else
        ministatus_display_message (_("Save error"));

    tuple_unref (tuple);
}

static bool_t genre_fill (GtkWidget * combo)
{
    GList * list = NULL;
    GList * node;
    int i;

    for (i = 0; i < G_N_ELEMENTS (genre_table); i ++)
        list = g_list_prepend (list, _(genre_table[i]));

    list = g_list_sort (list, (GCompareFunc) strcmp);

    for (node = list; node != NULL; node = node->next)
        gtk_combo_box_text_append_text ((GtkComboBoxText *) combo, node->data);

    g_list_free (list);
    return FALSE;
}

static void infowin_display_image (const char * filename)
{
    if (! current_file || strcmp (filename, current_file))
        return;

    GdkPixbuf * pb = audgui_pixbuf_request (filename);
    if (! pb)
        pb = audgui_pixbuf_fallback ();

    audgui_pixbuf_scale_within (& pb, IMAGE_SIZE);
    gtk_image_set_from_pixbuf ((GtkImage *) widgets.image, pb);
    g_object_unref (pb);
}

static void infowin_destroyed (void)
{
    hook_dissociate ("art ready", (HookFunction) infowin_display_image);

    infowin = NULL;
    memset (& widgets, 0, sizeof widgets);

    str_unref (current_file);
    current_file = NULL;
    current_decoder = NULL;
    can_write = FALSE;
}

static void create_infowin (void)
{
    GtkWidget * hbox;
    GtkWidget * hbox_status_and_bbox;
    GtkWidget * vbox0;
    GtkWidget * vbox2;
    GtkWidget * codec_hbox;
    GtkWidget * grid1;
    GtkWidget * bbox_close;
    GtkWidget * btn_close;
    GtkWidget * alignment;

    infowin = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_container_set_border_width ((GtkContainer *) infowin, 6);
    gtk_window_set_title ((GtkWindow *) infowin, _("Track Information"));
    gtk_window_set_type_hint ((GtkWindow *) infowin,
     GDK_WINDOW_TYPE_HINT_DIALOG);

    vbox0 = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add ((GtkContainer *) infowin, vbox0);

    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,  6);
    gtk_box_pack_start ((GtkBox *) vbox0, hbox, TRUE, TRUE, 0);

    vbox2 = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start ((GtkBox *) hbox, vbox2, TRUE, TRUE, 0);

    GtkWidget * vbox3 = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start ((GtkBox *) vbox2, vbox3, TRUE, FALSE, 0);

    widgets.image = gtk_image_new ();
    gtk_widget_set_size_request (widgets.image, IMAGE_SIZE, IMAGE_SIZE);
    gtk_box_pack_start ((GtkBox *) vbox3, widgets.image, FALSE, FALSE, 0);

    widgets.location = gtk_label_new ("");
    gtk_widget_set_size_request (widgets.location, 200, -1);
    gtk_label_set_line_wrap ((GtkLabel *) widgets.location, TRUE);
    gtk_label_set_line_wrap_mode ((GtkLabel *) widgets.location, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable ((GtkLabel *) widgets.location, TRUE);
    gtk_box_pack_start ((GtkBox *) vbox3, widgets.location, FALSE, FALSE, 0);

    codec_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,  6);
    gtk_box_pack_start ((GtkBox *) vbox2, codec_hbox, FALSE, FALSE, 0);

    GtkWidget * codec_grid = gtk_grid_new ();
    gtk_grid_set_row_spacing ((GtkGrid *) codec_grid, 3);
    gtk_grid_set_column_spacing ((GtkGrid *) codec_grid, 12);
    gtk_box_pack_start ((GtkBox *) codec_hbox, codec_grid, FALSE, FALSE, 0);

    for (int row = 0; row < CODEC_ITEMS; row ++)
    {
        GtkWidget * label = small_label_new (_(codec_labels[row]));
        gtk_grid_attach ((GtkGrid *) codec_grid, label, 0, row, 1, 1);

        widgets.codec[row] = small_label_new (NULL);
        gtk_grid_attach ((GtkGrid *) codec_grid, widgets.codec[row], 1, row, 1, 1);
    }

    vbox2 = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start ((GtkBox *) hbox, vbox2, TRUE, TRUE, 0);

    gtk_box_pack_start ((GtkBox *) vbox2, small_label_new (_("Title")), FALSE, FALSE, 0);

    alignment = gtk_alignment_new (0.5, 0.5, 1, 1);
    gtk_box_pack_start ((GtkBox *) vbox2, alignment, FALSE, FALSE, 0);
    gtk_alignment_set_padding ((GtkAlignment *) alignment, 0, 6, 0, 0);
    widgets.title = gtk_entry_new ();
    gtk_container_add ((GtkContainer *) alignment, widgets.title);
    g_signal_connect (widgets.title, "changed", (GCallback) entry_changed, NULL);

    gtk_box_pack_start ((GtkBox *) vbox2, small_label_new (_("Artist")), FALSE, FALSE, 0);

    alignment = gtk_alignment_new (0.5, 0.5, 1, 1);
    gtk_box_pack_start ((GtkBox *) vbox2, alignment, FALSE, FALSE, 0);
    gtk_alignment_set_padding ((GtkAlignment *) alignment, 0, 6, 0, 0);
    widgets.artist = gtk_entry_new ();
    gtk_container_add ((GtkContainer *) alignment, widgets.artist);
    g_signal_connect (widgets.artist, "changed", (GCallback) entry_changed, NULL);

    gtk_box_pack_start ((GtkBox *) vbox2, small_label_new (_("Album")), FALSE, FALSE, 0);

    alignment = gtk_alignment_new (0.5, 0.5, 1, 1);
    gtk_box_pack_start ((GtkBox *) vbox2, alignment, FALSE, FALSE, 0);
    gtk_alignment_set_padding ((GtkAlignment *) alignment, 0, 6, 0, 0);
    widgets.album = gtk_entry_new ();
    gtk_container_add ((GtkContainer *) alignment, widgets.album);
    g_signal_connect (widgets.album, "changed", (GCallback) entry_changed, NULL);

    gtk_box_pack_start ((GtkBox *) vbox2, small_label_new (_("Comment")), FALSE, FALSE, 0);

    alignment = gtk_alignment_new (0.5, 0.5, 1, 1);
    gtk_box_pack_start ((GtkBox *) vbox2, alignment, FALSE, FALSE, 0);
    gtk_alignment_set_padding ((GtkAlignment *) alignment, 0, 6, 0, 0);
    widgets.comment = gtk_entry_new ();
    gtk_container_add ((GtkContainer *) alignment, widgets.comment);
    g_signal_connect (widgets.comment, "changed", (GCallback) entry_changed, NULL);

    gtk_box_pack_start ((GtkBox *) vbox2, small_label_new (_("Genre")), FALSE, FALSE, 0);

    alignment = gtk_alignment_new (0.5, 0.5, 1, 1);
    gtk_box_pack_start ((GtkBox *) vbox2, alignment, FALSE, FALSE, 0);
    gtk_alignment_set_padding ((GtkAlignment *) alignment, 0, 6, 0, 0);

    widgets.genre = gtk_combo_box_text_new_with_entry ();
    gtk_container_add ((GtkContainer *) alignment, widgets.genre);
    g_signal_connect (widgets.genre, "changed", (GCallback) entry_changed, NULL);
    g_idle_add ((GSourceFunc) genre_fill, widgets.genre);

    alignment = gtk_alignment_new (0.5, 0.5, 1, 1);
    gtk_box_pack_start ((GtkBox *) vbox2, alignment, FALSE, FALSE, 0);
    gtk_alignment_set_padding ((GtkAlignment *) alignment, 0, 6, 0, 0);
    grid1 = gtk_grid_new ();
    gtk_grid_set_column_homogeneous ((GtkGrid *) grid1, TRUE);
    gtk_container_add ((GtkContainer *) alignment, grid1);
    gtk_grid_set_column_spacing ((GtkGrid *) grid1, 6);

    gtk_grid_attach ((GtkGrid *) grid1, small_label_new (_("Year")), 0, 0, 1, 1);

    widgets.year = gtk_entry_new ();
    gtk_grid_attach ((GtkGrid *) grid1, widgets.year, 0, 1, 1, 1);
    g_signal_connect (widgets.year, "changed", (GCallback) entry_changed, NULL);

    gtk_grid_attach ((GtkGrid *) grid1, small_label_new (_("Track Number")), 1, 0, 1, 1);

    widgets.track = gtk_entry_new ();
    gtk_grid_attach ((GtkGrid *) grid1, widgets.track, 1, 1, 1, 1);
    g_signal_connect (widgets.track, "changed", (GCallback) entry_changed, NULL);

    hbox_status_and_bbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,  0);
    gtk_box_pack_start ((GtkBox *) vbox0, hbox_status_and_bbox, FALSE, FALSE, 0);

    widgets.ministatus = small_label_new (NULL);
    gtk_box_pack_start ((GtkBox *) hbox_status_and_bbox, widgets.ministatus, TRUE, TRUE, 0);

    bbox_close = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_box_set_spacing ((GtkBox *) bbox_close, 6);
    gtk_box_pack_start ((GtkBox *) hbox_status_and_bbox, bbox_close, FALSE,
     FALSE, 0);
    gtk_button_box_set_layout ((GtkButtonBox *) bbox_close, GTK_BUTTONBOX_END);

    widgets.apply = gtk_button_new_from_stock (GTK_STOCK_SAVE);
    gtk_container_add ((GtkContainer *) bbox_close, widgets.apply);
    g_signal_connect (widgets.apply, "clicked", (GCallback) infowin_update_tuple, NULL);
    gtk_widget_set_sensitive (widgets.apply, FALSE);

    btn_close = gtk_button_new_from_stock (GTK_STOCK_CLOSE);
    gtk_container_add ((GtkContainer *) bbox_close, btn_close);
    g_signal_connect_swapped (btn_close, "clicked", (GCallback) gtk_widget_hide,
     infowin);

    gtk_widget_show_all (vbox0);
    gtk_widget_grab_focus (widgets.title);

    audgui_destroy_on_escape (infowin);
    g_signal_connect (infowin, "destroy", (GCallback) infowin_destroyed, NULL);

    hook_associate ("art ready", (HookFunction) infowin_display_image, NULL);
}

static void infowin_show (int list, int entry, const char * filename,
 const Tuple * tuple, PluginHandle * decoder, bool_t updating_enabled)
{
    char * tmp;

    if (! infowin)
        create_infowin ();

    str_unref (current_file);
    current_file = str_get (filename);
    current_decoder = decoder;
    can_write = updating_enabled;

    set_entry_str_from_field (widgets.title, tuple, FIELD_TITLE, updating_enabled);
    set_entry_str_from_field (widgets.artist, tuple, FIELD_ARTIST, updating_enabled);
    set_entry_str_from_field (widgets.album, tuple, FIELD_ALBUM, updating_enabled);
    set_entry_str_from_field (widgets.comment, tuple, FIELD_COMMENT, updating_enabled);
    set_entry_str_from_field (gtk_bin_get_child ((GtkBin *) widgets.genre),
     tuple, FIELD_GENRE, updating_enabled);

    tmp = uri_to_display (filename);
    gtk_label_set_text ((GtkLabel *) widgets.location, tmp);
    g_free (tmp);

    set_entry_int_from_field (widgets.year, tuple, FIELD_YEAR, updating_enabled);
    set_entry_int_from_field (widgets.track, tuple, FIELD_TRACK_NUMBER, updating_enabled);

    char * codec_values[CODEC_ITEMS] = {
        [CODEC_FORMAT] = tuple_get_str (tuple, FIELD_CODEC, NULL),
        [CODEC_QUALITY] = tuple_get_str (tuple, FIELD_QUALITY, NULL)
    };

    if (tuple_get_value_type (tuple, FIELD_BITRATE, NULL) == TUPLE_INT)
    {
        int bitrate = tuple_get_int (tuple, FIELD_BITRATE, NULL);
        codec_values[CODEC_BITRATE] = str_printf (_("%d kb/s"), bitrate);
    }

    for (int row = 0; row < CODEC_ITEMS; row ++)
    {
        const char * text = codec_values[row] ? codec_values[row] : _("N/A");
        gtk_label_set_text ((GtkLabel *) widgets.codec[row], text);
        str_unref (codec_values[row]);
    }

    infowin_display_image (filename);

    gtk_window_present ((GtkWindow *) infowin);
}

EXPORT void audgui_infowin_show (int playlist, int entry)
{
    char * filename = aud_playlist_entry_get_filename (playlist, entry);
    g_return_if_fail (filename != NULL);

    PluginHandle * decoder = aud_playlist_entry_get_decoder (playlist, entry,
     FALSE);
    if (decoder == NULL)
        goto FREE;

    if (aud_custom_infowin (filename, decoder))
        goto FREE;

    Tuple * tuple = aud_playlist_entry_get_tuple (playlist, entry, FALSE);

    if (tuple == NULL)
    {
        char * message = g_strdup_printf (_("No info available for %s.\n"),
         filename);
        aud_interface_show_error (message);
        g_free (message);
        goto FREE;
    }

    infowin_show (playlist, entry, filename, tuple, decoder,
     aud_file_can_write_tuple (filename, decoder));
    tuple_unref (tuple);

FREE:
    str_unref (filename);
}

EXPORT void audgui_infowin_show_current (void)
{
    int playlist = aud_playlist_get_playing ();
    int position;

    if (playlist == -1)
        playlist = aud_playlist_get_active ();

    position = aud_playlist_get_position (playlist);

    if (position == -1)
        return;

    audgui_infowin_show (playlist, position);
}

EXPORT void audgui_infowin_hide (void)
{
    if (infowin)
        gtk_widget_destroy (infowin);
}
