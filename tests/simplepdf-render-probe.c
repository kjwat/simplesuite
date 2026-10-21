/* Optional Linux LD_PRELOAD diagnostic for both SimplePDF and Evince.
 * GPL-3.0-or-later. Captures unmodified backend output and final view geometry;
 * it never changes font options, model settings, or drawing operations.
 * Captured PNGs contain document content. Use a private output directory.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <gtk/gtk.h>
#include <evince-document.h>
#include <evince-view.h>
#include <cairo-ft.h>
#include <stdio.h>

typedef struct {
    GKeyFile *record;
    GHashTable *font_options;
    GHashTable *load_flags;
    GHashTable *glyph_load_flags;
    GHashTable *glyph_render_modes;
    guint glyph_calls;
} RenderTrace;
static _Thread_local RenderTrace *active_render;
static cairo_user_data_key_t page_tag;

static const char *output_directory(void)
{
    const char *directory=g_getenv("SIMPLEPDF_RENDER_PROBE_DIR");
    return directory && g_path_is_absolute(directory) ? directory : NULL;
}
static gpointer resolve(const char *name)
{
    gpointer symbol=dlsym(RTLD_NEXT,name);
    if (!symbol) g_error("Render probe: cannot resolve %s: %s",name,dlerror());
    return symbol;
}
static void write_record(const char *name,GKeyFile *record)
{
    const char *directory=output_directory();
    if (!directory) return;
    g_autoptr(GError) error=NULL;
    if (g_mkdir_with_parents(directory,0700)!=0) {
        g_printerr("Render probe: cannot create output directory\n");return;
    }
    g_autofree char *path=g_build_filename(directory,name,NULL);
    gsize size;
    g_autofree char *text=g_key_file_to_data(record,&size,NULL);
    if (!g_file_set_contents_full(path,text,size,G_FILE_SET_CONTENTS_CONSISTENT,0600,&error))
        g_printerr("Render probe: %s\n",error->message);
}
static void record_matrix(GKeyFile *record,const char *group,const char *key,const cairo_matrix_t *m)
{
    double values[]={m->xx,m->yx,m->xy,m->yy,m->x0,m->y0};
    g_key_file_set_double_list(record,group,key,values,G_N_ELEMENTS(values));
}
static char *font_options_string(cairo_font_options_t *options)
{
    return g_strdup_printf("antialias=%d subpixel=%d hint-style=%d hint-metrics=%d variations=%s",
        cairo_font_options_get_antialias(options),cairo_font_options_get_subpixel_order(options),
        cairo_font_options_get_hint_style(options),cairo_font_options_get_hint_metrics(options),
        cairo_font_options_get_variations(options)?cairo_font_options_get_variations(options):"");
}
static void record_library(GKeyFile *record,const char *symbol,gpointer function)
{
    Dl_info info;
    if(dladdr(function,&info) && info.dli_fname)
        g_key_file_set_string(record,"libraries",symbol,info.dli_fname);
}
/* Observe the actual PDF Cairo context, before Poppler touches it. No GTK
   font settings are substituted: that would change the pipeline under test. */
void poppler_page_render(void *page,cairo_t *cr)
{
    static gsize symbol;
    if(g_once_init_enter(&symbol)) g_once_init_leave(&symbol,(gsize)resolve("poppler_page_render"));
    void (*render)(void *,cairo_t *)=(gpointer)symbol;
    if(active_render) {
        GKeyFile *record=active_render->record;
        cairo_surface_t *surface=cairo_get_target(cr);
        cairo_matrix_t matrix;cairo_get_matrix(cr,&matrix);
        record_matrix(record,"poppler","ctm",&matrix);
        g_key_file_set_integer(record,"poppler","surface-type",cairo_surface_get_type(surface));
        g_key_file_set_integer(record,"poppler","antialias",cairo_get_antialias(cr));
        g_key_file_set_string(record,"poppler","entry-point","poppler_page_render (screen)");
        cairo_font_options_t *options=cairo_font_options_create();
        cairo_get_font_options(cr,options);
        g_autofree char *context=font_options_string(options);
        g_key_file_set_string(record,"poppler","context-font-options",context);
        cairo_surface_get_font_options(surface,options);
        g_autofree char *target=font_options_string(options);
        g_key_file_set_string(record,"poppler","surface-font-options",target);
        cairo_font_options_destroy(options);
        record_library(record,"poppler_page_render",(gpointer)symbol);
    }
    render(page,cr);
}
cairo_font_face_t *cairo_ft_font_face_create_for_ft_face(FT_Face face,int flags)
{
    static gsize symbol;
    if(g_once_init_enter(&symbol))
        g_once_init_leave(&symbol,(gsize)resolve("cairo_ft_font_face_create_for_ft_face"));
    cairo_font_face_t *(*create)(FT_Face,int)=(gpointer)symbol;
    if(active_render) {
        g_hash_table_add(active_render->load_flags,g_strdup_printf("0x%x",flags));
        record_library(active_render->record,"cairo_ft_font_face_create_for_ft_face",(gpointer)symbol);
    }
    return create(face,flags);
}
FT_Error FT_Load_Glyph(FT_Face face,FT_UInt index,FT_Int32 flags)
{
    static gsize symbol;
    if(g_once_init_enter(&symbol)) g_once_init_leave(&symbol,(gsize)resolve("FT_Load_Glyph"));
    FT_Error (*load)(FT_Face,FT_UInt,FT_Int32)=(gpointer)symbol;
    if(active_render) {
        g_hash_table_add(active_render->glyph_load_flags,g_strdup_printf("0x%lx",(unsigned long)flags));
        record_library(active_render->record,"FT_Load_Glyph",(gpointer)symbol);
    }
    return load(face,index,flags);
}
FT_Error FT_Render_Glyph(FT_GlyphSlot slot,FT_Render_Mode mode)
{
    static gsize symbol;
    if(g_once_init_enter(&symbol)) g_once_init_leave(&symbol,(gsize)resolve("FT_Render_Glyph"));
    FT_Error (*render)(FT_GlyphSlot,FT_Render_Mode)=(gpointer)symbol;
    if(active_render) g_hash_table_add(active_render->glyph_render_modes,g_strdup_printf("%d",mode));
    return render(slot,mode);
}
static void record_glyph_options(cairo_t *cr)
{
    if(!active_render) return;
    cairo_font_options_t *options=cairo_font_options_create();
    cairo_scaled_font_get_font_options(cairo_get_scaled_font(cr),options);
    g_hash_table_add(active_render->font_options,font_options_string(options));
    cairo_font_options_destroy(options);
    active_render->glyph_calls++;
}
void cairo_show_glyphs(cairo_t *cr,const cairo_glyph_t *glyphs,int count)
{
    static gsize symbol;
    if(g_once_init_enter(&symbol)) g_once_init_leave(&symbol,(gsize)resolve("cairo_show_glyphs"));
    void (*show)(cairo_t *,const cairo_glyph_t *,int)=(gpointer)symbol;
    record_glyph_options(cr);show(cr,glyphs,count);
}
void cairo_show_text_glyphs(cairo_t *cr,const char *text,int length,const cairo_glyph_t *glyphs,
    int count,const cairo_text_cluster_t *clusters,int cluster_count,cairo_text_cluster_flags_t flags)
{
    static gsize symbol;
    if(g_once_init_enter(&symbol)) g_once_init_leave(&symbol,(gsize)resolve("cairo_show_text_glyphs"));
    void (*show)(cairo_t *,const char *,int,const cairo_glyph_t *,int,
        const cairo_text_cluster_t *,int,cairo_text_cluster_flags_t)=(gpointer)symbol;
    record_glyph_options(cr);show(cr,text,length,glyphs,count,clusters,cluster_count,flags);
}
static void record_set(GKeyFile *record,const char *key,GHashTable *set)
{
    GList *keys=g_hash_table_get_keys(set);
    keys=g_list_sort(keys,(GCompareFunc)g_strcmp0);
    GString *joined=g_string_new(NULL);
    for(GList *item=keys;item;item=item->next) {
        if(joined->len) g_string_append_c(joined,'\n');
        g_string_append(joined,item->data);
    }
    g_key_file_set_string(record,"fonts",key,joined->str);
    g_string_free(joined,TRUE);g_list_free(keys);
}
cairo_surface_t *ev_document_render(EvDocument *document,EvRenderContext *context)
{
    static gsize symbol;
    if (g_once_init_enter(&symbol))
        g_once_init_leave(&symbol,(gsize)resolve("ev_document_render"));
    cairo_surface_t *(*render)(EvDocument *,EvRenderContext *)=(gpointer)symbol;
    const char *directory=output_directory();
    if(!directory) return render(document,context);
    g_autoptr(GKeyFile) record=g_key_file_new();
    RenderTrace trace={record,g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL),
        g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL),
        g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL),
        g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL),0};
    RenderTrace *previous=active_render;active_render=&trace;
    cairo_surface_t *surface=render(document,context);
    active_render=previous;
    record_set(record,"scaled-font-options",trace.font_options);
    record_set(record,"new-ft-face-load-flags",trace.load_flags);
    record_set(record,"ft-glyph-load-flags",trace.glyph_load_flags);
    record_set(record,"ft-glyph-render-modes",trace.glyph_render_modes);
    g_key_file_set_integer(record,"fonts","glyph-calls",trace.glyph_calls);
    g_hash_table_unref(trace.font_options);g_hash_table_unref(trace.load_flags);
    g_hash_table_unref(trace.glyph_load_flags);g_hash_table_unref(trace.glyph_render_modes);
    record_library(record,"ev_document_render",(gpointer)symbol);
    if (!surface || cairo_surface_status(surface)!=CAIRO_STATUS_SUCCESS ||
        cairo_surface_get_type(surface)!=CAIRO_SURFACE_TYPE_IMAGE) return surface;
    /* The caller has not yet exposed the surface to the view/cache. */
    cairo_surface_flush(surface);
    int width=cairo_image_surface_get_width(surface),height=cairo_image_surface_get_height(surface);
    int stride=cairo_image_surface_get_stride(surface);
    g_autofree char *hash=g_compute_checksum_for_data(G_CHECKSUM_SHA256,
        cairo_image_surface_get_data(surface),(gsize)stride*height);
    g_autofree char *base=g_strdup_printf("render-p%d-r%d-%dx%d",context->page->index,
        context->rotation,width,height);
    g_autofree char *name=g_strconcat(base,".ini",NULL);
    EvDocumentBackendInfo backend={0};
    gboolean have_backend=ev_document_get_backend_info(document,&backend);
    g_key_file_set_integer(record,"render","page",context->page->index);
    g_key_file_set_integer(record,"render","rotation",context->rotation);
    g_key_file_set_integer(record,"render","width",width);
    g_key_file_set_integer(record,"render","height",height);
    g_key_file_set_integer(record,"render","stride",stride);
    g_key_file_set_integer(record,"render","format",cairo_image_surface_get_format(surface));
    g_key_file_set_double(record,"render","scale",context->scale);
    g_key_file_set_integer(record,"render","target_width",context->target_width);
    g_key_file_set_integer(record,"render","target_height",context->target_height);
    g_key_file_set_string(record,"render","sha256",hash);
    g_key_file_set_string(record,"render","backend",have_backend?backend.name:"unknown");
    g_key_file_set_string(record,"render","backend_version",have_backend?backend.version:"unknown");
    g_key_file_set_string(record,"render","cairo_version",cairo_version_string());
    char *tag=g_strdup(base);
    if(cairo_surface_set_user_data(surface,&page_tag,tag,g_free)!=CAIRO_STATUS_SUCCESS) g_free(tag);
    write_record(name,record);
    g_autofree char *png_name=g_strconcat(base,".png",NULL);
    g_autofree char *path=g_build_filename(directory,png_name,NULL);
    cairo_status_t status=cairo_surface_write_to_png(surface,path);
    if (status!=CAIRO_STATUS_SUCCESS)
        g_printerr("Render probe: %s\n",cairo_status_to_string(status));
    return surface;
}
/* The tagged surface is the actual cache surface EvView paints. Record its
   final sampling/transform without reading a screenshot or altering drawing. */
void cairo_paint(cairo_t *cr)
{
    static gsize symbol;
    if(g_once_init_enter(&symbol)) g_once_init_leave(&symbol,(gsize)resolve("cairo_paint"));
    void (*paint)(cairo_t *)=(gpointer)symbol;
    cairo_pattern_t *pattern=cairo_get_source(cr);
    cairo_surface_t *source=NULL;
    if(output_directory() && cairo_pattern_get_surface(pattern,&source)==CAIRO_STATUS_SUCCESS) {
        const char *tag=cairo_surface_get_user_data(source,&page_tag);
        if(tag) {
            g_autoptr(GKeyFile) record=g_key_file_new();
            g_key_file_set_string(record,"paint","source",tag);
            g_key_file_set_integer(record,"paint","filter",cairo_pattern_get_filter(pattern));
            g_key_file_set_integer(record,"paint","operator",cairo_get_operator(cr));
            cairo_matrix_t matrix;cairo_get_matrix(cr,&matrix);
            record_matrix(record,"paint","ctm",&matrix);
            cairo_pattern_get_matrix(pattern,&matrix);
            record_matrix(record,"paint","pattern-matrix",&matrix);
            double x,y;cairo_surface_get_device_scale(source,&x,&y);
            double scales[]={x,y};
            g_key_file_set_double_list(record,"paint","source-device-scale",scales,2);
            cairo_surface_get_device_scale(cairo_get_target(cr),&x,&y);
            scales[0]=x;scales[1]=y;
            g_key_file_set_double_list(record,"paint","target-device-scale",scales,2);
            g_autofree char *name=g_strdup_printf("paint-%s.ini",tag);
            write_record(name,record);
        }
    }
    paint(cr);
}
static gboolean view_drawn(GtkWidget *widget,cairo_t *cr,EvDocumentModel *model)
{
    if (!output_directory() || !ev_document_model_get_document(model)) return FALSE;
    GdkRectangle area;GtkBorder border;
    int page=ev_document_model_get_page(model);
    if (!ev_view_get_page_extents(EV_VIEW(widget),page,&area,&border)) return FALSE;
    g_autoptr(GKeyFile) record=g_key_file_new();
    g_key_file_set_integer(record,"view","page",page);
    g_key_file_set_integer(record,"view","rotation",ev_document_model_get_rotation(model));
    g_key_file_set_double(record,"view","scale",ev_document_model_get_scale(model));
    g_key_file_set_integer(record,"view","sizing",ev_document_model_get_sizing_mode(model));
    g_key_file_set_integer(record,"view","device_scale",gtk_widget_get_scale_factor(widget));
    g_key_file_set_boolean(record,"view","loading",ev_view_is_loading(EV_VIEW(widget)));
    g_key_file_set_double(record,"view","ui_font_dpi",gdk_screen_get_resolution(gtk_widget_get_screen(widget)));
    g_key_file_set_double(record,"view","pdf_dpi",ev_document_misc_get_widget_dpi(widget));
    g_key_file_set_string(record,"view","display_backend",G_OBJECT_TYPE_NAME(gtk_widget_get_display(widget)));
    g_key_file_set_integer(record,"view","page_width",area.width-border.left-border.right);
    g_key_file_set_integer(record,"view","page_height",area.height-border.top-border.bottom);
    g_key_file_set_integer(record,"view","page_x",area.x+border.left);
    g_key_file_set_integer(record,"view","page_y",area.y+border.top);
    g_key_file_set_integer(record,"view","view_width",gtk_widget_get_allocated_width(widget));
    g_key_file_set_integer(record,"view","view_height",gtk_widget_get_allocated_height(widget));
    GtkAdjustment *h=gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(widget));
    GtkAdjustment *v=gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(widget));
    g_key_file_set_double(record,"view","scroll_x",gtk_adjustment_get_value(h));
    g_key_file_set_double(record,"view","scroll_y",gtk_adjustment_get_value(v));
    GtkWidget *window=gtk_widget_get_toplevel(widget);
    int x=0,y=0;
    gtk_widget_translate_coordinates(widget,window,0,0,&x,&y);
    g_key_file_set_integer(record,"view","widget_x",x);
    g_key_file_set_integer(record,"view","widget_y",y);
    gdk_window_get_origin(gtk_widget_get_window(window),&x,&y);
    g_key_file_set_integer(record,"view","window_x",x);
    g_key_file_set_integer(record,"view","window_y",y);
    g_key_file_set_integer(record,"view","border_left",border.left);
    g_key_file_set_integer(record,"view","border_right",border.right);
    g_key_file_set_integer(record,"view","border_top",border.top);
    g_key_file_set_integer(record,"view","border_bottom",border.bottom);
    cairo_matrix_t matrix;cairo_get_matrix(cr,&matrix);
    double values[]={matrix.xx,matrix.yx,matrix.xy,matrix.yy,matrix.x0,matrix.y0};
    g_key_file_set_double_list(record,"view","cairo_matrix",values,G_N_ELEMENTS(values));
    const char *variables[]={"GDK_BACKEND","GDK_SCALE","GDK_DPI_SCALE","FREETYPE_PROPERTIES",
        "FONTCONFIG_FILE","FONTCONFIG_PATH","PANGOCAIRO_BACKEND"};
    for (guint i=0;i<G_N_ELEMENTS(variables);i++) {
        const char *value=g_getenv(variables[i]);
        if (value) g_key_file_set_string(record,"environment",variables[i],value);
    }
    write_record("view.ini",record);
    /* Append geometry and damage history; a final snapshot alone can hide
       scroll-triggered scale changes or repeated allocations. */
    g_autofree char *trace_path=g_build_filename(output_directory(),"draws.tsv",NULL);
    FILE *trace=fopen(trace_path,"a");
    if(trace) {
        GdkRectangle clip={0};gdk_cairo_get_clip_rectangle(cr,&clip);
        GdkFrameClock *clock=gtk_widget_get_frame_clock(widget);
        fprintf(trace,"%"G_GINT64_FORMAT"\t%"G_GINT64_FORMAT"\t%.17g\t%.17g\t%.17g\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n",
            g_get_monotonic_time(),clock?gdk_frame_clock_get_frame_counter(clock):-1,
            ev_document_model_get_scale(model),gtk_adjustment_get_value(h),gtk_adjustment_get_value(v),
            area.width-border.left-border.right,area.height-border.top-border.bottom,
            gtk_widget_get_allocated_width(widget),gtk_widget_get_allocated_height(widget),
            clip.x,clip.y,clip.width,clip.height);
        fclose(trace);
    }
    return FALSE;
}
void ev_view_set_model(EvView *view,EvDocumentModel *model)
{
    static gsize symbol;
    if (g_once_init_enter(&symbol))
        g_once_init_leave(&symbol,(gsize)resolve("ev_view_set_model"));
    void (*set_model)(EvView *,EvDocumentModel *)=(gpointer)symbol;
    set_model(view,model);
    if (!output_directory()) return;
    gulong old=GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(view),"render-probe-handler"));
    if (old && g_signal_handler_is_connected(view,old)) g_signal_handler_disconnect(view,old);
    gulong handler=model?g_signal_connect_object(view,"draw",G_CALLBACK(view_drawn),model,G_CONNECT_AFTER):0;
    g_object_set_data(G_OBJECT(view),"render-probe-handler",GSIZE_TO_POINTER(handler));
}
