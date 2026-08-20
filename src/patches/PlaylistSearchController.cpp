// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/patches/PlaylistSearchController.hpp"

#include <cstring>
#include <string>
#include <utility>

#include <gtk/gtk.h>

namespace pcmtp {
namespace {

std::string utf8_casefold_copy(const std::string& text) {
    gchar* folded = g_utf8_casefold(text.c_str(), -1);
    if (folded == nullptr) {
        return text;
    }
    std::string result(folded);
    g_free(folded);
    return result;
}

} // namespace

PlaylistSearchController::PlaylistSearchController(Delegate& delegate) : delegate_(delegate) {}

PlaylistSearchController::~PlaylistSearchController() {
    shutdown();
}

void PlaylistSearchController::install_in_panel(GtkBox* playlist_panel) {
    if (playlist_panel == nullptr) {
        return;
    }

    search_entry_ = gtk_search_entry_new();
    g_object_add_weak_pointer(G_OBJECT(search_entry_),
                              reinterpret_cast<gpointer*>(&search_entry_));
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry_), "Search artist, album or title");
    GtkStyleContext* search_style = gtk_widget_get_style_context(search_entry_);
    gtk_style_context_add_class(search_style, "playlist-search-entry");
    gtk_box_pack_start(playlist_panel, search_entry_, FALSE, FALSE, 0);
    gtk_box_reorder_child(playlist_panel, search_entry_, 0);
    gtk_widget_show(search_entry_);

    GtkListStore* store = delegate_.playlist_store();
    if (store != nullptr) {
        filter_ = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new(GTK_TREE_MODEL(store), nullptr));
        gtk_tree_model_filter_set_visible_func(filter_,
                                               PlaylistSearchController::on_filter_visible,
                                               this,
                                               nullptr);
    }

    search_changed_handler_id_ =
        g_signal_connect(search_entry_,
                         "search-changed",
                         G_CALLBACK(PlaylistSearchController::on_search_changed),
                         this);
    search_activate_handler_id_ =
        g_signal_connect(search_entry_,
                         "activate",
                         G_CALLBACK(PlaylistSearchController::on_search_entry_activate),
                         this);
    search_stop_handler_id_ =
        g_signal_connect(search_entry_,
                         "stop-search",
                         G_CALLBACK(PlaylistSearchController::on_stop_search),
                         this);
}

int PlaylistSearchController::search_entry_natural_height() const {
    if (search_entry_ == nullptr) {
        return 0;
    }
    gint minimum_height = 0;
    gint natural_height = 0;
    gtk_widget_get_preferred_height(search_entry_, &minimum_height, &natural_height);
    return natural_height > 0 ? natural_height : minimum_height;
}

void PlaylistSearchController::set_search_entry_visible(bool visible) {
    if (search_entry_ == nullptr) {
        return;
    }
    gtk_widget_set_visible(search_entry_, visible ? TRUE : FALSE);
}

void PlaylistSearchController::set_search_text(const std::string& text) {
    if (search_entry_ == nullptr) {
        return;
    }
    if (search_changed_handler_id_ != 0) {
        g_signal_handler_block(search_entry_, search_changed_handler_id_);
    }
    gtk_entry_set_text(GTK_ENTRY(search_entry_), text.c_str());
    if (search_changed_handler_id_ != 0) {
        g_signal_handler_unblock(search_entry_, search_changed_handler_id_);
    }
}

bool PlaylistSearchController::update_filter_text(const std::string& text) {
    std::string folded = utf8_casefold_copy(text);
    if (folded == filter_text_) {
        return false;
    }

    const bool was_active = !filter_text_.empty();
    const bool is_active = !folded.empty();
    if (!was_active && is_active) {
        delegate_.on_search_filter_started();
    }
    filter_text_ = std::move(folded);
    return true;
}

void PlaylistSearchController::apply_search_text(const std::string& text) {
    if (invalidated_ || !update_filter_text(text)) {
        return;
    }

    refilter();
    if (filter_text_.empty()) {
        delegate_.on_search_filter_cleared();
    } else {
        delegate_.on_search_filtered();
    }
}

void PlaylistSearchController::cancel_search() {
    if (invalidated_) {
        return;
    }

    const bool needs_refilter = !filter_text_.empty();
    set_search_text("");
    filter_text_.clear();
    if (needs_refilter) {
        refilter();
        delegate_.on_search_filter_cleared();
    }
    delegate_.on_search_cancelled();
}

void PlaylistSearchController::refilter() {
    if (filter_ == nullptr || invalidated_ || !GTK_IS_TREE_MODEL_FILTER(filter_)) {
        return;
    }

    delegate_.begin_refilter();
    gtk_tree_model_filter_refilter(filter_);
    delegate_.end_refilter();
}

void PlaylistSearchController::flush_pending_refilter() {
    if (invalidated_ || search_entry_ == nullptr) {
        return;
    }

    const gchar* text = gtk_entry_get_text(GTK_ENTRY(search_entry_));
    apply_search_text(text != nullptr ? text : std::string());
}

gboolean PlaylistSearchController::on_playlist_key_press(GtkWidget* widget, GdkEventKey* event) {
    if (event == nullptr || invalidated_ || delegate_.ui_closing() || search_entry_ == nullptr) {
        return FALSE;
    }
    if (gtk_widget_is_focus(search_entry_)) {
        return FALSE;
    }

    // Track activation is an application command rather than search text
    // handling. Keep the established behavior for Enter while the playlist
    // has focus; GtkEntry::activate handles Enter when the entry has focus.
    if (event->keyval == GDK_KEY_Return ||
        event->keyval == GDK_KEY_ISO_Enter ||
        event->keyval == GDK_KEY_KP_Enter) {
        delegate_.activate_filtered_playlist_selection();
        return TRUE;
    }

    const bool had_search_text =
        gtk_entry_get_text_length(GTK_ENTRY(search_entry_)) > 0;
    const gboolean handled = gtk_search_entry_handle_event(
        GTK_SEARCH_ENTRY(search_entry_),
        reinterpret_cast<GdkEvent*>(event));
    if (handled) {
        focus_search_entry();
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Escape && had_search_text) {
        return TRUE;
    }
    (void)widget;
    return FALSE;
}

void PlaylistSearchController::invalidate() {
    if (invalidated_) {
        return;
    }

    invalidated_ = true;
    if (search_entry_ != nullptr) {
        if (search_changed_handler_id_ != 0) {
            g_signal_handler_disconnect(search_entry_, search_changed_handler_id_);
            search_changed_handler_id_ = 0;
        }
        if (search_activate_handler_id_ != 0) {
            g_signal_handler_disconnect(search_entry_, search_activate_handler_id_);
            search_activate_handler_id_ = 0;
        }
        if (search_stop_handler_id_ != 0) {
            g_signal_handler_disconnect(search_entry_, search_stop_handler_id_);
            search_stop_handler_id_ = 0;
        }
    }
}

void PlaylistSearchController::shutdown() {
    invalidate();

    if (search_entry_ != nullptr) {
        GtkWidget* entry = search_entry_;
        g_object_remove_weak_pointer(G_OBJECT(entry),
                                     reinterpret_cast<gpointer*>(&search_entry_));
        search_entry_ = nullptr;
        gtk_widget_destroy(entry);
    }
    if (filter_ != nullptr) {
        g_object_unref(filter_);
        filter_ = nullptr;
    }
}

void PlaylistSearchController::on_search_changed(GtkSearchEntry* entry, gpointer user_data) {
    auto* self = static_cast<PlaylistSearchController*>(user_data);
    if (self == nullptr || self->invalidated_ || entry == nullptr) {
        return;
    }
    const gchar* text = gtk_entry_get_text(GTK_ENTRY(entry));
    self->apply_search_text(text != nullptr ? text : std::string());
}

void PlaylistSearchController::on_search_entry_activate(GtkEntry*, gpointer user_data) {
    auto* self = static_cast<PlaylistSearchController*>(user_data);
    if (self == nullptr || self->invalidated_ || self->delegate_.ui_closing()) {
        return;
    }
    self->delegate_.activate_filtered_playlist_selection();
}

void PlaylistSearchController::on_stop_search(GtkSearchEntry*, gpointer user_data) {
    auto* self = static_cast<PlaylistSearchController*>(user_data);
    if (self == nullptr || self->invalidated_) {
        return;
    }
    self->cancel_search();
}

gboolean PlaylistSearchController::on_filter_visible(GtkTreeModel* model,
                                                      GtkTreeIter* iter,
                                                      gpointer user_data) {
    auto* self = static_cast<PlaylistSearchController*>(user_data);
    if (self == nullptr || self->invalidated_ || self->filter_text_.empty()) {
        return TRUE;
    }

    gchar* folded = nullptr;
    gtk_tree_model_get(model,
                       iter,
                       self->delegate_.col_search_folded(),
                       &folded,
                       -1);
    const bool match = folded != nullptr &&
                       std::strstr(folded, self->filter_text_.c_str()) != nullptr;
    g_free(folded);
    return match ? TRUE : FALSE;
}

void PlaylistSearchController::focus_search_entry() {
    if (search_entry_ == nullptr) {
        return;
    }
    gtk_widget_grab_focus(search_entry_);
}

} // namespace pcmtp
