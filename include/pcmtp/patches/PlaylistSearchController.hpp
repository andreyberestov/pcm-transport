// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <gtk/gtk.h>

#include <string>

namespace pcmtp {

class PlaylistSearchController {
public:
    class Delegate {
    public:
        virtual ~Delegate() = default;

        virtual GtkListStore* playlist_store() = 0;
        virtual int col_search_folded() const = 0;
        virtual bool ui_closing() const = 0;
        virtual void on_search_filter_started() = 0;
        virtual void on_search_filter_cleared() = 0;
        virtual void on_search_filtered() = 0;
        virtual void on_search_cancelled() = 0;
        virtual void activate_filtered_playlist_selection() = 0;
        virtual void begin_refilter() = 0;
        virtual void end_refilter() = 0;
    };

    explicit PlaylistSearchController(Delegate& delegate);
    ~PlaylistSearchController();

    void install_in_panel(GtkBox* playlist_panel);
    GtkTreeModelFilter* filter_model() const { return filter_; }
    int search_entry_natural_height() const;
    void set_search_entry_visible(bool visible);

    void cancel_search();
    void refilter();
    void flush_pending_refilter();
    bool is_filter_active() const { return !filter_text_.empty(); }
    gboolean on_playlist_key_press(GtkWidget* widget, GdkEventKey* event);
    void invalidate();
    void shutdown();

private:
    static void on_search_changed(GtkSearchEntry* entry, gpointer user_data);
    static void on_search_entry_activate(GtkEntry* entry, gpointer user_data);
    static void on_stop_search(GtkSearchEntry* entry, gpointer user_data);
    static gboolean on_filter_visible(GtkTreeModel* model,
                                      GtkTreeIter* iter,
                                      gpointer user_data);

    void focus_search_entry();
    void set_search_text(const std::string& text);
    bool update_filter_text(const std::string& text);
    void apply_search_text(const std::string& text);

    Delegate& delegate_;
    GtkTreeModelFilter* filter_ = nullptr;
    GtkWidget* search_entry_ = nullptr;
    std::string filter_text_;
    bool invalidated_ = false;
    gulong search_changed_handler_id_ = 0;
    gulong search_activate_handler_id_ = 0;
    gulong search_stop_handler_id_ = 0;
};

} // namespace pcmtp
