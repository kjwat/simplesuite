#define _GNU_SOURCE
#include <ctype.h>
#include <curses.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define MAX_VOLUME 130
#define READ_CHUNK 8192
#define ACTION_ALT_LEFT  1000001
#define ACTION_ALT_RIGHT 1000002

static char mpv_socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)] = "";
static char mpv_socket_tmpdir[PATH_MAX] = "";

typedef enum { EK_UP, EK_DIR, EK_CUE, EK_PLAYLISTFILE, EK_FILE, EK_CUETRACK, EK_PLTRACK } EntryKind;

typedef struct {
    int number;
    char *title;
    char *performer;
    char *audio;
    double start;
    double end;
    bool has_end;
} CueTrack;

typedef struct {
    int number;
    char *title;
    char *source;
} ListTrack;

typedef struct {
    EntryKind kind;
    char *path;
    char *label;
    CueTrack *cue;
    ListTrack *pl;
} Entry;

typedef struct { Entry *v; size_t n, cap; } Entries;
typedef struct { CueTrack *v; size_t n, cap; } CueTracks;
typedef struct { ListTrack *v; size_t n, cap; } ListTracks;
typedef struct { char **v; size_t n, cap; } StrList;

static pid_t current_player = -1;
static int current_volume = 100;
static bool paused = false;
static bool continuous = true;
static bool random_play = false;
static Entries playlist = {0};
static int play_index = -1;
static int *play_history = NULL;
static size_t play_history_len = 0;
static size_t play_history_cap = 0;
static int *play_forward = NULL;
static size_t play_forward_len = 0;
static size_t play_forward_cap = 0;

static int NORMAL_ATTR, SELECTED_ATTR, PLAYING_ATTR, PLAYING_SELECTED_ATTR;

typedef struct {
    Entry entry;
    int order;
} QueueItem;

static QueueItem *queue_items = NULL;
static size_t queue_count = 0;
static size_t queue_cap = 0;
static bool queue_mode = false;
static int queue_play_pos = -1;
static int swallow_next_arrow = 0;


static void die_nomem(void) { endwin(); fprintf(stderr, "simpleflac: out of memory\n"); exit(1); }
static void *xcalloc(size_t n, size_t s) { void *p = calloc(n, s); if (!p) die_nomem(); return p; }
static char *xstrdup(const char *s) { char *p = strdup(s ? s : ""); if (!p) die_nomem(); return p; }

static char *xasprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); char *s = NULL;
    if (vasprintf(&s, fmt, ap) < 0) die_nomem();
    va_end(ap); return s;
}

static void strlist_push(StrList *l, char *s) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->v = realloc(l->v, l->cap * sizeof(char*)); if (!l->v) die_nomem(); }
    l->v[l->n++] = s;
}
static void strlist_free(StrList *l) { for (size_t i=0;i<l->n;i++) free(l->v[i]); free(l->v); memset(l,0,sizeof(*l)); }

static void entries_push(Entries *e, Entry it) {
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 64; e->v = realloc(e->v, e->cap * sizeof(Entry)); if (!e->v) die_nomem(); }
    e->v[e->n++] = it;
}
static void cue_push(CueTracks *t, CueTrack c) {
    if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 32; t->v = realloc(t->v, t->cap * sizeof(CueTrack)); if (!t->v) die_nomem(); }
    t->v[t->n++] = c;
}
static void list_push(ListTracks *t, ListTrack c) {
    if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 32; t->v = realloc(t->v, t->cap * sizeof(ListTrack)); if (!t->v) die_nomem(); }
    t->v[t->n++] = c;
}

static void free_cuetrack(CueTrack *c) { free(c->title); free(c->performer); free(c->audio); }
static void free_listtrack(ListTrack *l) { free(l->title); free(l->source); }
static void entries_free(Entries *e) {
    for (size_t i=0;i<e->n;i++) { free(e->v[i].path); free(e->v[i].label); if (e->v[i].cue) { free_cuetrack(e->v[i].cue); free(e->v[i].cue); } if (e->v[i].pl) { free_listtrack(e->v[i].pl); free(e->v[i].pl); } }
    free(e->v); memset(e,0,sizeof(*e));
}
static void cue_free(CueTracks *t) { for (size_t i=0;i<t->n;i++) free_cuetrack(&t->v[i]); free(t->v); memset(t,0,sizeof(*t)); }
static void list_free(ListTracks *t) { for (size_t i=0;i<t->n;i++) free_listtrack(&t->v[i]); free(t->v); memset(t,0,sizeof(*t)); }

static bool st_is_dir(const char *p) { struct stat st; return stat(p,&st)==0 && S_ISDIR(st.st_mode); }
static bool st_is_file(const char *p) { struct stat st; return stat(p,&st)==0 && S_ISREG(st.st_mode); }
static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); return s ? s+1 : p; }
static char *dir_name_dup(const char *p) { char *d=xstrdup(p); char *s=strrchr(d,'/'); if(!s) { free(d); return xstrdup("."); } if(s==d) s[1]='\0'; else *s='\0'; return d; }
static char *path_join(const char *a, const char *b) { if (!a || !*a) return xstrdup(b); if (b[0]=='/') return xstrdup(b); return xasprintf("%s/%s", a, b); }
static char *expand_home(const char *s) { if (s[0]=='~' && (s[1]=='/' || s[1]=='\0')) { const char *h=getenv("HOME"); return xasprintf("%s%s", h?h:"", s+1); } return xstrdup(s); }
static void trim_inplace(char *s) { char *p=s; while(isspace((unsigned char)*p)) p++; if(p!=s) memmove(s,p,strlen(p)+1); size_t n=strlen(s); while(n && isspace((unsigned char)s[n-1])) s[--n]='\0'; if((s[0]=='"'&&s[n-1]=='"')||(s[0]=='\''&&s[n-1]=='\'')) { s[n-1]='\0'; memmove(s,s+1,n-1); } }
static char *lower_dup(const char *s) { char *r=xstrdup(s); for(char *p=r;*p;p++) *p=tolower((unsigned char)*p); return r; }
static const char *suffix_of(const char *p) { const char *b=base_name(p); const char *dot=strrchr(b,'.'); return dot?dot:""; }
static bool suffix_in(const char *p, const char **exts) { char *s=lower_dup(suffix_of(p)); bool ok=false; for(size_t i=0;exts[i];i++) if(strcmp(s,exts[i])==0){ok=true;break;} free(s); return ok; }
static const char *audio_exts[]={".flac",".mp3",".ogg",".wav",".m4a",".aac",".opus",".ape",NULL};
static const char *playlist_exts[]={".m3u",".m3u8",".pls",".xspf",NULL};
static bool playable_file(const char *p){ return st_is_file(p)&&suffix_in(p,audio_exts); }
static bool cue_file(const char *p){ return st_is_file(p)&&strcasecmp(suffix_of(p),".cue")==0; }
static bool playlist_file(const char *p){ return st_is_file(p)&&suffix_in(p,playlist_exts); }
static bool is_url(const char *s){ return !strncasecmp(s,"http://",7)||!strncasecmp(s,"https://",8)||!strncasecmp(s,"ftp://",6)||!strncasecmp(s,"ftps://",7); }

static char from_hex(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return 0; }
static char *url_decode(const char *s) { char *o=xcalloc(strlen(s)+1,1), *w=o; for(size_t i=0;s[i];i++){ if(s[i]=='%'&&isxdigit((unsigned char)s[i+1])&&isxdigit((unsigned char)s[i+2])){ *w++=(from_hex(s[i+1])<<4)|from_hex(s[i+2]); i+=2; } else *w++=s[i]; } return o; }
static char *url_path_part(const char *s) { const char *p=strstr(s,"://"); if(!p) return xstrdup(s); p+=3; const char *slash=strchr(p,'/'); if(!slash) return xstrdup(""); char *r=xstrdup(slash); char *q=strchr(r,'?'); if(q)*q='\0'; q=strchr(r,'#'); if(q)*q='\0'; return r; }
static char *source_name(const char *src) { if(is_url(src)){ char *path=url_path_part(src); char *dec=url_decode(path); free(path); char *r=xstrdup(*base_name(dec)?base_name(dec):src); free(dec); return r; } return xstrdup(base_name(src)); }

static bool source_is_playable(const char *value, const char *base_dir) {
    char *t=xstrdup(value); trim_inplace(t); if(!*t){free(t);return false;}
    bool ok=false;
    if(is_url(t)){ char *path=url_path_part(t); ok=suffix_in(path,audio_exts); free(path); }
    else { char *dec=url_decode(t); for(char *p=dec;*p;p++) if(*p=='\\') *p='/'; char *exp=expand_home(dec); char *full = exp[0]=='/' ? xstrdup(exp) : (base_dir ? path_join(base_dir,exp) : xstrdup(exp)); ok=suffix_in(full,audio_exts)&&st_is_file(full); free(dec); free(exp); free(full); }
    free(t); return ok;
}

static char *normalize_playlist_source(const char *value, const char *base_dir) {
    char *t=xstrdup(value); trim_inplace(t); if(!*t){free(t);return NULL;}
    char *dec=url_decode(t); free(t); for(char *p=dec;*p;p++) if(*p=='\\') *p='/';
    if(!strncasecmp(dec,"file://",7)){ char *path=url_path_part(dec); free(dec); char *exp=expand_home(path); free(path); return exp; }
    if(is_url(dec)) return dec;
    char *exp=expand_home(dec); free(dec); if(exp[0]=='/') return exp; char *full=path_join(base_dir,exp); free(exp); return full;
}

static char *read_text_file(const char *path) {
    FILE *f=fopen(path,"rb"); if(!f) return xstrdup(""); size_t cap=READ_CHUNK,n=0; char *buf=xcalloc(cap,1); size_t r;
    while((r=fread(buf+n,1,READ_CHUNK,f))>0){ n+=r; if(n+READ_CHUNK+1>cap){cap*=2; buf=realloc(buf,cap); if(!buf)die_nomem();}}
    fclose(f); buf[n]='\0'; if(n>=3 && (unsigned char)buf[0]==0xEF && (unsigned char)buf[1]==0xBB && (unsigned char)buf[2]==0xBF) memmove(buf,buf+3,n-2);
    return buf;
}

static double cue_time_to_seconds(const char *v){ int m=0,s=0,f=0; sscanf(v,"%d:%d:%d",&m,&s,&f); return m*60.0+s+f/75.0; }
static bool starts_ci(const char *s,const char *p){ while(*p){ if(toupper((unsigned char)*s++)!=toupper((unsigned char)*p++)) return false; } return true; }
static char *quoted_after(const char *s) { const char *q=strchr(s,'"'); if(!q) return NULL; const char *e=strchr(q+1,'"'); if(!e) return NULL; return strndup(q+1,e-q-1); }

static CueTracks parse_cue(const char *cue_path) {
    CueTracks tracks={0}; char *text=read_text_file(cue_path); char *cue_dir=dir_name_dup(cue_path); char *current_audio=NULL; char *album_performer=xstrdup(""); CueTrack *cur=NULL;
    for(char *save=NULL,*line=strtok_r(text,"\n",&save); line; line=strtok_r(NULL,"\n",&save)){
        trim_inplace(line); if(!*line) continue;
        if(starts_ci(line,"FILE ")){ char *q=quoted_after(line); if(q){ free(current_audio); current_audio=path_join(cue_dir,q); free(q);} continue; }
        if(starts_ci(line,"PERFORMER ") && !cur){ free(album_performer); album_performer=xstrdup(line+10); trim_inplace(album_performer); continue; }
        if(starts_ci(line,"TRACK ")){ int num=0; sscanf(line+6,"%d",&num); CueTrack c={0}; c.number=num; c.title=xasprintf("Track %02d",num); c.performer=xstrdup(album_performer); c.audio=current_audio?xstrdup(current_audio):NULL; c.start=-1; c.end=0; c.has_end=false; cue_push(&tracks,c); cur=&tracks.v[tracks.n-1]; continue; }
        if(cur && starts_ci(line,"TITLE ")){ free(cur->title); cur->title=xstrdup(line+6); trim_inplace(cur->title); }
        else if(cur && starts_ci(line,"PERFORMER ")){ free(cur->performer); cur->performer=xstrdup(line+10); trim_inplace(cur->performer); }
        else if(cur && starts_ci(line,"INDEX 01 ")){ cur->start=cue_time_to_seconds(line+9); }
    }
    for(size_t i=0;i+1<tracks.n;i++) if(tracks.v[i].audio && tracks.v[i+1].audio && strcmp(tracks.v[i].audio,tracks.v[i+1].audio)==0){ tracks.v[i].end=tracks.v[i+1].start; tracks.v[i].has_end=true; }
    CueTracks filtered={0}; for(size_t i=0;i<tracks.n;i++){ CueTrack *t=&tracks.v[i]; if(t->audio && st_is_file(t->audio) && t->start>=0){ cue_push(&filtered,*t); memset(t,0,sizeof(*t)); } }
    cue_free(&tracks); free(current_audio); free(album_performer); free(cue_dir); free(text); return filtered;
}

static ListTracks parse_m3u(const char *path){
    ListTracks out={0}; char *text=read_text_file(path); char *base=dir_name_dup(path); char *pending=NULL;
    for(char *save=NULL,*line=strtok_r(text,"\n",&save); line; line=strtok_r(NULL,"\n",&save)){
        trim_inplace(line); if(!*line) continue;
        if(!strncmp(line,"#EXTINF:",8)){ char *comma=strchr(line,','); free(pending); pending=comma&&comma[1]?xstrdup(comma+1):NULL; continue; }
        if(line[0]=='#') continue;
        char *src=normalize_playlist_source(line,base); if(!src || !source_is_playable(src,base)){ free(src); free(pending); pending=NULL; continue; }
        char *nm=source_name(src); ListTrack t={.number=(int)out.n+1,.title=pending?xstrdup(pending):nm,.source=src}; if(pending) free(nm); list_push(&out,t); free(pending); pending=NULL;
    }
    free(pending); free(base); free(text); return out;
}

typedef struct { int n; char *file; char *title; } PlsItem;
static int cmp_pls(const void *a,const void*b){ const PlsItem *x=a,*y=b; return x->n-y->n; }
static ListTracks parse_pls(const char *path){
    ListTracks out={0}; char *text=read_text_file(path); char *base=dir_name_dup(path); PlsItem *items=NULL; size_t n=0,cap=0;
    for(char *save=NULL,*line=strtok_r(text,"\n",&save); line; line=strtok_r(NULL,"\n",&save)){
        trim_inplace(line); if(!*line || line[0]=='#'||line[0]==';'||line[0]=='[') continue; char *eq=strchr(line,'='); if(!eq) continue; *eq='\0'; char *key=line,*val=eq+1; trim_inplace(key); trim_inplace(val);
        int num=0; if(sscanf(key,"%*[Ff]%*[Ii]%*[Ll]%*[Ee]%d",&num)==1 || sscanf(key,"File%d",&num)==1 || sscanf(key,"file%d",&num)==1){
            if(n==cap){cap=cap?cap*2:16;items=realloc(items,cap*sizeof(*items));if(!items)die_nomem();} items[n++]=(PlsItem){num,xstrdup(val),NULL};
        } else if(sscanf(key,"%*[Tt]%*[Ii]%*[Tt]%*[Ll]%*[Ee]%d",&num)==1 || sscanf(key,"Title%d",&num)==1 || sscanf(key,"title%d",&num)==1){
            for(size_t i=0;i<n;i++) if(items[i].n==num){ free(items[i].title); items[i].title=xstrdup(val); goto done_title; }
            if(n==cap){cap=cap?cap*2:16;items=realloc(items,cap*sizeof(*items));if(!items)die_nomem();} items[n++]=(PlsItem){num,NULL,xstrdup(val)};
        }
        done_title: ;
    }
    qsort(items,n,sizeof(*items),cmp_pls);
    for(size_t i=0;i<n;i++) if(items[i].file){ char *src=normalize_playlist_source(items[i].file,base); if(src && source_is_playable(src,base)){ char *nm=source_name(src); ListTrack t={.number=(int)out.n+1,.title=items[i].title?xstrdup(items[i].title):nm,.source=src}; if(items[i].title) free(nm); list_push(&out,t);} else free(src); }
    for(size_t i=0;i<n;i++){free(items[i].file);free(items[i].title);} free(items); free(base); free(text); return out;
}

static char *xml_text_between(const char *from, const char *tag, const char **after){
    char *open=xasprintf("<%s",tag); char *close=xasprintf("</%s>",tag); const char *p=strstr(from,open); free(open); if(!p){free(close);return NULL;} p=strchr(p,'>'); if(!p){free(close);return NULL;} p++; const char *e=strstr(p,close); free(close); if(!e)return NULL; if(after)*after=e; char *raw=strndup(p,e-p); char *dec=url_decode(raw); free(raw); return dec;
}
static ListTracks parse_xspf(const char *path){
    ListTracks out={0}; char *text=read_text_file(path); char *base=dir_name_dup(path); const char *p=text;
    while((p=strstr(p,"<track"))){ const char *track_end=strstr(p,"</track>"); if(!track_end) break; char *block=strndup(p,track_end-p); char *loc=xml_text_between(block,"location",NULL); if(loc){ char *src=normalize_playlist_source(loc,base); if(src && source_is_playable(src,base)){ char *title=xml_text_between(block,"title",NULL); char *creator=xml_text_between(block,"creator",NULL); char *nm=source_name(src); char *label=title&&*title ? (creator&&*creator ? xasprintf("%s - %s",creator,title) : xstrdup(title)) : nm; if(title&&*title) free(nm); ListTrack t={.number=(int)out.n+1,.title=label,.source=src}; list_push(&out,t); free(title); free(creator); } else free(src); free(loc);} free(block); p=track_end+8; }
    free(base); free(text); return out;
}
static ListTracks parse_playlist(const char *path){ if(!strcasecmp(suffix_of(path),".m3u")||!strcasecmp(suffix_of(path),".m3u8")) return parse_m3u(path); if(!strcasecmp(suffix_of(path),".pls")) return parse_pls(path); if(!strcasecmp(suffix_of(path),".xspf")) return parse_xspf(path); return (ListTracks){0}; }

static int cmp_strp(const void *a, const void *b) {
    const char *pa = *(const char * const *)a;
    const char *pb = *(const char * const *)b;

    const char *sa = strrchr(pa, '/');
    const char *sb = strrchr(pb, '/');
    sa = sa ? sa + 1 : pa;
    sb = sb ? sb + 1 : pb;

    while (*sa && *sb) {
        if (isdigit((unsigned char)*sa) && isdigit((unsigned char)*sb)) {
            char *ea, *eb;
            unsigned long na = strtoul(sa, &ea, 10);
            unsigned long nb = strtoul(sb, &eb, 10);
            if (na < nb) return -1;
            if (na > nb) return 1;
            sa = ea;
            sb = eb;
            continue;
        }

        unsigned char ca = (unsigned char)tolower((unsigned char)*sa);
        unsigned char cb = (unsigned char)tolower((unsigned char)*sb);
        if (ca < cb) return -1;
        if (ca > cb) return 1;
        sa++;
        sb++;
    }

    if (*sa) return 1;
    if (*sb) return -1;
    return 0;
}
static void list_dir_sorted(const char *path, StrList *dirs, StrList *cues, StrList *pls, StrList *files){
    DIR *d=opendir(path); if(!d) return; struct dirent *de;
    while((de=readdir(d))){ if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue; char *full=path_join(path,de->d_name); if(st_is_dir(full)) strlist_push(dirs,full); else if(cue_file(full)) strlist_push(cues,full); else if(playlist_file(full)) strlist_push(pls,full); else if(playable_file(full)) strlist_push(files,full); else free(full); }
    closedir(d); qsort(dirs->v,dirs->n,sizeof(char*),cmp_strp); qsort(cues->v,cues->n,sizeof(char*),cmp_strp); qsort(pls->v,pls->n,sizeof(char*),cmp_strp); qsort(files->v,files->n,sizeof(char*),cmp_strp);
}

static StrList choose_start_roots(void){
    StrList roots={0}, uniq={0}; const char *home=getenv("HOME"); const char *user=getlogin(); if(!user) user=getenv("USER");
    if(home){ char *m=xasprintf("%s/Music",home); if(st_is_dir(m)) strlist_push(&roots,m); else free(m); }
    char *bases[3]; bases[0]=user?xasprintf("/media/%s",user):NULL; bases[1]=user?xasprintf("/run/media/%s",user):NULL; bases[2]=xstrdup("/mnt");
    for(int i=0;i<3;i++) if(bases[i]){ if(st_is_dir(bases[i])){ DIR*d=opendir(bases[i]); struct dirent*de; if(d){ while((de=readdir(d))){ if(de->d_name[0]=='.') continue; char *p=path_join(bases[i],de->d_name); if(st_is_dir(p)) strlist_push(&roots,p); else free(p); } closedir(d);} } free(bases[i]); }
    for(size_t i=0;i<roots.n;i++){ bool seen=false; for(size_t j=0;j<uniq.n;j++) if(!strcmp(roots.v[i],uniq.v[j])){seen=true;break;} if(!seen) strlist_push(&uniq,xstrdup(roots.v[i])); }
    strlist_free(&roots); return uniq;
}

static Entries make_entries(const char *current, StrList *roots, char **title){
    Entries e={0};
    if(!current){ for(size_t i=0;i<roots->n;i++) entries_push(&e,(Entry){EK_DIR,xstrdup(roots->v[i]),NULL,NULL,NULL}); *title=xstrdup("Choose Music Root"); return e; }
    if(!strcasecmp(suffix_of(current),".cue")){ char *parent=dir_name_dup(current); entries_push(&e,(Entry){EK_UP,parent,NULL,NULL,NULL}); CueTracks t=parse_cue(current); for(size_t i=0;i<t.n;i++){ CueTrack *src=&t.v[i]; CueTrack *c=xcalloc(1,sizeof(*c)); *c=*src; memset(src,0,sizeof(*src)); char *label = c->performer && *c->performer ? xasprintf("%02d. %s - %s",c->number,c->performer,c->title) : xasprintf("%02d. %s",c->number,c->title); entries_push(&e,(Entry){EK_CUETRACK,xstrdup(current),label,c,NULL}); } cue_free(&t); *title=xasprintf("CUE: %s",base_name(current)); return e; }
    if(suffix_in(current,playlist_exts)){ char *parent=dir_name_dup(current); entries_push(&e,(Entry){EK_UP,parent,NULL,NULL,NULL}); ListTracks t=parse_playlist(current); for(size_t i=0;i<t.n;i++){ ListTrack *src=&t.v[i]; ListTrack *pl=xcalloc(1,sizeof(*pl)); *pl=*src; memset(src,0,sizeof(*src)); char *label=xasprintf("%02d. %s",pl->number,pl->title); entries_push(&e,(Entry){EK_PLTRACK,xstrdup(current),label,NULL,pl}); } *title=xasprintf("PLAYLIST: %zu %s",t.n,base_name(current)); list_free(&t); return e; }
    char *parent=dir_name_dup(current); entries_push(&e,(Entry){EK_UP,parent,NULL,NULL,NULL}); StrList dirs={0},cues={0},pls={0},files={0}; list_dir_sorted(current,&dirs,&cues,&pls,&files);
    for(size_t i=0;i<dirs.n;i++) entries_push(&e,(Entry){EK_DIR,xstrdup(dirs.v[i]),NULL,NULL,NULL});
    for(size_t i=0;i<cues.n;i++) entries_push(&e,(Entry){EK_CUE,xstrdup(cues.v[i]),NULL,NULL,NULL});
    for(size_t i=0;i<pls.n;i++) entries_push(&e,(Entry){EK_PLAYLISTFILE,xstrdup(pls.v[i]),NULL,NULL,NULL});
    for(size_t i=0;i<files.n;i++) entries_push(&e,(Entry){EK_FILE,xstrdup(files.v[i]),NULL,NULL,NULL});
    strlist_free(&dirs); strlist_free(&cues); strlist_free(&pls); strlist_free(&files); *title=xstrdup(current); return e;
}

static void cleanup_mpv_socket_path(void){ if(mpv_socket_path[0]) unlink(mpv_socket_path); if(mpv_socket_tmpdir[0]){ rmdir(mpv_socket_tmpdir); mpv_socket_tmpdir[0]='\0'; } }
static int set_private_tmp_mpv_socket_path(void){ struct sockaddr_un addr; const char *bases[2]={getenv("TMPDIR"),"/tmp"}; for(size_t i=0;i<2;i++){ const char *base=bases[i]; char tmpl[PATH_MAX]; char *dir; int n; if(!base||!*base) continue; if(i==1&&bases[0]&&*bases[0]&&!strcmp(bases[0],"/tmp")) continue; n=snprintf(tmpl,sizeof(tmpl),"%s/simpleflac-mpv-XXXXXX",base); if(n<0||(size_t)n>=sizeof(tmpl)) continue; dir=mkdtemp(tmpl); if(!dir) continue; n=snprintf(mpv_socket_path,sizeof(mpv_socket_path),"%s/socket",dir); if(n>=0&&(size_t)n<sizeof(mpv_socket_path)&&(size_t)n<sizeof(addr.sun_path)){ snprintf(mpv_socket_tmpdir,sizeof(mpv_socket_tmpdir),"%s",dir); return 1; } rmdir(dir); mpv_socket_path[0]='\0'; } return 0; }
static void init_mpv_socket_path(void){ struct sockaddr_un addr; const char *runtime=getenv("XDG_RUNTIME_DIR"); int n=-1; if(runtime&&*runtime)n=snprintf(mpv_socket_path,sizeof(mpv_socket_path),"%s/simpleflac-mpv-%ld.sock",runtime,(long)getpid()); if(n<0||(size_t)n>=sizeof(mpv_socket_path)||(size_t)n>=sizeof(addr.sun_path)){ if(!set_private_tmp_mpv_socket_path()) mpv_socket_path[0]='\0'; } if(mpv_socket_path[0]) unlink(mpv_socket_path); atexit(cleanup_mpv_socket_path); }
static void mpv_command_raw(const char *json){ int fd=socket(AF_UNIX,SOCK_STREAM,0); if(fd<0)return; struct sockaddr_un a={0}; size_t len=strlen(mpv_socket_path); if(len>=sizeof(a.sun_path)){ close(fd); return; } a.sun_family=AF_UNIX; memcpy(a.sun_path,mpv_socket_path,len+1); if(connect(fd,(struct sockaddr*)&a,sizeof(a))==0){ ssize_t ignored; ignored=write(fd,json,strlen(json)); (void)ignored; ignored=write(fd,"\n",1); (void)ignored;} close(fd); }
static void set_volume(int v){ char *j=xasprintf("{\"command\":[\"set_property\",\"volume\",%d]}",v); mpv_command_raw(j); free(j); }
static void stop_player(void){ if(current_player>0){ kill(current_player,SIGTERM); for(int i=0;i<10;i++){ if(waitpid(current_player,NULL,WNOHANG)==current_player) break; usleep(100000);} kill(current_player,SIGKILL); waitpid(current_player,NULL,WNOHANG);} current_player=-1; paused=false; unlink(mpv_socket_path); }
static char *toggle_pause(void){ if(current_player<0) return xstrdup("Nothing playing"); mpv_command_raw("{\"command\":[\"cycle\",\"pause\"]}"); paused=!paused; return xstrdup(paused?"Paused":"Playing"); }
static void play_in_mpv(const char *source, double start, bool has_start, double end, bool has_end){
    mpv_command_raw("{\"command\":[\"quit\"]}");
    usleep(100000);
    stop_player();
    pid_t pid=fork(); if(pid==0){ int dn=open("/dev/null",O_RDWR); if(dn>=0){dup2(dn,1);dup2(dn,2);close(dn);} char startarg[64], endarg[64], sockarg[PATH_MAX+64]; snprintf(sockarg,sizeof(sockarg),"--input-ipc-server=%s",mpv_socket_path); char *argv[12]; int n=0; argv[n++]="mpv"; argv[n++]="--no-video"; argv[n++]="--no-audio-display"; argv[n++]="--force-window=no"; argv[n++]="--quiet"; argv[n++]=sockarg; if(has_start){snprintf(startarg,sizeof(startarg),"--start=%.3f",start); argv[n++]=startarg;} if(has_end){snprintf(endarg,sizeof(endarg),"--end=%.3f",end); argv[n++]=endarg;} argv[n++]=(char*)source; argv[n]=NULL; execvp("mpv",argv); _exit(127);} current_player=pid; usleep(150000); set_volume(current_volume); paused=false; }

static bool same_entry(const Entry *a,const Entry*b){ if(a->kind!=b->kind) return false; if(strcmp(a->path?a->path:"",b->path?b->path:"")) return false; if(a->kind==EK_CUETRACK) return a->cue&&b->cue&&a->cue->number==b->cue->number&&a->cue->start==b->cue->start; if(a->kind==EK_PLTRACK) return a->pl&&b->pl&&a->pl->number==b->pl->number; return true; }
static bool entry_is_queue_playing(const Entry *e){
    return queue_mode &&
           queue_play_pos >= 0 &&
           (size_t)queue_play_pos < queue_count &&
           same_entry(e, &queue_items[queue_play_pos].entry);
}

static bool entry_is_playing(const Entry *e){
    if(queue_mode && queue_play_pos >= 0)
        return entry_is_queue_playing(e);

    return playlist.n &&
           play_index >= 0 &&
           (size_t)play_index < playlist.n &&
           same_entry(e, &playlist.v[play_index]);
}

static void stack_push(int **v, size_t *len, size_t *cap, int idx){ if(idx<0) return; if(*len==*cap){ *cap=*cap?*cap*2:128; *v=realloc(*v,*cap*sizeof(int)); if(!*v) die_nomem(); } (*v)[(*len)++]=idx; }
static bool stack_pop(int *v, size_t *len, int *idx){ if(!*len) return false; *idx=v[--(*len)]; return true; }
static void history_clear(void){ play_history_len=0; play_forward_len=0; }
static void history_push(int idx){ stack_push(&play_history,&play_history_len,&play_history_cap,idx); }
static bool history_pop(int *idx){ return stack_pop(play_history,&play_history_len,idx); }
static void forward_push(int idx){ stack_push(&play_forward,&play_forward_len,&play_forward_cap,idx); }
static bool forward_pop(int *idx){ return stack_pop(play_forward,&play_forward_len,idx); }
static void forward_clear(void){ play_forward_len=0; }

static void clone_playlist_from(Entries *entries, EntryKind kind){ entries_free(&playlist); history_clear(); play_index=-1; for(size_t i=0;i<entries->n;i++) if(entries->v[i].kind==kind){ Entry s=entries->v[i], c={0}; c.kind=s.kind; c.path=xstrdup(s.path); c.label=s.label?xstrdup(s.label):NULL; if(s.cue){ c.cue=xcalloc(1,sizeof(CueTrack)); *c.cue=(CueTrack){s.cue->number,xstrdup(s.cue->title),xstrdup(s.cue->performer),xstrdup(s.cue->audio),s.cue->start,s.cue->end,s.cue->has_end}; } if(s.pl){ c.pl=xcalloc(1,sizeof(ListTrack)); *c.pl=(ListTrack){s.pl->number,xstrdup(s.pl->title),xstrdup(s.pl->source)}; } entries_push(&playlist,c); } }
static char *play_playlist_item_core(int idx, bool record_history){ if(!playlist.n) return xstrdup("Nothing to play"); if(idx<0 || (size_t)idx>=playlist.n) return xstrdup("Nothing to play"); if(record_history && play_index>=0 && play_index!=idx) history_push(play_index); play_index=idx; Entry *e=&playlist.v[play_index]; if(e->kind==EK_FILE){ play_in_mpv(e->path,0,false,0,false); return xasprintf("Playing: %s",base_name(e->path)); } if(e->kind==EK_CUETRACK){ play_in_mpv(e->cue->audio,e->cue->start,true,e->cue->end,e->cue->has_end); return xasprintf("Playing: %s",e->label); } if(e->kind==EK_PLTRACK){ play_in_mpv(e->pl->source,0,false,0,false); return xasprintf("Playing: %s",e->label); } return xstrdup("Nothing to play"); }
static char *play_playlist_item(int idx){ return play_playlist_item_core(idx,true); }
static char *play_playlist_item_no_history(int idx){ return play_playlist_item_core(idx,false); }
static int next_play_index(void){ if(!playlist.n||play_index<0)return-1; if(random_play){ if(playlist.n==1)return-1; int n; do{ n=rand()%(int)playlist.n; }while(n==play_index); return n; } int n=play_index+1; return n>=(int)playlist.n?-1:n; }

static void queue_clear_all(void);


static char *queue_play_item_at(int pos){
    if(!queue_mode || queue_count==0) return xstrdup("No playlist");
    if(pos < 0 || (size_t)pos >= queue_count){
        return xstrdup("End of playlist");
    }

    entries_free(&playlist);
    history_clear();
    forward_clear();
    play_index = -1;

    queue_play_pos = pos;
    Entry *e = &queue_items[pos].entry;

    if(e->kind==EK_FILE){
        play_in_mpv(e->path,0,false,0,false);
        return xasprintf("Playlist %d: %s", queue_items[pos].order, base_name(e->path));
    }

    if(e->kind==EK_CUETRACK){
        play_in_mpv(e->cue->audio,e->cue->start,true,e->cue->end,e->cue->has_end);
        return xasprintf("Playlist %d: %s", queue_items[pos].order, e->label);
    }

    if(e->kind==EK_PLTRACK){
        play_in_mpv(e->pl->source,0,false,0,false);
        return xasprintf("Playlist %d: %s", queue_items[pos].order, e->label);
    }

    return xstrdup("No playlist item");
}

static int queue_prev_index(void){
    if(!queue_mode || queue_count==0) return -1;
    if(queue_play_pos <= 0) return -1;
    return queue_play_pos - 1;
}

static int queue_next_index(void){
    if(!queue_mode || queue_count==0) return -1;
    if(queue_play_pos < 0) return 0;
    if((size_t)(queue_play_pos + 1) < queue_count) return queue_play_pos + 1;
    return -1;
}

static char *check_auto_advance(void){
    if(current_player<0) return NULL;

    int st;
    pid_t r=waitpid(current_player,&st,WNOHANG);
    if(r==0) return NULL;

    current_player=-1;
    unlink(mpv_socket_path);

    if(queue_mode){
        if(queue_count > 0)
            return queue_play_item_at(queue_play_pos < 0 ? 0 : queue_next_index());

        queue_clear_all();
        return xstrdup("End of playlist");
    }

    if(!continuous||play_index<0) return NULL;

    int n=next_play_index();
    if(n<0){
        play_index=-1;
        return xstrdup("End of folder");
    }

    return play_playlist_item(n);
}
static char *shorten_utf8ish(const char *text,int width){
    if(width<=1) return xstrdup("");

    mbstate_t st;
    memset(&st,0,sizeof(st));

    const char *p=text;
    size_t bytes=0;
    int cells=0;

    while(*p){
        wchar_t wc;
        size_t len=mbrtowc(&wc,p,MB_CUR_MAX,&st);

        if(len==(size_t)-1 || len==(size_t)-2 || len==0)
            break;

        int w=wcwidth(wc);
        if(w<0) w=1;

        if(cells+w>width)
            break;

        cells+=w;
        bytes+=len;
        p+=len;
    }

    return strndup(text,bytes);
}

static void draw_full_line(WINDOW*w,int y,const char*text,int width,int attr){ (void)w; char *s=shorten_utf8ish(text,width-1); int need=width-1; char *line=xcalloc(need+1,1); snprintf(line,need+1,"%-*s",need,s); attron(attr); mvaddnstr(y,0,line,need); attroff(attr); free(s); free(line); }
static char *entry_label(Entry *e){ switch(e->kind){ case EK_UP: return xstrdup("../"); case EK_DIR: return xasprintf("[%s]/",base_name(e->path)); case EK_CUE: return xasprintf("[CUE] %s",base_name(e->path)); case EK_PLAYLISTFILE: return xasprintf("[LIST] %s",base_name(e->path)); case EK_FILE: return xstrdup(base_name(e->path)); case EK_CUETRACK: case EK_PLTRACK: return xstrdup(e->label?e->label:""); } return xstrdup("(empty)"); }

static void select_playing_entry(Entries *entries, int *selected){
    if(play_index < 0 || !playlist.n) return;
    for(size_t i=0;i<entries->n;i++){
        if(entry_is_playing(&entries->v[i])){
            *selected=(int)i;
            return;
        }
    }
}


static void select_queue_entry(Entries *entries, int *selected){
    if(!queue_mode || queue_play_pos < 0 || (size_t)queue_play_pos >= queue_count)
        return;

    Entry *q = &queue_items[queue_play_pos].entry;

    for(size_t i=0;i<entries->n;i++){
        if(same_entry(&entries->v[i], q)){
            *selected = (int)i;
            return;
        }
    }
}

static int escape_key_action(void){
    int seq[16];
    int n = 0;

    timeout(25);
    for(int i=0;i<16;i++){
        int c = getch();
        if(c == ERR) break;
        seq[n++] = c;
        if((c >= 'A' && c <= 'Z') || c == '~')
            break;
    }
    timeout(200);

    if(n == 0) return -1;

    /* Plain arrows only: ESC [ A/B/C/D */
    if(n == 2 && seq[0] == '['){
        if(seq[1] == 'A') return KEY_UP;
        if(seq[1] == 'B') return KEY_DOWN;
        if(seq[1] == 'C') return KEY_RIGHT;
        if(seq[1] == 'D') return KEY_LEFT;
    }

    /* Application cursor arrows: ESC O A/B/C/D */
    if(n == 2 && seq[0] == 'O'){
        if(seq[1] == 'A') return KEY_UP;
        if(seq[1] == 'B') return KEY_DOWN;
        if(seq[1] == 'C') return KEY_RIGHT;
        if(seq[1] == 'D') return KEY_LEFT;
    }

    /* Page Up / Page Down */
    if(n == 3 && seq[0] == '[' && seq[1] == '5' && seq[2] == '~') return KEY_PPAGE;
    if(n == 3 && seq[0] == '[' && seq[1] == '6' && seq[2] == '~') return KEY_NPAGE;

    /* Everything else is modified/focus/workspace junk.
       GNOME workspace switches can then leak a plain Up/Down on refocus. */
    swallow_next_arrow = 1;
    return 0;
}


static bool queue_same_entry(const Entry *a,const Entry *b){
    return same_entry(a,b);
}

static int queue_find(const Entry *e){
    for(size_t i=0;i<queue_count;i++)
        if(queue_same_entry(e,&queue_items[i].entry))
            return (int)i;
    return -1;
}

static int queue_order_for(const Entry *e){
    int idx = queue_find(e);
    if(idx < 0) return 0;
    return queue_items[idx].order;
}

static void queue_add(const Entry *e){
    int idx = queue_find(e);

    if(idx >= 0){
        queue_items[idx].order++;
        return;
    }

    if(queue_count == queue_cap){
        queue_cap = queue_cap ? queue_cap * 2 : 32;
        queue_items = realloc(queue_items,
                              queue_cap * sizeof(QueueItem));
    }

    QueueItem *q = &queue_items[queue_count++];

    memset(q,0,sizeof(*q));

    q->entry.kind = e->kind;
    q->entry.path = xstrdup(e->path);

    if(e->label)
        q->entry.label = xstrdup(e->label);

    if(e->cue){
        q->entry.cue = xcalloc(1,sizeof(CueTrack));
        *q->entry.cue = *e->cue;
        q->entry.cue->title = xstrdup(e->cue->title);
        q->entry.cue->performer = xstrdup(e->cue->performer);
        q->entry.cue->audio = xstrdup(e->cue->audio);
    }

    if(e->pl){
        q->entry.pl = xcalloc(1,sizeof(ListTrack));
        *q->entry.pl = *e->pl;
        q->entry.pl->title = xstrdup(e->pl->title);
        q->entry.pl->source = xstrdup(e->pl->source);
    }

    q->order = (int)queue_count;
    queue_mode = true;
}

static void queue_clear_all(void){
    for(size_t i=0;i<queue_count;i++){
        free(queue_items[i].entry.path);
        free(queue_items[i].entry.label);
    }

    free(queue_items);
    queue_items = NULL;
    queue_count = 0;
    queue_cap = 0;
    queue_mode = false;
    queue_play_pos = -1;
}

static char *play_browser_entry(Entries *entries, Entry *entry){
    if(queue_mode)
        queue_clear_all();

    clone_playlist_from(entries,entry->kind);
    int wanted=0;

    for(size_t i=0;i<playlist.n;i++){
        if(same_entry(&playlist.v[i],entry)){
            wanted=(int)i;
            break;
        }
    }

    return play_playlist_item(wanted);
}

static void browser(StrList *roots, const char *startup_path){
    char *current=NULL,*startup_target=NULL,*status=xstrdup("Choose a folder");
    bool startup_pending=false;

    if(startup_path){
        if(st_is_dir(startup_path)){
            current=xstrdup(startup_path);
        } else if(playable_file(startup_path)){
            current=dir_name_dup(startup_path);
            startup_target=xstrdup(startup_path);
            startup_pending=true;
        } else if(cue_file(startup_path) || playlist_file(startup_path)){
            current=xstrdup(startup_path);
            startup_pending=true;
        }

        if(current){
            free(status);
            status=xstrdup(current);
        }
    }

    int selected=0,offset=0;
    timeout(200);
    keypad(stdscr,FALSE);

    for(;;){
        char *auto_status=check_auto_advance();

        erase();
        int height,width;
        getmaxyx(stdscr,height,width);

        char *title=NULL;
        Entries entries=make_entries(current,roots,&title);

        if(startup_pending){
            int wanted=-1;

            for(size_t i=0;i<entries.n;i++){
                Entry *entry=&entries.v[i];
                bool playable=entry->kind==EK_FILE ||
                              entry->kind==EK_CUETRACK ||
                              entry->kind==EK_PLTRACK;

                if(!playable)
                    continue;
                if(startup_target && strcmp(entry->path,startup_target)!=0)
                    continue;

                wanted=(int)i;
                break;
            }

            if(wanted>=0){
                selected=wanted;
                free(status);
                status=play_browser_entry(&entries,&entries.v[wanted]);
            } else {
                free(status);
                status=xstrdup("Could not find requested track");
            }

            startup_pending=false;
            free(startup_target);
            startup_target=NULL;
        }

        int visible=height-2;
        if(visible<1) visible=1;

        if(auto_status){
            free(status);
            status=auto_status;

            if(queue_mode)
                select_queue_entry(&entries,&selected);
            else
                select_playing_entry(&entries,&selected);
        }

        if(entries.n==0) selected=0;
        else {
            if(selected<0) selected=0;
            if(selected>=(int)entries.n) selected=(int)entries.n-1;
        }

        if(selected<offset) offset=selected;
        else if(selected>=offset+visible) offset=selected-visible+1;

        char *head=xasprintf("%s | Vol: %d%% | %s | %s",
                             title,
                             current_volume,
                             continuous?"Continuous":"Single",
                             random_play?"Random":"Ordered");
        draw_full_line(stdscr,0,head,width,NORMAL_ATTR|A_BOLD);
        free(head);

        for(int row=1,idx=offset; idx<(int)entries.n && row<height-1; row++,idx++){
            char *lab=entry_label(&entries.v[idx]);
            char *text=xasprintf("%s%s",idx==selected?"> ":"  ",lab);

            bool is_sel=idx==selected;
            bool is_play=entry_is_playing(&entries.v[idx]);
            int attr=is_play&&is_sel?PLAYING_SELECTED_ATTR:
                     is_play?PLAYING_ATTR:
                     is_sel?SELECTED_ATTR:
                     NORMAL_ATTR;

            draw_full_line(stdscr,row,text,width,attr);

            int qord=queue_order_for(&entries.v[idx]);
            if(qord>0){
                char qbuf[32];
                snprintf(qbuf,sizeof(qbuf),"%d",qord);
                int qx=(int)strlen(text)+1;
                if(qx<0) qx=0;
                if(qx>width-2) qx=width-2;

                int qattr=is_sel ? (COLOR_PAIR(4)|A_BOLD) : (COLOR_PAIR(3)|A_BOLD);
                attron(qattr);
                mvaddnstr(row,qx,qbuf,width-qx-1);
                attroff(qattr);
            }

            free(lab);
            free(text);
        }

        char *foot=xasprintf("Enter=open/play  p=playlist  Space=pause  Left=prev  Right=next  c=mode  r=random  PgUp/PgDn=volume  Backspace=up  q=quit | %s",status);
        draw_full_line(stdscr,height-1,foot,width,NORMAL_ATTR|A_DIM);
        free(foot);
        refresh();

        int key=getch();

        if(key==ERR){
            entries_free(&entries);
            free(title);
            continue;
        }

        int action_key=key;

        if(key==552 || key==567){
            /* Modified/workspace arrows: ignore completely.
               Also swallow the next plain arrow that GNOME may leak. */
            swallow_next_arrow = 1;
            entries_free(&entries);
            free(title);
            continue;
        }
        else if(key==27){
            int esc_action=escape_key_action();

            if(esc_action<0){
                stop_player();
                entries_free(&entries);
                free(title);
                break;
            }

            if(esc_action==0){
                entries_free(&entries);
                free(title);
                continue;
            }

            action_key=esc_action;
        }

        if(key=='q'){
            stop_player();
            entries_free(&entries);
            free(title);
            break;
        }
        else if(action_key==' '){
            free(status);
            status=toggle_pause();
        }
        else if(action_key=='c'){
            if(queue_mode){
                queue_clear_all();
                play_index=-1;
                free(status);
                status=xstrdup("Playlist cleared");
            } else {
                continuous=!continuous;
                free(status);
                status=xstrdup(continuous?"Mode: Continuous":"Mode: Single");
            }
        }
        else if(action_key=='p' || action_key=='P'){
            free(status);

            if(!entries.n){
                status=xstrdup("Nothing to queue");
            } else {
                Entry *e=&entries.v[selected];

                if(e->kind==EK_FILE || e->kind==EK_CUETRACK || e->kind==EK_PLTRACK){
                    queue_add(e);
                    status=xasprintf("Playlist: added as #%d", queue_order_for(e));
                } else {
                    status=xstrdup("Not a track");
                }
            }
        }
        else if(action_key=='r'){
            random_play=!random_play;
            free(status);
            status=xstrdup(random_play?"Random: On":"Random: Off");
        }
        else if(action_key==ACTION_ALT_LEFT || action_key==ACTION_ALT_RIGHT){
            /* Ubuntu workspace switching can leak Mod/Alt-arrow sequences.
               Do not let those touch playback, playlist, or highlight state. */
        }
        else if(action_key==KEY_UP || action_key=='k'){
            if(swallow_next_arrow && action_key==KEY_UP){
                swallow_next_arrow = 0;
            } else {
                selected=selected>0?selected-1:0;
            }
        }
        else if(action_key==KEY_DOWN || action_key=='j'){
            if(swallow_next_arrow && action_key==KEY_DOWN){
                swallow_next_arrow = 0;
            } else {
                selected=selected+1<(int)entries.n?selected+1:selected;
            }
        }
        else if(action_key==KEY_LEFT){
            if(swallow_next_arrow){
                swallow_next_arrow = 0;
                entries_free(&entries);
                free(title);
                continue;
            }
            free(status);

            if(queue_mode){
                int qp=queue_prev_index();
                if(qp>=0){
                    status=queue_play_item_at(qp);
                    select_queue_entry(&entries,&selected);
                } else {
                    status=xstrdup("Start of playlist");
                }
            } else {
                int prev=-1;
                if(history_pop(&prev)){
                    forward_push(play_index);
                    status=play_playlist_item_no_history(prev);
                    select_playing_entry(&entries,&selected);
                } else {
                    status=xstrdup("No song history");
                }
            }
        }
        else if(action_key==KEY_RIGHT){
            if(swallow_next_arrow){
                swallow_next_arrow = 0;
                entries_free(&entries);
                free(title);
                continue;
            }
            free(status);

            if(queue_mode){
                int qn = queue_play_pos < 0 ? 0 : queue_next_index();
                if(qn>=0){
                    status=queue_play_item_at(qn);
                    select_queue_entry(&entries,&selected);
                } else {
                    status=xstrdup("End of playlist");
                }
            } else {
                int next=-1;
                if(forward_pop(&next)){
                    history_push(play_index);
                    status=play_playlist_item_no_history(next);
                    select_playing_entry(&entries,&selected);
                } else {
                    next=next_play_index();
                    if(next>=0){
                        forward_clear();
                        status=play_playlist_item(next);
                        select_playing_entry(&entries,&selected);
                    } else {
                        status=xstrdup("No next song");
                    }
                }
            }
        }
        else if(action_key==KEY_BACKSPACE || action_key==127 || action_key==8){
            if(current){
                char *parent=dir_name_dup(current);
                free(current);
                current=parent;
                selected=0;
                offset=0;
            }
        }
        else if(action_key==KEY_PPAGE){
            current_volume=current_volume+5>MAX_VOLUME?MAX_VOLUME:current_volume+5;
            set_volume(current_volume);
            free(status);
            status=xasprintf("Volume: %d%%",current_volume);
        }
        else if(action_key==KEY_NPAGE){
            current_volume=current_volume-5<0?0:current_volume-5;
            set_volume(current_volume);
            free(status);
            status=xasprintf("Volume: %d%%",current_volume);
        }
        else if(action_key==10 || action_key==13){
            if(entries.n){
                Entry *e=&entries.v[selected];

                if(e->kind==EK_DIR || e->kind==EK_UP || e->kind==EK_CUE || e->kind==EK_PLAYLISTFILE){
                    free(current);
                    current=xstrdup(e->path);
                    selected=0;
                    offset=0;
                    free(status);
                    status=xstrdup(current);
                }
                else if(e->kind==EK_FILE || e->kind==EK_CUETRACK || e->kind==EK_PLTRACK){
                    free(status);
                    status=play_browser_entry(&entries,e);
                }
            }
        }

        entries_free(&entries);
        free(title);
    }

    free(current);
    free(startup_target);
    free(status);
}

int main(int argc, char **argv){
    char *startup_path=NULL;

    if(argc>2){
        fprintf(stderr,"usage: simpleflac [audio-file|cue|playlist|directory]\n");
        return 2;
    }

    if(argc==2){
        startup_path=realpath(argv[1],NULL);
        if(!startup_path){
            fprintf(stderr,"simpleflac: cannot open %s: %s\n",argv[1],strerror(errno));
            return 1;
        }
        if(!st_is_dir(startup_path) && !playable_file(startup_path) &&
           !cue_file(startup_path) && !playlist_file(startup_path)){
            fprintf(stderr,"simpleflac: unsupported path: %s\n",startup_path);
            free(startup_path);
            return 1;
        }
    }

    setlocale(LC_ALL, "");
    init_mpv_socket_path();
    srand((unsigned)time(NULL));
    StrList roots=choose_start_roots();

    if(!startup_path && !roots.n){
        fprintf(stderr,"simpleflac: no music roots found\n");
        strlist_free(&roots);
        return 1;
    }

    initscr();
    cbreak();
    noecho();
    start_color();
    int black = COLORS>=256 ? 16 : COLOR_BLACK;
    int white = COLORS>=256 ? 15 : COLOR_WHITE;
    init_pair(1,white,black);
    init_pair(2,black,white);
    init_pair(3,COLOR_YELLOW,black);
    init_pair(4,COLOR_YELLOW,white);
    NORMAL_ATTR=COLOR_PAIR(1);
    SELECTED_ATTR=COLOR_PAIR(2);
    PLAYING_ATTR=COLOR_PAIR(3)|A_BOLD;
    PLAYING_SELECTED_ATTR=COLOR_PAIR(4)|A_BOLD;
    bkgd(' '|NORMAL_ATTR);
    curs_set(0);

    browser(&roots,startup_path);
    stop_player();
    strlist_free(&roots);
    entries_free(&playlist);
    free(play_history);
    free(play_forward);
    free(startup_path);
    endwin();
    return 0;
}
