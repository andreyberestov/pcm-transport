// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/patches/PlaylistSearchController.hpp"

#include <cstring>
#include <string>

#include <gtk/gtk.h>

namespace pcmtp {
namespace {

constexpr guint kRefilterDebounceMs = 50;

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
        g_signal_connect(search_entry_, "changed", G_CALLBACK(PlaylistSearchController::on_search_changed), this);
    search_key_press_handler_id_ = g_signal_connect(search_entry_,
                                                    "key-press-event",
                                                    G_CALLBACK(PlaylistSearchController::on_search_entry_key_press),
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

void PlaylistSearchController::cancel_pending_refilter() {
    if (refilter_timeout_id_ != 0) {
        g_source_remove(refilter_timeout_id_);
        refilter_timeout_id_ = 0;
    }
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

void PlaylistSearchController::update_filter_text(const std::string& text) {
    const bool was_active = !filter_text_.empty();
    std::string folded = utf8_casefold_copy(text);
    const bool is_active = !folded.empty();
    if (!was_active && is_active) {
        delegate_.on_search_filter_started();
    }
    filter_text_ = folded;
}

void PlaylistSearchController::cancel_search() {
    if (invalidated_) {
        return;
    }

    const bool needs_refilter = refilter_timeout_id_ != 0 || !filter_text_.empty();
    cancel_pending_refilter();
    filter_text_.clear();
    set_search_text("");
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
    if (invalidated_ || refilter_timeout_id_ == 0) {
        return;
    }
    cancel_pending_refilter();
    refilter();
    if (filter_text_.empty()) {
        delegate_.on_search_filter_cleared();
    } else {
        delegate_.on_search_filtered();
    }
}

void PlaylistSearchController::schedule_refilter() {
    if (invalidated_) {
        return;
    }
    cancel_pending_refilter();
    refilter_timeout_id_ = g_timeout_add(kRefilterDebounceMs, PlaylistSearchController::on_refilter_timeout, this);
}

gboolean PlaylistSearchController::on_refilter_timeout(gpointer user_data) {
    auto* self = static_cast<PlaylistSearchController*>(user_data);
    if (self == nullptr || self->invalidated_) {
        return G_SOURCE_REMOVE;
    }
    self->refilter_timeout_id_ = 0;
    self->refilter();
    if (self->filter_text_.empty()) {
        self->delegate_.on_search_filter_cleared();
    } else {
        self->delegate_.on_search_filtered();
    }
    return G_SOURCE_REMOVE;
}

gboolean PlaylistSearchController::on_playlist_key_press(GtkWidget* widget, GdkEventKey* event) {
    if (event == nullptr || invalidated_ || delegate_.ui_closing()) {
        return FALSE;
    }
    if (search_entry_ != nullptr && gtk_widget_is_focus(search_entry_)) {
        return FALSE;
    }

    if (event->keyval == GDK_KEY_Escape) {
        if (!filter_text_.empty()) {
            cancel_search();
            return TRUE;
        }
        return FALSE;
    }

    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_ISO_Enter || event->keyval == GDK_KEY_KP_Enter) {
        delegate_.activate_filtered_playlist_selection();
        return TRUE;
    }

    if (event->keyval == GDK_KEY_BackSpace) {
        focus_search_entry();
        if (search_entry_ == nullptr) {
            return FALSE;
        }
        const gchar* text = gtk_entry_get_text(GTK_ENTRY(search_entry_));
        if (text == nullptr || *text == '\0') {
            return TRUE;
        }
        const char* end = text + std::strlen(text);
        const char* prev = g_utf8_find_prev_char(text, end);
        std::string next;
        if (prev != nullptr) {
            next.assign(text, static_cast<std::size_t>(prev - text));
        }
        set_search_text(next);
        update_filter_text(next);
        schedule_refilter();
        return TRUE;
    }

    if ((event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK)) != 0) {
        return FALSE;
    }

    guint32 unicode = gdk_keyval_to_unicode(event->keyval);
    if (unicode == 0 || !g_unichar_isprint(unicode)) {
        return FALSE;
    }

    char buffer[8];
    const gint written = g_unichar_to_utf8(unicode, buffer);
    if (written <= 0) {
        return FALSE;
    }
    buffer[written] = '\0';
    focus_search_entry();
    append_to_search_entry(buffer);
    (void)widget;
    return TRUE;
}

gboolean PlaylistSearchController::on_search_entry_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data) {
    auto* self = static_cast<PlaylistSearchController*>(user_data);
    if (self == nullptr || event == nullptr || self->invalidated_) {
        return FALSE;
    }
    if (event->keyval == GDK_KEY_Escape) {
        self->cancel_search();
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_ISO_Enter || event->keyval == GDK_KEY_KP_Enter) {
        self->delegate_.activate_filtered_playlist_selection();
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
    cancel_pending_refilter();
    if (search_entry_ != nullptr) {
        if (search_changed_handler_id_ != 0) {
            g_signal_handler_disconnect(search_entry_, search_changed_handler_id_);
            search_changed_handler_id_ = 0;
        }
        if (search_key_press_handler_id_ != 0) {
            g_signal_handler_disconnect(search_entry_, search_key_press_handler_id_);
            search_key_press_handler_id_ = 0;
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

void PlaylistSearchController::on_search_changed(GtkEditable* editable, gpointer user_data) {
    auto* self = static_cast<PlaylistSearchController*>(user_data);
    if (self == nullptr || self->invalidated_) {
        return;
    }
    const gchar* text = gtk_entry_get_text(GTK_ENTRY(editable));
    self->update_filter_text(text != nullptr ? text : std::string());
    self->schedule_refilter();
}

gboolean PlaylistSearchController::on_filter_visible(GtkTreeModel* model, GtkTreeIter* iter, gpointer user_data) {
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

void PlaylistSearchController::append_to_search_entry(const char* text) {
    if (search_entry_ == nullptr || text == nullptr || *text == '\0') {
        return;
    }
    const gchar* current = gtk_entry_get_text(GTK_ENTRY(search_entry_));
    std::string next = current != nullptr ? current : std::string();
    next.append(text);
    set_search_text(next);
    update_filter_text(next);
    schedule_refilter();
    gtk_editable_set_position(GTK_EDITABLE(search_entry_), -1);
}

} // namespace pcmtp
