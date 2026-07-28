#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <string>

namespace pcmtp {

class PlaylistSearchController {
public:
    class Delegate {
    public:
        virtual ~Delegate() = default;

        virtual GtkListStore* playlist_store() = 0;
        virtual GtkWidget* playlist_view() = 0;
        virtual int col_search_folded() const = 0;
        virtual bool ui_closing() const = 0;
    };

    explicit PlaylistSearchController(Delegate& delegate);
    ~PlaylistSearchController();

    void install_in_panel(GtkBox* playlist_panel);
    GtkTreeModelFilter* filter_model() const { return filter_; }

    void clear_search();
    void refilter();
    gboolean on_playlist_key_press(GtkWidget* widget, GdkEventKey* event);
    void shutdown();

private:
    static void on_search_changed(GtkEditable* editable, gpointer user_data);
    static gboolean on_filter_visible(GtkTreeModel* model, GtkTreeIter* iter, gpointer user_data);
    static gboolean on_refilter_timeout(gpointer user_data);
    static gboolean on_search_entry_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data);

    void schedule_refilter();
    void append_to_search_entry(const char* text);
    void focus_search_entry();

    Delegate& delegate_;
    GtkTreeModelFilter* filter_ = nullptr;
    GtkWidget* search_entry_ = nullptr;
    std::string filter_text_;
    bool invalidated_ = false;
    gulong search_changed_handler_id_ = 0;
    gulong search_key_press_handler_id_ = 0;
    guint refilter_timeout_id_ = 0;
};

} // namespace pcmtp
