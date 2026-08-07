// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/patches/PlaylistSelectionPatches.hpp"

#include "pcmtp/gui/GtkPlayerWindow.hpp"

namespace pcmtp::patches {

namespace {

bool playlist_index_from_path(GtkTreeModel* model,
                              GtkTreePath* path,
                              int index_column,
                              std::size_t* out_index) {
    if (model == nullptr || path == nullptr || out_index == nullptr) {
        return false;
    }

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
        return false;
    }

    return playlist_index_from_model_iter(model, &iter, index_column, out_index);
}

} // namespace

bool playlist_index_from_model_iter(GtkTreeModel* model,
                                    GtkTreeIter* iter,
                                    int index_column,
                                    std::size_t* out_index) {
    if (model == nullptr || iter == nullptr || out_index == nullptr) {
        return false;
    }

    int row_index = -1;
    gtk_tree_model_get(model, iter, index_column, &row_index, -1);
    if (row_index < 0) {
        return false;
    }
    *out_index = static_cast<std::size_t>(row_index);
    return true;
}

void on_playlist_selection_changed(GtkTreeSelection* selection, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || self->ui_closing_) {
        return;
    }
    self->update_selected_playlist_index_from_ui();
    (void)selection;
}

gboolean on_playlist_focus_in(GtkWidget*, GdkEventFocus*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || self->ui_closing_) {
        return FALSE;
    }
    self->sync_playlist_cursor_to_selection();
    return FALSE;
}

gboolean on_playlist_view_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || event == nullptr || self->search_controller_ == nullptr) {
        return FALSE;
    }
    return self->search_controller_->on_playlist_key_press(widget, event);
}

bool find_playlist_view_path_for_index(GtkTreeView* playlist_view,
                                       std::size_t index,
                                       int index_column,
                                       GtkTreePath** out_path) {
    if (out_path == nullptr || playlist_view == nullptr) {
        return false;
    }
    *out_path = nullptr;

    GtkTreeModel* model = gtk_tree_view_get_model(playlist_view);
    if (model == nullptr) {
        return false;
    }

    GtkTreePath* child_path = gtk_tree_path_new_from_indices(static_cast<int>(index), -1);
    if (child_path == nullptr) {
        return false;
    }

    if (GTK_IS_TREE_MODEL_FILTER(model)) {
        GtkTreeModelFilter* filter = GTK_TREE_MODEL_FILTER(model);
        GtkTreePath* filter_path = gtk_tree_model_filter_convert_child_path_to_path(filter, child_path);
        gtk_tree_path_free(child_path);
        if (filter_path == nullptr) {
            return false;
        }
        *out_path = filter_path;
        return true;
    }

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, child_path)) {
        gtk_tree_path_free(child_path);
        return false;
    }

    int row_index = -1;
    gtk_tree_model_get(model, &iter, index_column, &row_index, -1);
    if (row_index < 0 || static_cast<std::size_t>(row_index) != index) {
        gtk_tree_path_free(child_path);
        return false;
    }

    *out_path = child_path;
    return true;
}

bool playlist_index_from_view_path(GtkTreeView* playlist_view,
                                   GtkTreePath* path,
                                   int index_column,
                                   std::size_t* out_index) {
    if (playlist_view == nullptr) {
        return false;
    }
    GtkTreeModel* model = gtk_tree_view_get_model(playlist_view);
    return playlist_index_from_path(model, path, index_column, out_index);
}

} // namespace pcmtp::patches
