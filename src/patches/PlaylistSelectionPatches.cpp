#include "pcmtp/patches/PlaylistSelectionPatches.hpp"

#include "pcmtp/gui/GtkPlayerWindow.hpp"

namespace pcmtp::patches {

void update_current_track_from_playlist_ui(GtkPlayerWindow& window, int index_column) {
    if (window.playlist_.empty() || window.playlist_view_ == nullptr) {
        return;
    }

    GtkTreeView* view = GTK_TREE_VIEW(window.playlist_view_);
    GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        int row_index = -1;
        gtk_tree_model_get(model, &iter, index_column, &row_index, -1);
        if (row_index >= 0 && static_cast<std::size_t>(row_index) < window.playlist_.size()) {
            window.current_track_index_ = static_cast<std::size_t>(row_index);
            return;
        }
    }

    GtkTreePath* cursor_path = nullptr;
    GtkTreeViewColumn* cursor_column = nullptr;
    gtk_tree_view_get_cursor(view, &cursor_path, &cursor_column);
    if (cursor_path != nullptr) {
        GtkTreeModel* cursor_model = gtk_tree_view_get_model(view);
        GtkTreeIter cursor_iter;
        if (cursor_model != nullptr && gtk_tree_model_get_iter(cursor_model, &cursor_iter, cursor_path)) {
            int row_index = -1;
            gtk_tree_model_get(cursor_model, &cursor_iter, index_column, &row_index, -1);
            if (row_index >= 0 && static_cast<std::size_t>(row_index) < window.playlist_.size()) {
                window.current_track_index_ = static_cast<std::size_t>(row_index);
            }
        }
        gtk_tree_path_free(cursor_path);
    }
}

gboolean on_playlist_focus_in(GtkWidget*, GdkEventFocus*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || self->ui_closing_) {
        return FALSE;
    }
    self->sync_playlist_cursor_to_selection();
    return FALSE;
}

} // namespace pcmtp::patches
