#pragma once

#include <gtk/gtk.h>

#include <cstddef>

namespace pcmtp {

class GtkPlayerWindow;

namespace patches {

void update_current_track_from_playlist_ui(GtkPlayerWindow& window, int index_column);
gboolean on_playlist_focus_in(GtkWidget* widget, GdkEventFocus* event, gpointer user_data);
gboolean on_playlist_view_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data);
bool find_playlist_view_path_for_index(GtkTreeView* playlist_view,
                                       std::size_t index,
                                       int index_column,
                                       GtkTreePath** out_path);
bool playlist_index_from_view_path(GtkTreeView* playlist_view,
                                   GtkTreePath* path,
                                   int index_column,
                                   std::size_t* out_index);

} // namespace patches
} // namespace pcmtp
