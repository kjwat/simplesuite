/* Dependency regression: repainting a settled page through different damage
 * rectangles must not change its pixels. Exercises the real EvView draw path.
 * GPL-3.0-or-later. See SIMPLEPDF-RENDERING.md. */
#define main simplepdf_program_main
#include "../simplepdf.c"
#undef main
#include <cairo-pdf.h>
#include <glib/gstdio.h>

static void pump(int ms)
{
    gint64 end=g_get_monotonic_time()+ms*1000;
    do { while(g_main_context_iteration(NULL,FALSE));g_usleep(1000); } while(g_get_monotonic_time()<end);
}
static cairo_surface_t *draw(GtkWidget *view,cairo_surface_t *previous,int top,int height)
{
    int sf=gtk_widget_get_scale_factor(view);
    cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
        gtk_widget_get_allocated_width(view)*sf,gtk_widget_get_allocated_height(view)*sf);
    cairo_surface_set_device_scale(surface,sf,sf);
    cairo_t *cr=cairo_create(surface);
    if(previous) { cairo_set_source_surface(cr,previous,0,0);cairo_paint(cr); }
    cairo_rectangle(cr,0,top,gtk_widget_get_allocated_width(view),height);cairo_clip(cr);
    gtk_widget_draw(view,cr);cairo_destroy(cr);cairo_surface_flush(surface);
    return surface;
}
int main(int argc,char **argv)
{
    g_autofree char *directory=g_dir_make_tmp("simplepdf-damage-XXXXXX",NULL);
    g_assert_nonnull(directory);
    g_setenv("XDG_STATE_HOME",directory,TRUE);g_setenv("XDG_CONFIG_HOME",directory,TRUE);
    g_setenv("GIO_USE_VFS","local",TRUE);g_setenv("GSETTINGS_BACKEND","memory",TRUE);
    g_autofree char *path=g_build_filename(directory,"sample.pdf",NULL);
    cairo_surface_t *pdf=cairo_pdf_surface_create(path,432,648);cairo_t *cr=cairo_create(pdf);
    cairo_pdf_surface_add_outline(pdf,CAIRO_PDF_OUTLINE_ROOT,"Page","page=1",0);
    cairo_select_font_face(cr,"serif",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr,12);
    for(int i=0;i<33;i++) { cairo_move_to(cr,25,30+i*18);cairo_show_text(cr,"Fine type: a stationary page must survive partial redraws."); }
    cairo_destroy(cr);cairo_surface_destroy(pdf);
    gtk_init(&argc,&argv);g_assert_true(ev_init());
    application=gtk_application_new("org.simplesuite.Simplepdf.DamageCheck",G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application),NULL,NULL));
    Reader *r=new_reader();g_autofree char *uri=g_filename_to_uri(path,NULL,NULL);
    open_uri(r,uri,FALSE);
    for(int i=0;i<500 && r->load;i++) pump(10);
    g_assert_null(r->load);g_assert_nonnull(document(r));pump(200);
    int failures=0;
    const int widths[]={864,902,903,905};
    const int tops[]={127,250,384,512};
    for(guint w=0;w<G_N_ELEMENTS(widths);w++) {
        ev_document_model_set_sizing_mode(r->model,EV_SIZING_FREE);
        ev_document_model_set_scale(r->model,widths[w]/432.0);pump(200);
        ev_job_scheduler_wait();pump(100);
        GdkRectangle area;GtkBorder border;
        g_assert_true(ev_view_get_page_extents(EV_VIEW(r->view),0,&area,&border));
        cairo_surface_t *full=draw(r->view,NULL,0,gtk_widget_get_allocated_height(r->view));
        int stride=cairo_image_surface_get_stride(full),height=cairo_image_surface_get_height(full);
        int width=cairo_image_surface_get_width(full);
        for(guint t=0;t<G_N_ELEMENTS(tops);t++) {
            cairo_surface_t *partial=draw(r->view,full,tops[t],73);
            const unsigned char *a=cairo_image_surface_get_data(full),*b=cairo_image_surface_get_data(partial);
            int changed=0,max_delta=0;
            for(int y=0;y<height;y++) for(int x=0;x<width;x++) {
                int different=0;
                for(int c=0;c<4;c++) { int delta=abs(a[y*stride+x*4+c]-b[y*stride+x*4+c]);
                    if(delta) different=1;
                    max_delta=MAX(max_delta,delta); }
                changed+=different;
            }
            g_print("page=%dx%d device=%d clip=0,%d,%d,73 changed_pixels=%d max_channel_delta=%d\n",
                area.width-border.left-border.right,area.height-border.top-border.bottom,
                gtk_widget_get_scale_factor(r->view),tops[t],gtk_widget_get_allocated_width(r->view),changed,max_delta);
            if(changed) failures++;
            cairo_surface_destroy(partial);
        }
        cairo_surface_destroy(full);
    }
    gtk_widget_destroy(r->window);pump(100);ev_job_scheduler_wait();
    g_object_unref(application);ev_shutdown();g_remove(path);
    /* State files are confined to the temporary directory. */
    return failures?1:0;
}
