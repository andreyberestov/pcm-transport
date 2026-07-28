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

    gtk_box_set_spacing(playlist_panel, 10);

    search_entry_ = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry_), "Search title or artist");
    GtkStyleContext* search_style = gtk_widget_get_style_context(search_entry_);
    gtk_style_context_add_class(search_style, "playlist-search-entry");
    gtk_box_pack_start(playlist_panel, search_entry_, FALSE, FALSE, 0);

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

void PlaylistSearchController::clear_search() {
    filter_text_.clear();
    if (search_entry_ != nullptr) {
        gtk_entry_set_text(GTK_ENTRY(search_entry_), "");
    }
    refilter();
}

void PlaylistSearchController::refilter() {
    if (filter_ != nullptr && !invalidated_) {
        gtk_tree_model_filter_refilter(filter_);
    }
}

void PlaylistSearchController::schedule_refilter() {
    if (invalidated_) {
        return;
    }
    if (refilter_timeout_id_ != 0) {
        g_source_remove(refilter_timeout_id_);
    }
    refilter_timeout_id_ = g_timeout_add(kRefilterDebounceMs, PlaylistSearchController::on_refilter_timeout, this);
}

gboolean PlaylistSearchController::on_refilter_timeout(gpointer user_data) {
    auto* self = static_cast<PlaylistSearchController*>(user_data);
    if (self == nullptr || self->invalidated_) {
        return G_SOURCE_REMOVE;
    }
    self->refilter_timeout_id_ = 0;
    self->refilter();
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
            clear_search();
            return TRUE;
        }
        return FALSE;
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
        gtk_entry_set_text(GTK_ENTRY(search_entry_), next.c_str());
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
        if (!self->filter_text_.empty()) {
            self->clear_search();
            return TRUE;
        }
        return FALSE;
    }
    (void)widget;
    return FALSE;
}

void PlaylistSearchController::shutdown() {
    invalidated_ = true;
    if (refilter_timeout_id_ != 0) {
        g_source_remove(refilter_timeout_id_);
        refilter_timeout_id_ = 0;
    }
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
    if (filter_ != nullptr) {
        gtk_tree_model_filter_set_visible_func(filter_, nullptr, nullptr, nullptr);
        filter_ = nullptr;
    }
    search_entry_ = nullptr;
}

void PlaylistSearchController::on_search_changed(GtkEditable* editable, gpointer user_data) {
    auto* self = static_cast<PlaylistSearchController*>(user_data);
    if (self == nullptr || self->invalidated_) {
        return;
    }
    const gchar* text = gtk_entry_get_text(GTK_ENTRY(editable));
    self->filter_text_ = text != nullptr ? utf8_casefold_copy(text) : std::string();
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
    gtk_entry_set_text(GTK_ENTRY(search_entry_), next.c_str());
    gtk_editable_set_position(GTK_EDITABLE(search_entry_), -1);
}

} // namespace pcmtp
