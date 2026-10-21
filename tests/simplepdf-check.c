#define main simplepdf_program_main
#include "../simplepdf.c"
#undef main
#include <cairo-pdf.h>
#include <glib/gstdio.h>

static void pump(int ms)
{
    gint64 end=g_get_monotonic_time()+ms*1000;
    do { while(g_main_context_iteration(NULL,FALSE)); g_usleep(1000); } while(g_get_monotonic_time()<end);
}
static void pinch(Reader *r, GdkTouchpadGesturePhase phase, double scale)
{
    GdkEvent *event=gdk_event_new(GDK_TOUCHPAD_PINCH);
    event->touchpad_pinch.window=g_object_ref(gtk_widget_get_window(r->view));
    event->touchpad_pinch.send_event=TRUE;
    event->touchpad_pinch.phase=phase;event->touchpad_pinch.n_fingers=2;
    event->touchpad_pinch.time=GDK_CURRENT_TIME;
    event->touchpad_pinch.x=240;event->touchpad_pinch.y=240;
    event->touchpad_pinch.scale=scale;
    GdkDevice *pointer=gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
    gdk_event_set_device(event,pointer);gdk_event_set_source_device(event,pointer);
    gtk_main_do_event(event);gdk_event_free(event);pump(30);
}
static void loaded(Reader *r)
{
    for(int i=0;i<500 && r->load;i++) pump(10);
    g_assert_null(r->load); g_assert_nonnull(document(r)); pump(200);
}
static void fixture(const char *path)
{
    cairo_surface_t *surface=cairo_pdf_surface_create(path,432,648);
    cairo_t *cr=cairo_create(surface);
    cairo_pdf_surface_add_outline(surface,CAIRO_PDF_OUTLINE_ROOT,"Second chapter","page=2",CAIRO_PDF_OUTLINE_FLAG_OPEN);
    for(int p=0;p<3;p++) {
        cairo_pdf_surface_set_page_label(surface,p==0?"iv":p==1?"1":"2");
        cairo_set_source_rgb(cr,0,0,0);cairo_select_font_face(cr,"serif",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr,12);
        for(int i=0;i<28;i++) { cairo_move_to(cr,36,50+i*18);cairo_show_text(cr,"A quiet reader: fine type, precise pages, readable text."); }
        cairo_set_line_width(cr,.3);cairo_move_to(cr,36,585);cairo_line_to(cr,396,585);cairo_stroke(cr);
        cairo_show_page(cr);
    }
    cairo_destroy(cr);cairo_surface_destroy(surface);
}
int main(int argc,char **argv)
{
    g_test_init(&argc,&argv,NULL);
    g_autoptr(GError) error=NULL;
    g_autofree char *directory=g_dir_make_tmp("simplepdf-check-XXXXXX",&error);g_assert_no_error(error);
    g_setenv("XDG_STATE_HOME",directory,TRUE);
    g_autofree char *path=g_build_filename(directory,"reader's — sample.pdf",NULL);
    fixture(path);
    gtk_init(&argc,&argv);g_assert_true(ev_init());
    application=gtk_application_new("org.simplesuite.Simplepdf.Test",G_APPLICATION_NON_UNIQUE|G_APPLICATION_HANDLES_OPEN);
    g_assert_true(g_application_register(G_APPLICATION(application),NULL,&error));g_assert_no_error(error);
    Reader *r=new_reader();
    g_autofree char *uri=g_filename_to_uri(path,NULL,NULL);
    open_uri(r,uri,FALSE); loaded(r);
    g_assert_cmpint(ev_document_get_n_pages(document(r)),==,3);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(r->page)),==,"iv");
    /* Genuine engine renders/caches a page and supplies the selectable text. */
    EvPage *page=ev_document_get_page(document(r),0);
    char *text=ev_document_text_get_text(EV_DOCUMENT_TEXT(document(r)),page);
    g_assert_nonnull(strstr(text,"quiet reader"));g_free(text);g_object_unref(page);
    g_assert_nonnull(gtk_tree_view_get_model(GTK_TREE_VIEW(r->outline)));
    ev_view_select_all(EV_VIEW(r->view));pump(80);
    g_autofree char *selection=ev_view_get_selected_text(EV_VIEW(r->view));
    g_assert_nonnull(selection);g_assert_nonnull(strstr(selection,"quiet reader"));
    action(r,"copy");
    g_autofree char *copied=gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
    g_assert_nonnull(copied);g_assert_nonnull(strstr(copied,"precise pages"));
    const double percentages[]={.75,1,1.25,1.5,2};
    for (guint i=0;i<G_N_ELEMENTS(percentages);i++) {
        ev_document_model_set_sizing_mode(r->model,EV_SIZING_FREE);
        ev_document_model_set_scale(r->model,normal_scale(r)*percentages[i]);pump(80);
        GdkRectangle area;GtkBorder border;
        g_assert_true(ev_view_get_page_extents(EV_VIEW(r->view),0,&area,&border));
        g_assert_cmpfloat_with_epsilon(area.width-border.left-border.right,432*normal_scale(r)*percentages[i],2);
    }
    action(r,"rotate-right");pump(150);
    g_assert_cmpint(ev_document_model_get_rotation(r->model),==,90);
    action(r,"rotate-left");pump(150);
    gtk_entry_set_text(GTK_ENTRY(r->search),"precise");start_search(r);
    for(int i=0;i<300 && r->find && !r->find->finished;i++) pump(10);
    g_assert_nonnull(r->find);g_assert_true(ev_job_find_has_results(EV_JOB_FIND(r->find)));
    gtk_entry_set_text(GTK_ENTRY(r->search),"");start_search(r);
    ev_document_model_set_page(r->model,0);pump(100);
    PdfPosition start=capture(r); remember(r);ev_document_model_set_page(r->model,2);pump(100);
    history(r,FALSE);pump(160);g_assert_cmpint(ev_document_model_get_page(r->model),==,start.page);
    action(r,"actual-size");pump(160);g_assert_cmpfloat_with_epsilon(ev_document_model_get_scale(r->model),normal_scale(r),.001);
    gtk_entry_set_text(GTK_ENTRY(r->page),"2");page_activate(GTK_ENTRY(r->page),r);pump(100);
    g_assert_cmpint(ev_document_model_get_page(r->model),==,2);
    save_state(r);PdfPosition saved;g_assert_true(pdf_state_load(uri,&saved));g_assert_cmpint(saved.page,==,2);
    saved.scale=NAN;g_assert_false(pdf_state_save(uri,&saved,NULL));
    double before=ev_document_model_get_scale(r->model);
    pinch(r,GDK_TOUCHPAD_GESTURE_PHASE_BEGIN,1);
    pinch(r,GDK_TOUCHPAD_GESTURE_PHASE_UPDATE,1.25);
    pinch(r,GDK_TOUCHPAD_GESTURE_PHASE_END,1.25);pump(150);
    g_assert_cmpfloat(ev_document_model_get_scale(r->model),>,before*1.1);
    g_assert_cmpint(ev_document_model_get_sizing_mode(r->model),==,EV_SIZING_FREE);
    const char *output=g_getenv("SIMPLEPDF_TEST_OUTPUT");
    if (output) {
        action(r,"fit-width");ev_document_model_set_page(r->model,0);pump(250);
        GdkPixbuf *shot=gdk_pixbuf_get_from_window(gtk_widget_get_window(r->window),0,0,
            gtk_widget_get_allocated_width(r->window),gtk_widget_get_allocated_height(r->window));
        g_assert_nonnull(shot);g_assert_true(gdk_pixbuf_save(shot,output,"png",&error,NULL));
        g_assert_no_error(error);g_object_unref(shot);
    }
    /* File watching reloads changed content without abandoning position. */
    ev_document_model_set_page(r->model,2);pump(120);
    guint generation=r->generation;
    fixture(path);pump(800);loaded(r);
    g_assert_cmpuint(r->generation,>,generation);
    g_assert_cmpint(ev_document_model_get_page(r->model),==,2);
    /* Exercise the real encrypted-document/password retry path when Ghostscript
       is available; it is a fixture generator, never a runtime dependency. */
    g_autofree char *gs=g_find_program_in_path("gs");
    if (gs) {
        g_autofree char *encrypted=g_build_filename(directory,"encrypted.pdf",NULL);
        g_autofree char *output_arg=g_strconcat("-sOutputFile=",encrypted,NULL);
        char *args[]={gs,"-q","-sDEVICE=pdfwrite","-dBATCH","-dNOPAUSE","-dEncryptionR=3","-dKeyLength=128",
            "-sOwnerPassword=owner","-sUserPassword=secret",output_arg,path,NULL};
        int status;g_autofree char *diagnostic=NULL;
        g_assert_true(g_spawn_sync(NULL,args,NULL,0,NULL,NULL,NULL,&diagnostic,&status,&error));
        g_assert_no_error(error);g_assert_true(g_spawn_check_wait_status(status,&error));g_assert_no_error(error);
        g_autofree char *encrypted_uri=g_filename_to_uri(encrypted,NULL,NULL);
        open_uri(r,encrypted_uri,FALSE);
        for(int attempt=0;attempt<2;attempt++) {
            for(int i=0;i<400 && r->load && !r->load->finished;i++)pump(10);
            pump(40);
            GList *windows=gtk_window_list_toplevels();GtkDialog *password=NULL;
            for(GList *item=windows;item;item=item->next)
                if(gtk_widget_get_visible(GTK_WIDGET(item->data)) && g_object_get_data(G_OBJECT(item->data),"password"))password=GTK_DIALOG(item->data);
            g_list_free(windows);g_assert_nonnull(password);
            GtkEntry *entry=g_object_get_data(G_OBJECT(password),"password");
            gtk_entry_set_text(entry,attempt==0?"wrong":"secret");
            gtk_dialog_response(password,GTK_RESPONSE_ACCEPT);pump(40);
        }
        loaded(r);g_autofree char *opened_uri=g_file_get_uri(r->file);g_assert_cmpstr(opened_uri,==,encrypted_uri);
        open_uri(r,uri,FALSE);loaded(r);
    }
    /* Failed opens and superseded loads preserve the current document. */
    EvDocument *old=document(r);
    g_autofree char *bad=g_build_filename(directory,"broken.pdf",NULL);g_file_set_contents(bad,"%PDF-1.7\nbroken",-1,NULL);
    g_autofree char *bad_uri=g_filename_to_uri(bad,NULL,NULL);
    open_uri(r,bad_uri,FALSE);for(int i=0;i<300 && r->load;i++) pump(10);
    g_assert_true(old==document(r));g_assert_true(gtk_widget_get_visible(r->infobar));
    open_uri(r,uri,FALSE);open_uri(r,uri,FALSE);loaded(r);
    open_uri(r,uri,FALSE);gtk_widget_destroy(r->window);pump(100);
    /* A pending load must never invoke a freed Reader. */
    g_object_unref(application);ev_shutdown();
    g_print("OK real PDF load/render/text/copy/outline/search, 75–200%% geometry, rotation/reload/password retry, labels, history, zoom, state, failed/replaced loads and close cancellation\n");
    return 0;
}
