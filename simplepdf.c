/* SimplePDF: a quiet GTK/Evince PDF reader. GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include <gtk/gtk.h>
#include <evince-document.h>
#include <evince-view.h>
#include <math.h>
#include "simplepdf-state.h"

#define VERSION "1.0.0"
#define HISTORY_LIMIT 64

typedef struct {
    GtkWidget *window, *header, *stack, *view, *scroll, *page, *pages, *zoom;
    GtkWidget *searchbar, *search, *matches, *sidebar, *outline, *message, *infobar, *status;
    EvDocumentModel *model;
    EvJob *load, *find, *links;
    GFile *file;
    GFileMonitor *monitor;
    char *pending_uri;
    PdfPosition restore, back[HISTORY_LIMIT], forward[HISTORY_LIMIT];
    guint n_back, n_forward, save_source, search_source, reload_source, restore_source;
    guint generation, restore_attempts;
    gboolean updating, restoring, reloading, focus, fullscreen, closing;
} Reader;
static GtkApplication *application;
static void open_uri(Reader *, const char *, gboolean);
static void action(Reader *, const char *);
static void update_ui(Reader *);
static void start_search(Reader *);

static EvDocument *document(Reader *r) { return ev_document_model_get_document(r->model); }
static double normal_scale(Reader *r)
{
    double dpi = gdk_screen_get_resolution(gtk_widget_get_screen(r->window));
    return (dpi > 0 ? dpi : 96.0) / 72.0;
}
static void cancel_job(EvJob **job, Reader *r)
{
    if (!*job) return;
    g_signal_handlers_disconnect_by_data(*job, r);
    ev_job_cancel(*job);
    g_clear_object(job);
}
static void remove_source(guint *source)
{
    if (*source) { g_source_remove(*source); *source = 0; }
}
static void message(Reader *r, const char *text)
{
    gtk_label_set_text(GTK_LABEL(r->message), text);
    gtk_widget_set_visible(r->infobar, text && *text);
}
static PdfPosition capture(Reader *r)
{
    PdfPosition p = { .page = MAX(0, ev_document_model_get_page(r->model)),
        .rotation = ev_document_model_get_rotation(r->model),
        .sizing = ev_document_model_get_sizing_mode(r->model),
        .scale = ev_document_model_get_scale(r->model), .x = .5, .y = .5 };
    gtk_window_get_size(GTK_WINDOW(r->window), &p.width, &p.height);
    GdkRectangle area; GtkBorder border;
    if (document(r) && ev_view_get_page_extents(EV_VIEW(r->view), p.page, &area, &border)) {
        GtkAdjustment *h = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(r->scroll));
        GtkAdjustment *v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(r->scroll));
        p.x = CLAMP((gtk_adjustment_get_value(h) + gtk_adjustment_get_page_size(h)/2 - area.x) / MAX(1,area.width), 0, 1);
        p.y = CLAMP((gtk_adjustment_get_value(v) + gtk_adjustment_get_page_size(v)/2 - area.y) / MAX(1,area.height), 0, 1);
    }
    return p;
}
static gboolean save_state(gpointer data)
{
    Reader *r = data; r->save_source = 0;
    if (r->file && document(r) && !r->restoring) {
        PdfPosition p = capture(r);
        g_autofree char *uri = g_file_get_uri(r->file);
        g_autoptr(GError) error = NULL;
        if (!pdf_state_save(uri, &p, &error) && error) message(r, error->message);
    }
    return G_SOURCE_REMOVE;
}
static void schedule_save(Reader *r)
{
    if (!r->file || r->restoring || r->closing) return;
    remove_source(&r->save_source);
    r->save_source = g_timeout_add(800, save_state, r);
}
static gboolean restore_anchor(gpointer data)
{
    Reader *r = data;
    GdkRectangle area; GtkBorder border;
    if (ev_view_get_page_extents(EV_VIEW(r->view), r->restore.page, &area, &border) && area.width > 1) {
        GtkAdjustment *h = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(r->scroll));
        GtkAdjustment *v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(r->scroll));
        gtk_adjustment_set_value(h, area.x + area.width*r->restore.x - gtk_adjustment_get_page_size(h)/2);
        gtk_adjustment_set_value(v, area.y + area.height*r->restore.y - gtk_adjustment_get_page_size(v)/2);
    }
    /* EvView may allocate once more after sizing/scrollbar negotiation. */
    if (++r->restore_attempts < 3) return G_SOURCE_CONTINUE;
    r->restore_source = 0; r->restoring = FALSE;
    return G_SOURCE_REMOVE;
}
static void restore_position(Reader *r, PdfPosition p)
{
    if (!document(r)) return;
    remove_source(&r->restore_source);
    r->restoring = TRUE;
    p.page = CLAMP(p.page, 0, ev_document_get_n_pages(document(r))-1);
    r->restore = p; r->restore_attempts = 0;
    ev_document_model_set_rotation(r->model, p.rotation);
    ev_document_model_set_sizing_mode(r->model, p.sizing);
    if (p.sizing == EV_SIZING_FREE) ev_document_model_set_scale(r->model, p.scale);
    ev_document_model_set_page(r->model, p.page);
    r->restore_source = g_timeout_add(20, restore_anchor, r);
}
static void push(PdfPosition *items, guint *length, PdfPosition p)
{
    if (*length == HISTORY_LIMIT) { memmove(items, items+1, (HISTORY_LIMIT-1)*sizeof(*items)); (*length)--; }
    items[(*length)++] = p;
}
static void remember(Reader *r)
{
    if (!document(r) || r->restoring) return;
    push(r->back, &r->n_back, capture(r)); r->n_forward = 0;
}
static void history(Reader *r, gboolean forward)
{
    PdfPosition *from = forward ? r->forward : r->back;
    guint *n = forward ? &r->n_forward : &r->n_back;
    if (!*n || !document(r)) return;
    push(forward ? r->back : r->forward, forward ? &r->n_back : &r->n_forward, capture(r));
    restore_position(r, from[--(*n)]);
}
static void changed(GObject *object, GParamSpec *property, Reader *r)
{
    (void)object; (void)property; update_ui(r); schedule_save(r);
}
static void scrolled(GtkAdjustment *adjustment, Reader *r)
{
    (void)adjustment; schedule_save(r);
}
static void update_ui(Reader *r)
{
    EvDocument *doc = document(r);
    r->updating = TRUE;
    if (doc) {
        int page = ev_document_model_get_page(r->model);
        g_autofree char *label = ev_document_get_page_label(doc, page);
        g_autofree char *total = g_strdup_printf("%d / %d", page+1, ev_document_get_n_pages(doc));
        if (!gtk_widget_has_focus(r->page)) gtk_entry_set_text(GTK_ENTRY(r->page), label ? label : "");
        gtk_label_set_text(GTK_LABEL(r->pages), total);
        if (!gtk_widget_has_focus(r->zoom)) {
            g_autofree char *text = g_strdup_printf("%.0f%%", 100*ev_document_model_get_scale(r->model)/normal_scale(r));
            gtk_entry_set_text(GTK_ENTRY(r->zoom), text);
        }
    }
    gtk_widget_set_sensitive(r->page, doc != NULL);
    gtk_widget_set_sensitive(r->zoom, doc != NULL);
    r->updating = FALSE;
}
static void page_activate(GtkEntry *entry, Reader *r)
{
    if (!document(r)) return;
    const char *text = gtk_entry_get_text(entry);
    int page;
    if (ev_document_find_page_by_label(document(r), text, &page)) {
        remember(r); ev_document_model_set_page(r->model, page); gtk_widget_grab_focus(r->view);
    } else message(r, "That page number or label is not in this document.");
}
static void zoom_activate(GtkEntry *entry, Reader *r)
{
    char *end; double value = g_ascii_strtod(gtk_entry_get_text(entry), &end);
    if (*end == '%') end++;
    if (isfinite(value) && value >= 10 && value <= 1000 && !*end) {
        PdfPosition p = capture(r); p.sizing = EV_SIZING_FREE; p.scale = value/100*normal_scale(r);
        restore_position(r,p); gtk_widget_grab_focus(r->view);
    } else { message(r,"Enter a zoom from 10% to 1000%."); update_ui(r); }
}
static void search_updated(EvJobFind *job, int page, Reader *r)
{
    (void)page;
    if (r->find != EV_JOB(job)) return;
    int n = 0;
    for (int i=0; i<ev_document_get_n_pages(document(r)); i++) n += ev_job_find_get_n_main_results(job,i);
    g_autofree char *text = g_strdup_printf("%d match%s%s",n,n==1?"":"es",EV_JOB(job)->finished?"":" · searching…");
    gtk_label_set_text(GTK_LABEL(r->matches),text);
}
static void search_finished(EvJob *job, Reader *r)
{
    if (r->find == job) search_updated(EV_JOB_FIND(job),0,r);
}
static gboolean search_timeout(gpointer data)
{
    Reader *r=data; r->search_source=0; start_search(r); return G_SOURCE_REMOVE;
}
static void search_changed(GtkSearchEntry *entry, Reader *r)
{
    (void)entry; remove_source(&r->search_source);
    r->search_source=g_timeout_add(160,search_timeout,r);
}
static void start_search(Reader *r)
{
    ev_view_find_cancel(EV_VIEW(r->view)); cancel_job(&r->find,r);
    gtk_label_set_text(GTK_LABEL(r->matches),"");
    const char *text=gtk_entry_get_text(GTK_ENTRY(r->search));
    if (!document(r) || !*text) return;
    remember(r);
    r->find=ev_job_find_new(document(r),ev_document_model_get_page(r->model),ev_document_get_n_pages(document(r)),text,FALSE);
    g_signal_connect(r->find,"updated",G_CALLBACK(search_updated),r);
    g_signal_connect(r->find,"finished",G_CALLBACK(search_finished),r);
    ev_view_find_started(EV_VIEW(r->view),EV_JOB_FIND(r->find));
    ev_view_find_set_highlight_search(EV_VIEW(r->view),TRUE);
    ev_job_scheduler_push_job(r->find,EV_JOB_PRIORITY_NONE);
}
static void next_match(GtkWidget *widget, Reader *r)
{
    (void)widget; action(r,"next-match");
}
static void previous_match(GtkWidget *widget, Reader *r)
{
    (void)widget; action(r,"previous-match");
}
static void outline_ready(EvJob *job, Reader *r)
{
    if (r->links != job || job->failed) return;
    gtk_tree_view_set_model(GTK_TREE_VIEW(r->outline),ev_job_links_get_model(EV_JOB_LINKS(job)));
    GtkTreePath *path=gtk_tree_path_new_first();
    gtk_tree_view_expand_to_path(GTK_TREE_VIEW(r->outline),path);gtk_tree_path_free(path);
}
static void outline_activate(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column, Reader *r)
{
    (void)column; GtkTreeIter iter; GtkTreeModel *model=gtk_tree_view_get_model(tree);
    EvLink *link=NULL;
    if (model && gtk_tree_model_get_iter(model,&iter,path)) {
        gtk_tree_model_get(model,&iter,EV_DOCUMENT_LINKS_COLUMN_LINK,&link,-1);
        if (link) { ev_view_handle_link(EV_VIEW(r->view),link); g_object_unref(link); gtk_widget_grab_focus(r->view); }
    }
}
static void link_handled(EvView *view, EvLink *link, Reader *r)
{
    (void)view; (void)link; remember(r);
}
static void external_link(EvView *view, EvLinkAction *link, Reader *r)
{
    (void)view;
    if (ev_link_action_get_action_type(link) != EV_LINK_ACTION_TYPE_EXTERNAL_URI) {
        message(r,"This document action is not supported."); return;
    }
    const char *uri=ev_link_action_get_uri(link);
    g_autofree char *scheme=uri?g_uri_parse_scheme(uri):NULL;
    if (!scheme || (g_ascii_strcasecmp(scheme,"https") && g_ascii_strcasecmp(scheme,"http") && g_ascii_strcasecmp(scheme,"mailto"))) {
        message(r,"This kind of link cannot be opened."); return;
    }
    g_autoptr(GError) error=NULL;
    if (!gtk_show_uri_on_window(GTK_WINDOW(r->window),uri,GDK_CURRENT_TIME,&error)) message(r,error->message);
}
static gboolean reload_timeout(gpointer data)
{
    Reader *r=data; r->reload_source=0;
    if (r->file && !r->load) { g_autofree char *uri=g_file_get_uri(r->file); open_uri(r,uri,TRUE); }
    return G_SOURCE_REMOVE;
}
static void file_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, Reader *r)
{
    (void)monitor; (void)file; (void)other;
    if (event!=G_FILE_MONITOR_EVENT_CHANGED && event!=G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
        event!=G_FILE_MONITOR_EVENT_CREATED && event!=G_FILE_MONITOR_EVENT_RENAMED) return;
    remove_source(&r->reload_source); r->reload_source=g_timeout_add(400,reload_timeout,r);
}
static void password_response(GtkDialog *dialog, int response, GtkWidget *window)
{
    Reader *r=g_object_get_data(G_OBJECT(window),"reader");
    if (r && !r->closing && response==GTK_RESPONSE_ACCEPT &&
        r->generation==GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(dialog),"generation"))) {
        GtkWidget *entry=g_object_get_data(G_OBJECT(dialog),"password");
        /* EvJobLoad applies a password to the encrypted document retained by
           its first failed load. A fresh job would ignore that password. */
        if (r->load && r->load->document) {
            ev_job_load_set_password(EV_JOB_LOAD(r->load),gtk_entry_get_text(GTK_ENTRY(entry)));
            gtk_label_set_text(GTK_LABEL(r->status),"Opening PDF…");gtk_widget_show(r->status);
            ev_job_scheduler_push_job(r->load,EV_JOB_PRIORITY_HIGH);
        }
    }
    if (r && response!=GTK_RESPONSE_ACCEPT) cancel_job(&r->load,r);
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void load_finished(EvJob *job, Reader *r)
{
    if (r->load!=job || job->cancelled) return;
    gtk_label_set_text(GTK_LABEL(r->status),"");gtk_widget_hide(r->status);
    if (job->failed) {
        if (g_error_matches(job->error,EV_DOCUMENT_ERROR,EV_DOCUMENT_ERROR_ENCRYPTED)) {
            GtkWidget *dialog=gtk_dialog_new_with_buttons("Unlock PDF",GTK_WINDOW(r->window),
                GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,"Cancel",GTK_RESPONSE_CANCEL,"Unlock",GTK_RESPONSE_ACCEPT,NULL);
            GtkWidget *entry=gtk_entry_new(); gtk_entry_set_visibility(GTK_ENTRY(entry),FALSE);
            gtk_entry_set_placeholder_text(GTK_ENTRY(entry),"Document password");
            gtk_entry_set_activates_default(GTK_ENTRY(entry),TRUE);
            gtk_container_set_border_width(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),18);
            gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),entry);
            gtk_dialog_set_default_response(GTK_DIALOG(dialog),GTK_RESPONSE_ACCEPT);
            g_object_set_data(G_OBJECT(dialog),"password",entry);
            g_object_set_data(G_OBJECT(dialog),"generation",GUINT_TO_POINTER(r->generation));
            g_signal_connect_object(dialog,"response",G_CALLBACK(password_response),r->window,0);
            gtk_widget_show_all(dialog);
            return;
        } else message(r,job->error?job->error->message:"The PDF could not be opened.");
        cancel_job(&r->load,r); return;
    }
    if (!job->document || ev_document_get_n_pages(job->document)<1) {
        message(r,"This document has no readable pages."); cancel_job(&r->load,r); return;
    }
    gboolean reload=r->reloading;
    PdfPosition old=capture(r);
    if (!reload) { remove_source(&r->save_source); save_state(r); }
    ev_view_find_cancel(EV_VIEW(r->view)); cancel_job(&r->find,r); cancel_job(&r->links,r);
    gtk_entry_set_text(GTK_ENTRY(r->search),""); remove_source(&r->search_source);
    g_clear_object(&r->monitor); g_clear_object(&r->file);
    r->file=g_file_new_for_uri(r->pending_uri);
    ev_document_model_set_document(r->model,job->document);
    ev_document_model_set_sizing_mode(r->model,EV_SIZING_FIT_WIDTH);
    if (!reload) { ev_document_model_set_rotation(r->model,0); r->n_back=r->n_forward=0; }
    gtk_tree_view_set_model(GTK_TREE_VIEW(r->outline),NULL);
    if (EV_IS_DOCUMENT_LINKS(job->document)) {
        r->links=ev_job_links_new(job->document);
        g_signal_connect(r->links,"finished",G_CALLBACK(outline_ready),r);
        ev_job_scheduler_push_job(r->links,EV_JOB_PRIORITY_LOW);
    }
    r->monitor=g_file_monitor_file(r->file,G_FILE_MONITOR_WATCH_MOVES,NULL,NULL);
    if (r->monitor) g_signal_connect(r->monitor,"changed",G_CALLBACK(file_changed),r);
    g_autofree char *basename=g_file_get_basename(r->file);
    g_autofree char *title=g_strdup_printf("%s — SimplePDF",basename);
    gtk_window_set_title(GTK_WINDOW(r->window),title);
    gtk_header_bar_set_title(GTK_HEADER_BAR(r->header),basename);
    gtk_stack_set_visible_child_name(GTK_STACK(r->stack),"document"); message(r,"");
    PdfPosition p;
    if (reload) restore_position(r,old);
    else if (pdf_state_load(r->pending_uri,&p)) {
        gtk_window_resize(GTK_WINDOW(r->window),p.width,p.height); restore_position(r,p);
    }
    cancel_job(&r->load,r); update_ui(r); gtk_widget_grab_focus(r->view);
}
static void open_uri(Reader *r, const char *uri, gboolean reload)
{
    g_autoptr(GFile) file=g_file_new_for_uri(uri);
    if (!g_file_is_native(file)) { message(r,"Open a local PDF file."); return; }
    /* Check content, not the filename extension, before enabling a backend. */
    g_autoptr(GFileInputStream) input=g_file_read(file,NULL,NULL);
    char header[1024]; gssize length=input?g_input_stream_read(G_INPUT_STREAM(input),header,sizeof header,NULL,NULL):-1;
    if (length<5 || !g_strstr_len(header,length,"%PDF-")) { message(r,"This file is not a readable PDF. Use SimpleBook for ebooks."); return; }
    cancel_job(&r->load,r); remove_source(&r->reload_source);
    g_free(r->pending_uri); r->pending_uri=g_strdup(uri); r->reloading=reload; r->generation++;
    r->load=ev_job_load_new(uri);
    g_signal_connect(r->load,"finished",G_CALLBACK(load_finished),r);
    gtk_label_set_text(GTK_LABEL(r->status),"Opening PDF…");gtk_widget_show(r->status);
    ev_job_scheduler_push_job(r->load,EV_JOB_PRIORITY_HIGH);
}
static void file_chooser_response(GtkDialog *dialog,int response,GtkWidget *window)
{
    Reader *r=g_object_get_data(G_OBJECT(window),"reader");
    if (r && response==GTK_RESPONSE_ACCEPT) {
        g_autofree char *uri=gtk_file_chooser_get_uri(GTK_FILE_CHOOSER(dialog));
        if (uri) open_uri(r,uri,FALSE);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void choose_file(Reader *r)
{
    GtkWidget *dialog=gtk_file_chooser_dialog_new("Open PDF",GTK_WINDOW(r->window),GTK_FILE_CHOOSER_ACTION_OPEN,
        "Cancel",GTK_RESPONSE_CANCEL,"Open",GTK_RESPONSE_ACCEPT,NULL);
    GtkFileFilter *filter=gtk_file_filter_new(); gtk_file_filter_set_name(filter,"PDF documents");
    gtk_file_filter_add_mime_type(filter,"application/pdf"); gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog),filter);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog),TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog),TRUE);
    g_signal_connect_object(dialog,"response",G_CALLBACK(file_chooser_response),r->window,0);
    gtk_widget_show(dialog);
}
static void action(Reader *r,const char *name)
{
    if (!strcmp(name,"open")) { choose_file(r); return; }
    if (!strcmp(name,"close")) { gtk_window_close(GTK_WINDOW(r->window)); return; }
    if (!strcmp(name,"help")) {
        GtkWidget *dialog=gtk_message_dialog_new(GTK_WINDOW(r->window),GTK_DIALOG_DESTROY_WITH_PARENT,GTK_MESSAGE_INFO,GTK_BUTTONS_CLOSE,
            "SimplePDF keyboard shortcuts");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
            "Ctrl+O  Open\nCtrl+F or /  Find\nCtrl+G / Ctrl+Shift+G  Next / previous match\nCtrl+L  Go to page\nCtrl+plus / minus  Zoom\nCtrl+0  100%%\nAlt+Left / Right  Back / forward\nF9  Contents\nF11  Fullscreen\nF7  Caret navigation\ni  Focus mode\nCtrl+W or q  Close\n\nPinch to zoom; two-finger scroll to read.");
        g_signal_connect(dialog,"response",G_CALLBACK(gtk_widget_destroy),NULL); gtk_widget_show(dialog); return;
    }
    if (!strcmp(name,"fullscreen")) {
        r->fullscreen=!r->fullscreen;
        if (r->fullscreen) gtk_window_fullscreen(GTK_WINDOW(r->window)); else gtk_window_unfullscreen(GTK_WINDOW(r->window));
        return;
    }
    if (!strcmp(name,"focus")) { r->focus=!r->focus; gtk_widget_set_visible(r->header,!r->focus); return; }
    if (!document(r)) return;
    if (!strcmp(name,"search")) { gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(r->searchbar),TRUE); gtk_widget_grab_focus(r->search); }
    else if (!strcmp(name,"page")) { gtk_widget_grab_focus(r->page); gtk_editable_select_region(GTK_EDITABLE(r->page),0,-1); }
    else if (!strcmp(name,"contents")) gtk_widget_set_visible(r->sidebar,!gtk_widget_get_visible(r->sidebar));
    else if (!strcmp(name,"copy")) ev_view_copy(EV_VIEW(r->view));
    else if (!strcmp(name,"back")) history(r,FALSE);
    else if (!strcmp(name,"forward")) history(r,TRUE);
    else if (!strcmp(name,"next-match")) { remember(r); ev_view_find_next(EV_VIEW(r->view)); }
    else if (!strcmp(name,"previous-match")) { remember(r); ev_view_find_previous(EV_VIEW(r->view)); }
    else if (!strcmp(name,"caret")) ev_view_set_caret_navigation_enabled(EV_VIEW(r->view),!ev_view_is_caret_navigation_enabled(EV_VIEW(r->view)));
    else if (!strcmp(name,"fit-width")) ev_document_model_set_sizing_mode(r->model,EV_SIZING_FIT_WIDTH);
    else if (!strcmp(name,"fit-page")) ev_document_model_set_sizing_mode(r->model,EV_SIZING_FIT_PAGE);
    else if (!strcmp(name,"zoom-in") || !strcmp(name,"zoom-out") || !strcmp(name,"actual-size")) {
        PdfPosition p=capture(r); p.sizing=EV_SIZING_FREE;
        p.scale=!strcmp(name,"actual-size")?normal_scale(r):p.scale*(!strcmp(name,"zoom-in")?1.2:1/1.2);
        p.scale=CLAMP(p.scale,.1*normal_scale(r),10*normal_scale(r)); restore_position(r,p);
    } else if (!strcmp(name,"rotate-left") || !strcmp(name,"rotate-right")) {
        PdfPosition p=capture(r); p.rotation=(p.rotation+(!strcmp(name,"rotate-left")?270:90))%360; restore_position(r,p);
    }
}
static gboolean key_press(GtkWidget *widget,GdkEventKey *event,Reader *r)
{
    (void)widget;
    GtkWidget *focus=gtk_window_get_focus(GTK_WINDOW(r->window));
    gboolean typing=GTK_IS_EDITABLE(focus), ctrl=event->state&GDK_CONTROL_MASK, alt=event->state&GDK_MOD1_MASK;
    guint key=gdk_keyval_to_lower(event->keyval); const char *name=NULL;
    if (key==GDK_KEY_Escape) {
        if (gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(r->searchbar))) {
            gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(r->searchbar),FALSE); ev_view_find_cancel(EV_VIEW(r->view));
            remove_source(&r->search_source); cancel_job(&r->find,r); gtk_widget_grab_focus(r->view);
        } else if (r->fullscreen) action(r,"fullscreen");
        else if (r->focus) action(r,"focus");
        else if (typing) gtk_widget_grab_focus(r->view);
        return TRUE;
    }
    if (ctrl) {
        switch (key) {
        case GDK_KEY_o:name="open";break; case GDK_KEY_w:name="close";break;
        case GDK_KEY_f:name="search";break; case GDK_KEY_l:name="page";break;
        case GDK_KEY_g:name=(event->state&GDK_SHIFT_MASK)?"previous-match":"next-match";break;
        case GDK_KEY_c:if (!typing) name="copy";break;
        case GDK_KEY_plus:case GDK_KEY_equal:case GDK_KEY_KP_Add:name="zoom-in";break;
        case GDK_KEY_minus:case GDK_KEY_KP_Subtract:name="zoom-out";break;
        case GDK_KEY_0:name="actual-size";break;
        }
    } else if (alt) {
        if (key==GDK_KEY_Left) name="back"; else if (key==GDK_KEY_Right) name="forward";
    } else if (key==GDK_KEY_F11) name="fullscreen";
    else if (!typing) {
        switch(key) {
        case GDK_KEY_F9:name="contents";break; case GDK_KEY_F7:name="caret";break;
        case GDK_KEY_slash:name="search";break; case GDK_KEY_question:name="help";break;
        case GDK_KEY_i:name="focus";break; case GDK_KEY_q:name="close";break;
        case GDK_KEY_j:case GDK_KEY_k:
            if (ev_view_is_caret_navigation_enabled(EV_VIEW(r->view))) break;
            g_signal_emit_by_name(r->view,"scroll",key==GDK_KEY_j?GTK_SCROLL_STEP_FORWARD:GTK_SCROLL_STEP_BACKWARD,FALSE);return TRUE;
        }
    }
    if (focus==r->search && (key==GDK_KEY_Return || key==GDK_KEY_KP_Enter)) name=(event->state&GDK_SHIFT_MASK)?"previous-match":"next-match";
    if (name) { action(r,name); return TRUE; }
    return FALSE;
}
static void clicked(GtkWidget *widget,Reader *r) { action(r,g_object_get_data(G_OBJECT(widget),"action")); }
static GtkWidget *button(Reader *r,const char *icon,const char *tip,const char *name)
{
    GtkWidget *b=gtk_button_new_from_icon_name(icon,GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(b,tip); g_object_set_data(G_OBJECT(b),"action",(gpointer)name);
    g_signal_connect(b,"clicked",G_CALLBACK(clicked),r); return b;
}
static void drop_received(GtkWidget *widget,GdkDragContext *context,int x,int y,GtkSelectionData *selection,guint info,guint time,Reader *r)
{
    (void)widget;(void)x;(void)y;(void)info;
    g_auto(GStrv) uris=gtk_selection_data_get_uris(selection);
    if (uris && uris[0]) open_uri(r,uris[0],FALSE);
    gtk_drag_finish(context,uris && uris[0],FALSE,time);
}
static void destroyed(GtkWidget *widget,Reader *r)
{
    (void)widget; r->closing=TRUE;
    remove_source(&r->save_source); save_state(r);
    remove_source(&r->restore_source); remove_source(&r->search_source); remove_source(&r->reload_source);
    cancel_job(&r->load,r); cancel_job(&r->find,r); cancel_job(&r->links,r);
    g_clear_object(&r->monitor); g_clear_object(&r->file); g_clear_object(&r->model);
    g_free(r->pending_uri); g_free(r);
}
static Reader *new_reader(void)
{
    Reader *r=g_new0(Reader,1);
    r->window=gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(r->window),"SimplePDF"); gtk_window_set_default_size(GTK_WINDOW(r->window),920,780);
    gtk_window_set_icon_name(GTK_WINDOW(r->window),"application-pdf");
    const char *icon_path="/usr/local/share/simplesuite/simplepdf/simplepdf.svg";
    if (g_file_test(icon_path,G_FILE_TEST_IS_REGULAR)) gtk_window_set_icon_from_file(GTK_WINDOW(r->window),icon_path,NULL);
    g_object_set_data(G_OBJECT(r->window),"reader",r);
    r->model=ev_document_model_new(); ev_document_model_set_continuous(r->model,TRUE);
    ev_document_model_set_page_layout(r->model,EV_PAGE_LAYOUT_SINGLE);
    ev_document_model_set_min_scale(r->model,.1*normal_scale(r)); ev_document_model_set_max_scale(r->model,10*normal_scale(r));
    r->header=gtk_header_bar_new(); gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(r->header),TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(r->header),"SimplePDF"); gtk_window_set_titlebar(GTK_WINDOW(r->window),r->header);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(r->header),button(r,"document-open-symbolic","Open PDF (Ctrl+O)","open"));
    gtk_header_bar_pack_start(GTK_HEADER_BAR(r->header),button(r,"go-previous-symbolic","Back (Alt+Left)","back"));
    GtkWidget *pagebox=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
    r->page=gtk_entry_new(); gtk_entry_set_width_chars(GTK_ENTRY(r->page),5);gtk_entry_set_max_width_chars(GTK_ENTRY(r->page),10);
    gtk_widget_set_tooltip_text(r->page,"Page number or PDF label (Ctrl+L)");
    r->pages=gtk_label_new("");gtk_box_pack_start(GTK_BOX(pagebox),r->page,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(pagebox),r->pages,FALSE,FALSE,0);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(r->header),pagebox);
    GtkWidget *menu=gtk_menu_new(), *menu_button=gtk_menu_button_new();
    const char *items[][2]={{"Fit width","fit-width"},{"Fit page","fit-page"},{"100%","actual-size"},{"Contents (F9)","contents"},
        {"Forward","forward"},{"Rotate left","rotate-left"},{"Rotate right","rotate-right"},{"Focus mode (i)","focus"},
        {"Fullscreen (F11)","fullscreen"},{"Caret navigation (F7)","caret"},{"Keyboard shortcuts","help"}};
    for (guint i=0;i<G_N_ELEMENTS(items);i++) {
        GtkWidget *item=gtk_menu_item_new_with_label(items[i][0]);g_object_set_data(G_OBJECT(item),"action",(gpointer)items[i][1]);
        g_signal_connect(item,"activate",G_CALLBACK(clicked),r);gtk_menu_shell_append(GTK_MENU_SHELL(menu),item);
    }
    gtk_widget_show_all(menu);gtk_container_add(GTK_CONTAINER(menu_button),gtk_image_new_from_icon_name("open-menu-symbolic",GTK_ICON_SIZE_BUTTON));
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(menu_button),menu);gtk_widget_set_tooltip_text(menu_button,"Reading options");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(r->header),menu_button);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(r->header),button(r,"edit-find-symbolic","Find (Ctrl+F)","search"));
    r->zoom=gtk_entry_new();gtk_entry_set_width_chars(GTK_ENTRY(r->zoom),5);gtk_entry_set_max_width_chars(GTK_ENTRY(r->zoom),7);
    gtk_widget_set_tooltip_text(r->zoom,"Zoom percentage — pinch or Ctrl+wheel to zoom");gtk_header_bar_pack_end(GTK_HEADER_BAR(r->header),r->zoom);
    GtkWidget *body=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_container_add(GTK_CONTAINER(r->window),body);
    r->searchbar=gtk_search_bar_new();GtkWidget *searchbox=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
    r->search=gtk_search_entry_new();gtk_widget_set_hexpand(r->search,TRUE);r->matches=gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(searchbox),r->search,TRUE,TRUE,0);
    GtkWidget *prev=gtk_button_new_from_icon_name("go-up-symbolic",GTK_ICON_SIZE_BUTTON), *next=gtk_button_new_from_icon_name("go-down-symbolic",GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(prev,"Previous match (Shift+Enter)");gtk_widget_set_tooltip_text(next,"Next match (Enter)");
    gtk_box_pack_start(GTK_BOX(searchbox),prev,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(searchbox),next,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(searchbox),r->matches,FALSE,FALSE,0);gtk_container_add(GTK_CONTAINER(r->searchbar),searchbox);
    gtk_search_bar_connect_entry(GTK_SEARCH_BAR(r->searchbar),GTK_ENTRY(r->search));gtk_search_bar_set_show_close_button(GTK_SEARCH_BAR(r->searchbar),TRUE);
    gtk_box_pack_start(GTK_BOX(body),r->searchbar,FALSE,FALSE,0);
    r->infobar=gtk_info_bar_new_with_buttons("Close",GTK_RESPONSE_CLOSE,NULL);r->message=gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(r->message),TRUE);gtk_label_set_selectable(GTK_LABEL(r->message),TRUE);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(r->infobar))),r->message);
    g_signal_connect_swapped(r->infobar,"response",G_CALLBACK(gtk_widget_hide),r->infobar);
    gtk_box_pack_start(GTK_BOX(body),r->infobar,FALSE,FALSE,0);
    r->stack=gtk_stack_new();gtk_box_pack_start(GTK_BOX(body),r->stack,TRUE,TRUE,0);
    GtkWidget *empty=gtk_box_new(GTK_ORIENTATION_VERTICAL,18);gtk_widget_set_valign(empty,GTK_ALIGN_CENTER);gtk_widget_set_halign(empty,GTK_ALIGN_CENTER);
    GtkWidget *label=gtk_label_new("<span size='xx-large' weight='bold'>A quiet place to read.</span>\n\nOpen a PDF, or drop one into this window.");
    gtk_label_set_use_markup(GTK_LABEL(label),TRUE);gtk_label_set_justify(GTK_LABEL(label),GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(empty),label,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(empty),button(r,"document-open-symbolic","Open PDF","open"),FALSE,FALSE,0);
    gtk_stack_add_named(GTK_STACK(r->stack),empty,"empty");
    GtkWidget *paned=gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);gtk_stack_add_named(GTK_STACK(r->stack),paned,"document");
    r->sidebar=gtk_scrolled_window_new(NULL,NULL);gtk_widget_set_size_request(r->sidebar,220,-1);
    r->outline=gtk_tree_view_new();gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(r->outline),FALSE);
    GtkCellRenderer *cell=gtk_cell_renderer_text_new();g_object_set(cell,"ellipsize",PANGO_ELLIPSIZE_END,NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(r->outline),-1,"Contents",cell,"markup",EV_DOCUMENT_LINKS_COLUMN_MARKUP,NULL);
    gtk_container_add(GTK_CONTAINER(r->sidebar),r->outline);gtk_paned_pack1(GTK_PANED(paned),r->sidebar,FALSE,FALSE);
    r->scroll=gtk_scrolled_window_new(NULL,NULL);r->view=ev_view_new();ev_view_set_model(EV_VIEW(r->view),r->model);
    ev_view_set_page_cache_size(EV_VIEW(r->view),128*1024*1024);ev_view_set_allow_links_change_zoom(EV_VIEW(r->view),FALSE);
    gtk_container_add(GTK_CONTAINER(r->scroll),r->view);gtk_paned_pack2(GTK_PANED(paned),r->scroll,TRUE,FALSE);
    r->status=gtk_label_new("");gtk_box_pack_end(GTK_BOX(body),r->status,FALSE,FALSE,0);
    g_signal_connect(r->model,"notify::page",G_CALLBACK(changed),r);g_signal_connect(r->model,"notify::scale",G_CALLBACK(changed),r);
    g_signal_connect(r->model,"notify::rotation",G_CALLBACK(changed),r);g_signal_connect(r->model,"notify::sizing-mode",G_CALLBACK(changed),r);
    g_signal_connect(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(r->scroll)),"value-changed",G_CALLBACK(scrolled),r);
    g_signal_connect(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(r->scroll)),"value-changed",G_CALLBACK(scrolled),r);
    g_signal_connect(r->page,"activate",G_CALLBACK(page_activate),r);g_signal_connect(r->zoom,"activate",G_CALLBACK(zoom_activate),r);
    g_signal_connect(r->search,"search-changed",G_CALLBACK(search_changed),r);
    g_signal_connect(prev,"clicked",G_CALLBACK(previous_match),r);g_signal_connect(next,"clicked",G_CALLBACK(next_match),r);
    g_signal_connect(r->outline,"row-activated",G_CALLBACK(outline_activate),r);
    g_signal_connect(r->view,"handle-link",G_CALLBACK(link_handled),r);g_signal_connect(r->view,"external-link",G_CALLBACK(external_link),r);
    g_signal_connect(r->window,"key-press-event",G_CALLBACK(key_press),r);g_signal_connect(r->window,"destroy",G_CALLBACK(destroyed),r);
    gtk_drag_dest_set(r->window,GTK_DEST_DEFAULT_ALL,NULL,0,GDK_ACTION_COPY);gtk_drag_dest_add_uri_targets(r->window);
    g_signal_connect(r->window,"drag-data-received",G_CALLBACK(drop_received),r);
    gtk_widget_show_all(r->window);gtk_widget_hide(r->sidebar);gtk_widget_hide(r->infobar);gtk_widget_hide(r->status);
    gtk_stack_set_visible_child_name(GTK_STACK(r->stack),"empty");update_ui(r);
    return r;
}
static void activate(GtkApplication *app,gpointer data)
{
    (void)app;(void)data;new_reader();
}
static void files_open(GtkApplication *app,GFile **files,int count,const char *hint,gpointer data)
{
    (void)app;(void)hint;(void)data;
    for(int i=0;i<count;i++) { Reader *r=new_reader();g_autofree char *uri=g_file_get_uri(files[i]);open_uri(r,uri,FALSE); }
}
int main(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--version")) { g_print("SimplePDF %s\n",VERSION);return 0; }
    if (argc==2 && (!strcmp(argv[1],"--help") || !strcmp(argv[1],"-h"))) {
        g_print("Usage: simplepdf [--] [FILE.pdf …]\nA GTK PDF reader. Pinch to zoom; Ctrl+F finds text.\nUse simplebook for terminal PDF/EPUB/Kindle reading.\n");return 0;
    }
    if (!ev_init()) { g_printerr("SimplePDF: PDF backends unavailable. Run install-simplepdf.sh.\n");return 1; }
    application=gtk_application_new("org.simplesuite.Simplepdf",G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(application,"activate",G_CALLBACK(activate),NULL);g_signal_connect(application,"open",G_CALLBACK(files_open),NULL);
    int result=g_application_run(G_APPLICATION(application),argc,argv);
    g_object_unref(application);ev_shutdown();return result;
}
