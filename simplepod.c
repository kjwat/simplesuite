
#include <ncurses.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <locale.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <sys/time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_SHOWS 50
#define MAX_EPS 200
#define MAX_FIELD 1024
#define MAX_URL 2048
#define RSS_CACHE_TTL 1800
#define MAX_VOLUME 130
#define RESPONSE_LIMIT (16u * 1024u * 1024u)

typedef struct { char *data; size_t size; } Buf;
typedef struct { char title[MAX_FIELD], artist[MAX_FIELD], feed[MAX_URL]; } Show;
typedef struct { char title[MAX_FIELD], audio[MAX_URL]; } Ep;

static Show shows[MAX_SHOWS];
static Ep eps[MAX_EPS];
static int show_count=0, ep_count=0, sel=0, mode=0, editing=0, paused=0, playing_ep=-1, current_volume=100, last_show_sel=0;
static int list_searching=0, list_search_len=0;
static int list_top=0, last_show_top=0;
static char query[256]="";
static char list_query[256]="";
static char status[512]="Press s to search Apple Podcasts.";
static long status_flash_until=0;
static char playing_audio[MAX_URL]="";
static double play_pos=0, play_dur=0;
static double selected_resume_pos=0;
static pid_t mpv_pid=-1;
static char mpv_socket[sizeof(((struct sockaddr_un *)0)->sun_path)] = "/tmp/simplepod-mpv.sock";

static void mpv_command(const char *json);
static void update_progress(void);
static void fmt_time(double sec, char *out, size_t n);
static unsigned long hash_url(const char *s);

static size_t write_cb(void *ptr,size_t size,size_t nmemb,void *ud){
    if(size && nmemb > ((size_t)-1) / size) return 0;
    size_t total=size*nmemb; Buf*b=ud;
    if(total > RESPONSE_LIMIT || b->size > RESPONSE_LIMIT - total) return 0;
    char*p=realloc(b->data,b->size+total+1);
    if(!p)return 0;
    b->data=p; memcpy(b->data+b->size,ptr,total);
    b->size+=total; b->data[b->size]=0;
    return total;
}

static void init_mpv_socket_path(void){
    struct sockaddr_un addr;
    const char *runtime=getenv("XDG_RUNTIME_DIR");
    int n=-1;
    if(runtime&&*runtime)
        n=snprintf(mpv_socket,sizeof(mpv_socket),"%s/simplepod-mpv-%ld.sock",runtime,(long)getpid());
    if(n<0||(size_t)n>=sizeof(mpv_socket)||(size_t)n>=sizeof(addr.sun_path))
        snprintf(mpv_socket,sizeof(mpv_socket),"/tmp/simplepod-mpv-%ld.sock",(long)getpid());
    unlink(mpv_socket);
}

static char *fetch_url(const char *url){
    CURL*c=curl_easy_init(); if(!c)return NULL;
    Buf b={0};
    curl_easy_setopt(c,CURLOPT_URL,url);
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,write_cb);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,&b);
    curl_easy_setopt(c,CURLOPT_USERAGENT,"simplepod/0.1");
    curl_easy_setopt(c,CURLOPT_TIMEOUT,25L);
    CURLcode r=curl_easy_perform(c);
    curl_easy_cleanup(c);
    if(r!=CURLE_OK){ free(b.data); return NULL; }
    return b.data;
}

static void clean(char*s){
    char*w=s;
    for(char*r=s;*r;r++){
        if(*r=='\\'&&r[1]){ r++; if(*r=='n'||*r=='t'||*r=='r')*w++=' '; else *w++=*r; }
        else if(!strncmp(r,"&amp;",5)){*w++='&';r+=4;}
        else if(!strncmp(r,"&quot;",6)){*w++='"';r+=5;}
        else if(!strncmp(r,"&#39;",5)){*w++='\'';r+=4;}
        else if(!strncmp(r,"<![CDATA[",9)){r+=8;}
        else if(!strncmp(r,"]]>",3)){r+=2;}
        else *w++=*r;
    }
    *w=0;
}

static int json_field(const char*src,const char*key,char*out,size_t outsz){
    char pat[128]; snprintf(pat,sizeof(pat),"\"%s\":\"",key);
    char*p=strstr((char*)src,pat); if(!p)return 0;
    p+=strlen(pat); size_t i=0;
    while(*p&&i+1<outsz){ if(*p=='"'&&p[-1]!='\\')break; out[i++]=*p++; }
    out[i]=0; clean(out); return 1;
}

static void search_apple(const char*term){
    show_count=0; ep_count=0; mode=0; sel=0; list_top=0; last_show_top=0;
    list_searching=0; list_query[0]=0; list_search_len=0;
    if(!term[0]){ snprintf(status,sizeof(status),"Empty search."); return; }

    CURL*c=curl_easy_init(); if(!c)return;
    char*esc=curl_easy_escape(c,term,0);
    char url[MAX_URL];
    snprintf(url,sizeof(url),"https://itunes.apple.com/search?media=podcast&limit=50&term=%s",esc?esc:term);
    if(esc)
        curl_free(esc);
    curl_easy_cleanup(c);

    snprintf(status,sizeof(status),"Searching...");
    char*json=fetch_url(url);
    if(!json){ snprintf(status,sizeof(status),"Search failed."); return; }

    const char*p=json;
    while((p=strstr(p,"\"wrapperType\":\"track\""))&&show_count<MAX_SHOWS){
        const char*next=strstr(p+1,"\"wrapperType\":\"track\"");
        size_t len=next?(size_t)(next-p):strlen(p);
        char*chunk=malloc(len+1); if(!chunk)break;
        memcpy(chunk,p,len); chunk[len]=0;

        Show*s=&shows[show_count]; memset(s,0,sizeof(*s));
        json_field(chunk,"collectionName",s->title,sizeof(s->title));
        json_field(chunk,"artistName",s->artist,sizeof(s->artist));
        json_field(chunk,"feedUrl",s->feed,sizeof(s->feed));
        if(s->title[0]&&s->feed[0])show_count++;

        free(chunk); p++;
    }
    free(json);
    snprintf(status,sizeof(status),"%d shows found.",show_count);
}

static int tag_text(const char*src,const char*tag,char*out,size_t outsz){
    char open[64],close[64];
    snprintf(open,sizeof(open),"<%s",tag);
    snprintf(close,sizeof(close),"</%s>",tag);
    char*p=strstr((char*)src,open); if(!p)return 0;
    p=strchr(p,'>'); if(!p)return 0; p++;
    char*e=strstr(p,close); if(!e)return 0;
    size_t len=(size_t)(e-p); if(len>=outsz)len=outsz-1;
    memcpy(out,p,len); out[len]=0; clean(out); return 1;
}

static int attr_url(const char*src,char*out,size_t outsz){
    char*p=strstr((char*)src,"<enclosure"); if(!p)return 0;
    char*u=strstr(p,"url=\""); if(!u)u=strstr(p,"url='"); if(!u)return 0;
    char q=u[4]; u+=5; size_t i=0;
    while(*u&&*u!=q&&i+1<outsz)out[i++]=*u++;
    out[i]=0; clean(out); return 1;
}


static int snprintf_ok(int n, size_t size){
    return n >= 0 && (size_t)n < size;
}

static int ensure_dir(const char *path){
    struct stat st;
    if(!path || !*path)return 0;
    if(stat(path,&st)==0)return S_ISDIR(st.st_mode);
    if(mkdir(path,0700)==0)return 1;
    return errno==EEXIST && stat(path,&st)==0 && S_ISDIR(st.st_mode);
}

static int mkdirs(const char *path){
    char tmp[PATH_MAX];
    size_t len;
    if(!path || !*path)return 0;
    if(!snprintf_ok(snprintf(tmp,sizeof(tmp),"%s",path),sizeof(tmp)))return 0;
    len=strlen(tmp);
    while(len>1 && tmp[len-1]=='/')tmp[--len]=0;
    for(char *p=tmp+1;*p;p++){
        if(*p=='/'){
            *p=0;
            if(!ensure_dir(tmp))return 0;
            *p='/';
        }
    }
    return ensure_dir(tmp);
}

static int home_path(char *out, size_t n, const char *suffix){
    const char *home=getenv("HOME");
    if(!out || n==0)return 0;
    out[0]=0;
    if(!home || !*home || !suffix || !*suffix)return 0;
    return snprintf_ok(snprintf(out,n,"%s/%s",home,suffix),n);
}

static int regular_file(const char *path){
    struct stat st;
    return path && *path && stat(path,&st)==0 && S_ISREG(st.st_mode);
}

static int copy_file_for_migration(const char *src, const char *dst){
    FILE *in=fopen(src,"rb");
    FILE *out;
    char buf[8192];
    size_t got;
    int ok=1;
    if(!in)return 0;
    out=fopen(dst,"wbx");
    if(!out){
        fclose(in);
        return 0;
    }
    while((got=fread(buf,1,sizeof(buf),in))>0){
        if(fwrite(buf,1,got,out)!=got){
            ok=0;
            break;
        }
    }
    if(ferror(in))ok=0;
    fclose(in);
    if(fclose(out)!=0)ok=0;
    if(!ok){
        unlink(dst);
        return 0;
    }
    return 1;
}

static void migrate_file_if_safe(const char *src, const char *dst){
    char parent[PATH_MAX];
    char *slash;
    if(!regular_file(src) || regular_file(dst))return;
    if(!snprintf_ok(snprintf(parent,sizeof(parent),"%s",dst),sizeof(parent)))return;
    slash=strrchr(parent,'/');
    if(!slash)return;
    *slash=0;
    if(!mkdirs(parent))return;
    if(rename(src,dst)==0)return;
    if(copy_file_for_migration(src,dst))unlink(src);
}

static void migrate_legacy_cache_dir(const char *new_cache){
    static int attempted=0;
    char old_cache[PATH_MAX];
    DIR *dir;
    if(attempted)return;
    attempted=1;
    if(!home_path(old_cache,sizeof(old_cache),".config/simplepod/cache"))return;
    dir=opendir(old_cache);
    if(!dir)return;
    struct dirent *de;
    while((de=readdir(dir))){
        char src[PATH_MAX];
        char dst[PATH_MAX];
        if(!strcmp(de->d_name,".") || !strcmp(de->d_name,".."))continue;
        if(!snprintf_ok(snprintf(src,sizeof(src),"%s/%s",old_cache,de->d_name),sizeof(src)))continue;
        if(!regular_file(src))continue;
        if(!snprintf_ok(snprintf(dst,sizeof(dst),"%s/%s",new_cache,de->d_name),sizeof(dst)))continue;
        migrate_file_if_safe(src,dst);
    }
    closedir(dir);
}

static int cache_dir(char *out, size_t n){
    char base[PATH_MAX];
    const char *xdg=getenv("XDG_CACHE_HOME");
    if(xdg && *xdg && xdg[0]=='/')
        snprintf(base,sizeof(base),"%s",xdg);
    else if(!home_path(base,sizeof(base),".cache"))
        return 0;
    if(!snprintf_ok(snprintf(out,n,"%s/simplepod/cache",base),n))return 0;
    if(!mkdirs(out))return 0;
    migrate_legacy_cache_dir(out);
    return 1;
}

static int cache_file_path(const char *url, char *out, size_t n){
    char dir[PATH_MAX];
    if(!cache_dir(dir,sizeof(dir)))return 0;
    return snprintf_ok(snprintf(out,n,"%s/%lu.xml",dir,hash_url(url)),n);
}

static int state_dir(char *out, size_t n){
    if(!home_path(out,n,".local/state/simplepod"))return 0;
    return mkdirs(out);
}

static void migrate_legacy_resume(const char *new_path){
    static int attempted=0;
    char old_path[PATH_MAX];
    if(attempted)return;
    attempted=1;
    if(!home_path(old_path,sizeof(old_path),".config/simplepod/resume.txt"))return;
    migrate_file_if_safe(old_path,new_path);
}

static int resume_path(char *out, size_t n){
    char dir[PATH_MAX];
    if(!state_dir(dir,sizeof(dir)))return 0;
    if(!snprintf_ok(snprintf(out,n,"%s/resume.txt",dir),n))return 0;
    migrate_legacy_resume(out);
    return 1;
}

static unsigned long hash_url(const char *s){
    unsigned long h=5381;
    int c;
    while((c=*s++)) h=((h<<5)+h)+(unsigned char)c;
    return h;
}

static int read_file(const char *path, char **out){
    FILE *f=fopen(path,"rb");
    if(!f)return 0;
    fseek(f,0,SEEK_END);
    long n=ftell(f);
    rewind(f);
    if(n<0){ fclose(f); return 0; }

    char *buf=malloc((size_t)n+1);
    if(!buf){ fclose(f); return 0; }

    size_t got=fread(buf,1,(size_t)n,f);
    fclose(f);
    buf[got]=0;
    *out=buf;
    return 1;
}

static void write_file(const char *path, const char *data){
    FILE *f=fopen(path,"wb");
    if(!f)return;
    fwrite(data,1,strlen(data),f);
    fclose(f);
}


static double resume_get(const char *url){
    char path[PATH_MAX];
    if(!resume_path(path,sizeof(path))) return 0;

    FILE *f=fopen(path,"r");
    if(!f) return 0;

    char line[4096];
    while(fgets(line,sizeof(line),f)){
        char *tab=strchr(line,'\t');
        if(!tab) continue;
        *tab=0;

        if(strcmp(line,url)==0){
            double v=atof(tab+1);
            fclose(f);
            return v >= 60 ? v : 0;
        }
    }

    fclose(f);
    return 0;
}

static void resume_set(const char *url, double pos){
    if(!url || !*url || pos < 60) return;

    char path[PATH_MAX], tmp[PATH_MAX];
    if(!resume_path(path,sizeof(path))) return;
    if(!snprintf_ok(snprintf(tmp,sizeof(tmp),"%s.tmp.XXXXXX",path),sizeof(tmp))) return;

    FILE *in=fopen(path,"r");
    int fd=mkstemp(tmp);
    if(fd<0){
        if(in) fclose(in);
        return;
    }
    FILE *out=fdopen(fd,"w");
    if(!out){
        close(fd);
        unlink(tmp);
        if(in) fclose(in);
        return;
    }

    char line[4096];
    int found=0;

    if(in){
        while(fgets(line,sizeof(line),in)){
            char copy[4096];
            snprintf(copy,sizeof(copy),"%s",line);

            char *tab=strchr(copy,'\t');
            if(!tab){
                fputs(line,out);
                continue;
            }

            *tab=0;

            if(strcmp(copy,url)==0){
                fprintf(out,"%s\t%.0f\n",url,pos);
                found=1;
            } else {
                fputs(line,out);
            }
        }
        fclose(in);
    }

    if(!found)
        fprintf(out,"%s\t%.0f\n",url,pos);

    if(fclose(out)!=0){
        unlink(tmp);
        return;
    }
    if(rename(tmp,path)!=0)
        unlink(tmp);
}

static void save_current_resume(void){
    if(playing_audio[0] && play_pos >= 60)
        resume_set(playing_audio, play_pos);
}

__attribute__((unused)) static void seek_absolute(double sec){
    char buf[160];
    snprintf(buf,sizeof(buf),
             "{\"command\":[\"seek\",%.0f,\"absolute\"]}",
             sec);
    mpv_command(buf);
}

static char *fetch_feed_cached(const char *url){
    char path[PATH_MAX];
    if(!cache_file_path(url,path,sizeof(path)))
        return fetch_url(url);

    struct stat st;
    time_t now=time(NULL);

    if(stat(path,&st)==0 && now-st.st_mtime < RSS_CACHE_TTL){
        char *cached=NULL;
        if(read_file(path,&cached)){
            snprintf(status,sizeof(status),"Loaded cached feed.");
            return cached;
        }
    }

    char *fresh=fetch_url(url);
    if(fresh){
        write_file(path,fresh);
        snprintf(status,sizeof(status),"Downloaded and cached feed.");
    }
    return fresh;
}

static void load_feed(const char*feed){
    ep_count=0; sel=0; list_top=0;
    list_searching=0; list_query[0]=0; list_search_len=0;
    char*xml=fetch_feed_cached(feed);
    if(!xml){snprintf(status,sizeof(status),"Feed failed.");return;}

    const char*p=xml;
    while((p=strstr(p,"<item"))&&ep_count<MAX_EPS){
        const char*next=strstr(p+1,"<item");
        size_t len=next?(size_t)(next-p):strlen(p);
        char*chunk=malloc(len+1); if(!chunk)break;
        memcpy(chunk,p,len); chunk[len]=0;

        Ep*e=&eps[ep_count]; memset(e,0,sizeof(*e));
        tag_text(chunk,"title",e->title,sizeof(e->title));
        attr_url(chunk,e->audio,sizeof(e->audio));
        if(e->title[0]&&e->audio[0])ep_count++;

        free(chunk); p++;
    }
    free(xml); mode=1;
    snprintf(status,sizeof(status),"%d episodes.",ep_count);
}

static void mpv_command(const char *json)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(fd < 0) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t len = strlen(mpv_socket);
    if(len >= sizeof(addr.sun_path)){
        close(fd);
        return;
    }
    memcpy(addr.sun_path, mpv_socket, len + 1);

    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0){
        ssize_t ignored;
        ignored = write(fd, json, strlen(json));
        (void)ignored;
        ignored = write(fd, "\n", 1);
        (void)ignored;
    }

    close(fd);
}

static void set_volume(int volume)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"command\":[\"set_property\",\"volume\",%d]}",
             volume);
    mpv_command(buf);
}

static void seek_relative(int seconds)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"command\":[\"seek\",%d,\"relative\"]}",
             seconds);
    mpv_command(buf);
}

static void play_url(const char*url){
    update_progress();
    save_current_resume();

    if(mpv_pid>0){ kill(mpv_pid,SIGTERM); waitpid(mpv_pid,NULL,WNOHANG); }
    unlink(mpv_socket);
    paused=0;
    mpv_pid=fork();
    if(mpv_pid==0){
        char sockarg[sizeof("--input-ipc-server=") + sizeof(mpv_socket)];
        snprintf(sockarg,sizeof(sockarg),"--input-ipc-server=%s",mpv_socket);
        if(!freopen("/dev/null","w",stdout)) _exit(127);
        if(!freopen("/dev/null","w",stderr)) _exit(127);
        execlp("mpv","mpv","--no-video","--force-window=no","--terminal=no","--quiet",sockarg,"--cache=yes",url,(char*)NULL);
        _exit(127);
    }
    if(mode==1){ playing_ep=sel; snprintf(playing_audio,sizeof(playing_audio),"%s",url); }
    usleep(150000);
    set_volume(current_volume);
    snprintf(status,sizeof(status),"Playing. Vol: %d%%", current_volume);
}


static void play_resume_url(const char *url, double pos){
    update_progress();
    save_current_resume();

    if(mpv_pid>0){
        kill(mpv_pid,SIGTERM);
        waitpid(mpv_pid,NULL,WNOHANG);
    }

    unlink(mpv_socket);
    paused=0;
    play_pos=pos;
    play_dur=0;

    char startarg[64];
    snprintf(startarg,sizeof(startarg),"--start=%.0f",pos);

    mpv_pid=fork();
    if(mpv_pid==0){
        char sockarg[sizeof("--input-ipc-server=") + sizeof(mpv_socket)];
        snprintf(sockarg,sizeof(sockarg),"--input-ipc-server=%s",mpv_socket);
        if(!freopen("/dev/null","w",stdout)) _exit(127);
        if(!freopen("/dev/null","w",stderr)) _exit(127);
        execlp("mpv","mpv",
               "--no-video",
               "--force-window=no",
               "--terminal=no",
               "--quiet",
               sockarg,
               "--cache=yes",
               startarg,
               url,
               (char*)NULL);
        _exit(127);
    }

    if(mode==1){
        playing_ep=sel;
        snprintf(playing_audio,sizeof(playing_audio),"%s",url);
    }

    usleep(150000);
    set_volume(current_volume);

    char t[32];
    fmt_time(pos,t,sizeof(t));
    snprintf(status,sizeof(status),"Resuming at %s",t);
}

static void toggle_pause(void){
    if(mpv_pid<=0)return;

    paused = !paused;

    char buf[128];
    snprintf(buf,sizeof(buf),
             "{\"command\":[\"set_property\",\"pause\",%s]}",
             paused ? "true" : "false");

    mpv_command(buf);
    snprintf(status,sizeof(status),
             paused ? "Paused." : "Playing.");
}


static double mpv_get_double_property(const char *prop){
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr,0,sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t len = strlen(mpv_socket);
    if(len >= sizeof(addr.sun_path)){
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, mpv_socket, len + 1);

    if(connect(fd,(struct sockaddr*)&addr,sizeof(addr)) != 0){
        close(fd);
        return -1;
    }

    char cmd[256];
    snprintf(cmd,sizeof(cmd),
             "{\"command\":[\"get_property\",\"%s\"]}\n", prop);
    {
        ssize_t ignored = write(fd,cmd,strlen(cmd));
        (void)ignored;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd,&rfds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 30000;

    char buf[512]={0};
    if(select(fd+1,&rfds,NULL,NULL,&tv) > 0){
        ssize_t nread = read(fd,buf,sizeof(buf)-1);
        if(nread > 0) buf[nread] = '\0';
    }

    close(fd);

    char *d = strstr(buf,"\"data\":");
    if(!d) return -1;
    d += 7;
    if(!strncmp(d,"null",4)) return -1;
    return atof(d);
}

static void update_progress(void){
    if(mpv_pid <= 0) return;

    double p = mpv_get_double_property("time-pos");
    double d = mpv_get_double_property("duration");

    if(p >= 0) play_pos = p;
    if(d > 0) play_dur = d;
}

static void fmt_time(double sec, char *out, size_t n){
    if(sec < 0) sec = 0;
    int s = (int)sec;
    int h = s / 3600;
    int m = (s % 3600) / 60;
    int r = s % 60;

    if(h > 0) snprintf(out,n,"%d:%02d:%02d",h,m,r);
    else snprintf(out,n,"%d:%02d",m,r);
}

static void draw_progress_bar(void){
    if(mpv_pid <= 0 || play_dur <= 0) return;

    int width = COLS - 2;
    if(width < 20) return;
    if(width > 60) width = 60;

    double frac = play_pos / play_dur;
    if(frac < 0) frac = 0;
    if(frac > 1) frac = 1;

    int filled = (int)(frac * width);

    char cur[32], total[32];
    fmt_time(play_pos,cur,sizeof(cur));
    fmt_time(play_dur,total,sizeof(total));

    move(6,0);
    clrtoeol();

    printw("[");
    for(int i=0;i<width;i++) addch(i < filled ? '=' : '-');
    printw("] %s / %s", cur, total);
}


static long now_ms(void){
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return tv.tv_sec*1000L + tv.tv_usec/1000L;
}

static void flash_status(const char *msg, int ms){
    snprintf(status,sizeof(status),"%s",msg);
    status_flash_until = now_ms() + ms;
}

__attribute__((unused)) static void update_status_flash(void){
    if(status_flash_until && now_ms() >= status_flash_until){
        status_flash_until = 0;
        if(show_count==0 && ep_count==0 && mode==0 && !editing)
            flash_status("Search cancelled.", 1000);
    }
}


static int current_list_count(void){
    return mode==0 ? show_count : ep_count;
}

static int contains_icase(const char *haystack, const char *needle){
    if(!needle || !needle[0]) return 1;
    if(!haystack) return 0;

    size_t nlen = strlen(needle);
    if(nlen == 0) return 1;

    for(const char *h = haystack; *h; h++){
        size_t i = 0;
        while(i < nlen && h[i] &&
              tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])){
            i++;
        }
        if(i == nlen) return 1;
    }

    return 0;
}

static int list_item_matches(int idx, const char *needle){
    if(mode==0){
        if(idx < 0 || idx >= show_count) return 0;
        return contains_icase(shows[idx].title, needle) ||
               contains_icase(shows[idx].artist, needle);
    }

    if(idx < 0 || idx >= ep_count) return 0;
    return contains_icase(eps[idx].title, needle);
}

static void start_list_search(void){
    if(current_list_count() <= 0){
        snprintf(status,sizeof(status),"Nothing to search.");
        return;
    }

    list_searching = 1;
    list_query[0] = 0;
    list_search_len = 0;
    snprintf(status,sizeof(status),"Type a word, Enter to find, Esc to cancel.");
}

static int list_search_step(int direction, int include_current){
    int count = current_list_count();

    if(count <= 0){
        snprintf(status,sizeof(status),"Nothing to search.");
        return 0;
    }

    if(list_search_len <= 0){
        snprintf(status,sizeof(status),"No find term. Press f to find.");
        return 0;
    }

    for(int step = include_current ? 0 : 1; step < count; step++){
        int idx = sel + (direction * step);

        while(idx < 0)
            idx += count;

        idx %= count;

        if(list_item_matches(idx, list_query)){
            sel = idx;
            snprintf(status,sizeof(status),"Found: %s  (n/N for next/prev)", list_query);
            return 1;
        }
    }

    snprintf(status,sizeof(status),"Not found: %s", list_query);
    return 0;
}

static void execute_list_search(void){
    if(list_search_len <= 0){
        snprintf(status,sizeof(status),"Find cancelled.");
        return;
    }

    list_search_step(1, 1);
}

static void handle_list_search_input(int ch){
    if(ch == 27 || ch == 7){
        list_searching = 0;
        list_query[0] = 0;
        list_search_len = 0;
        snprintf(status,sizeof(status),"Find cancelled.");
        return;
    }

    if(ch=='\n' || ch=='\r' || ch==KEY_ENTER){
        list_searching = 0;
        execute_list_search();
        return;
    }

    if(ch==KEY_BACKSPACE || ch==127 || ch==8){
        if(list_search_len > 0){
            list_search_len--;
            list_query[list_search_len] = 0;
        }
        return;
    }

    if(ch >= 32 && ch <= 126 && list_search_len < (int)sizeof(list_query)-1){
        list_query[list_search_len++] = (char)ch;
        list_query[list_search_len] = 0;
        return;
    }
}


static int list_visible_rows(void){
    int rows = LINES - 9;
    return rows < 1 ? 1 : rows;
}

static void list_clamp_view(void){
    int count = current_list_count();
    int rows = list_visible_rows();

    if(count <= 0){
        sel = 0;
        list_top = 0;
        return;
    }

    if(sel < 0) sel = 0;
    if(sel >= count) sel = count - 1;

    int max_top = count - rows;
    if(max_top < 0) max_top = 0;

    if(list_top < 0) list_top = 0;
    if(list_top > max_top) list_top = max_top;

    if(sel < list_top)
        list_top = sel;
    else if(sel >= list_top + rows)
        list_top = sel - rows + 1;

    if(list_top < 0) list_top = 0;
    if(list_top > max_top) list_top = max_top;
}

static void list_move_selection(int delta){
    int count = current_list_count();
    int rows = list_visible_rows();

    if(count <= 0){
        sel = 0;
        list_top = 0;
        return;
    }

    sel += delta;

    if(sel < 0) sel = 0;
    if(sel >= count) sel = count - 1;

    int max_top = count - rows;
    if(max_top < 0) max_top = 0;

    if(list_top < 0) list_top = 0;
    if(list_top > max_top) list_top = max_top;

    /*
       Important bit:
       moving up only scrolls the viewport once the bar would leave
       the top. So when you come off the bottom, the bar climbs upward
       instead of dragging the whole list with it.
    */
    if(delta < 0){
        if(sel < list_top)
            list_top = sel;
    } else if(delta > 0){
        if(sel >= list_top + rows)
            list_top = sel - rows + 1;
    }

    if(list_top < 0) list_top = 0;
    if(list_top > max_top) list_top = max_top;
}

static void draw_screen(void){
    static int last_lines=0, last_cols=0;

    if(last_lines != LINES || last_cols != COLS){
        erase();
        last_lines = LINES;
        last_cols = COLS;
    }

    int w = COLS - 1;
    if(w < 1) w = 1;

    move(0,0); clrtoeol();
    mvprintw(0,0,"simplepod");

    move(1,0); clrtoeol();
    mvprintw(1,0,"s search | f find | n/N next/prev | Enter play | r resume | Space pause | PgUp/PgDn volume | b back | q quit");

    move(2,0); clrtoeol();
    if(list_searching || list_query[0])
        mvprintw(2,0,"Find: %s%s",list_query,list_searching?"_":"");

    move(3,0); clrtoeol();
    mvprintw(3,0,"Search: %s%s",query,editing?"_":"");

    move(4,0); clrtoeol();
    mvprintw(4,0,"%s",status);

    move(5,0); clrtoeol();
    selected_resume_pos = 0;
    if(mode==1 && ep_count>0 && sel>=0 && sel<ep_count){
        selected_resume_pos = resume_get(eps[sel].audio);
        if(selected_resume_pos >= 60){
            char rt[32];
            fmt_time(selected_resume_pos,rt,sizeof(rt));
            mvprintw(5,0,"Resume: %s available - press r",rt);
        } else {
            mvprintw(5,0,"Resume: none");
        }
    }

    move(6,0); clrtoeol();
    update_progress();
    draw_progress_bar();

    move(7,0); clrtoeol();

    int count = current_list_count();
    int rows = list_visible_rows();

    list_clamp_view();

    int start = list_top;
    int drawn = 0;

    for(int i=0; i<rows && start+i<count; i++){
        int idx = start + i;
        int y = i + 8;
        int is_playing = (mode==1 && playing_audio[0] && strcmp(eps[idx].audio, playing_audio)==0);

        char line[MAX_FIELD + MAX_FIELD + 8];

        if(mode==0)
            snprintf(line,sizeof(line),"%-45.45s  %-30.30s",shows[idx].title,shows[idx].artist);
        else
            snprintf(line,sizeof(line),"%s",eps[idx].title);

        if(is_playing && idx==sel)
            attron(COLOR_PAIR(4) | A_BOLD);
        else if(is_playing)
            attron(COLOR_PAIR(3) | A_BOLD);
        else if(idx==sel)
            attron(A_REVERSE);

        mvprintw(y,0,"%-*.*s",w,w,line);

        if(is_playing && idx==sel)
            attroff(COLOR_PAIR(4) | A_BOLD);
        else if(is_playing)
            attroff(COLOR_PAIR(3) | A_BOLD);
        else if(idx==sel)
            attroff(A_REVERSE);

        drawn = i + 1;
    }

    for(int i=drawn; i<rows; i++){
        move(i+8,0);
        clrtoeol();
    }

    wnoutrefresh(stdscr);
    doupdate();
}

int main(void){
    setlocale(LC_ALL,"");
    init_mpv_socket_path();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    initscr(); cbreak(); noecho(); keypad(stdscr,TRUE);
    start_color();
    use_default_colors();
    init_pair(3, COLORS >= 256 ? 226 : COLOR_YELLOW, -1);
    init_pair(4, COLORS >= 256 ? 226 : COLOR_YELLOW, COLOR_WHITE);
    set_escdelay(25); notimeout(stdscr, FALSE); curs_set(0); leaveok(stdscr,TRUE); timeout(500);

    int need_redraw=1;

    while(1){
        if(need_redraw || mpv_pid>0)
            draw_screen();

        need_redraw=0;

        int ch=getch();
        if(ch==ERR)
            continue;

        need_redraw=1;

        int count=mode==0?show_count:ep_count;

        if(editing){
            int len=strlen(query);

            if(ch==27 || ch==7){
                editing=0;
                query[0]=0;
                snprintf(status,sizeof(status),"Search cancelled.");
                draw_screen();
                napms(1000);
                snprintf(status,sizeof(status),"Press s to search Apple Podcasts.");
                continue;
            }

            if(ch=='\n' || ch=='\r' || ch==KEY_ENTER){
                editing=0;
                search_apple(query);
                continue;
            }

            if(ch==KEY_BACKSPACE || ch==127 || ch==8){
                if(len>0) query[len-1]=0;
                continue;
            }

            if(ch >= 32 && ch <= 126 && len < 255){
                query[len]=ch;
                query[len+1]=0;
                continue;
            }

            continue;
        }

        if(list_searching){
            handle_list_search_input(ch);
            continue;
        }

        if(ch=='q')break;
        else if(ch==KEY_UP)list_move_selection(-1);
        else if(ch==KEY_DOWN)list_move_selection(1);
        else if(ch==KEY_RIGHT){
            seek_relative(30);
            snprintf(status,sizeof(status),"Seek +30 sec");
        }
        else if(ch==KEY_LEFT){
            seek_relative(-15);
            snprintf(status,sizeof(status),"Seek -15 sec");
        }
        else if(ch==KEY_PPAGE){
            current_volume += 5;
            if(current_volume > MAX_VOLUME) current_volume = MAX_VOLUME;
            set_volume(current_volume);
            snprintf(status,sizeof(status),"Volume: %d%%", current_volume);
        }
        else if(ch==KEY_NPAGE){
            current_volume -= 5;
            if(current_volume < 0) current_volume = 0;
            set_volume(current_volume);
            snprintf(status,sizeof(status),"Volume: %d%%", current_volume);
        }
        else if(ch=='s'){ editing=1; list_searching=0; query[0]=0; sel=0; list_top=0; }
        else if(ch=='f')start_list_search();
        else if(ch=='n')list_search_step(1, 0);
        else if(ch=='N')list_search_step(-1, 0);
        else if(ch=='b' || ch==KEY_BACKSPACE || ch==127 || ch==8){
            if(mode==1){
                mode=0;
                list_searching=0;
                list_query[0]=0;
                list_search_len=0;
                sel=last_show_sel;
                list_top=last_show_top;
                if(sel<0)sel=0;
                if(sel>=show_count)sel=show_count-1;
                snprintf(status,sizeof(status),"Back to shows.");
            } else {
                show_count=0;
                ep_count=0;
                sel=0;
                mode=0;
                list_top=0;
                last_show_top=0;
                list_searching=0;
                list_query[0]=0;
                list_search_len=0;
                snprintf(status,sizeof(status),"Press s to search Apple Podcasts.");
            }
        }
        else if(ch=='r' && mode==1 && count>0){
            double pos = resume_get(eps[sel].audio);
            if(pos >= 60) play_resume_url(eps[sel].audio, pos);
            else snprintf(status,sizeof(status),"No resume data for this episode.");
        }
        else if(ch==' ')toggle_pause();
        else if((ch=='\n'||ch=='\r'||ch==KEY_ENTER)&&count>0){
            if(mode==0){ last_show_sel=sel; last_show_top=list_top; load_feed(shows[sel].feed); }
            else play_url(eps[sel].audio);
        }
    }

    update_progress();
    save_current_resume();

    endwin();
    if(mpv_pid>0)kill(mpv_pid,SIGTERM);
    unlink(mpv_socket);
    curl_global_cleanup();
    return 0;
}
