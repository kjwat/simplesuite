
#include <ncurses.h>
#include <curl/curl.h>
#include <openssl/sha.h>
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
#include <pthread.h>
#include <stdatomic.h>
#include "simpleui.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_SEARCH_RESULTS 1024
#define MAX_ITUNES_SHOWS 200
#define MAX_ITUNES_EPISODES 200
#define MAX_EPS 200
#define MAX_FIELD 1024
#define MAX_URL 2048
#define MAX_DEEP_RESULTS 512
#define MAX_DEEP_TERMS 16
#define RSS_CACHE_TTL 1800
#define MAX_VOLUME 130
#define APPLE_SEARCH_TIMEOUT 5L
#define PODCASTINDEX_TIMEOUT 10L
#define PODCASTINDEX_CACHE_TTL 86400
#define RESPONSE_LIMIT (16u * 1024u * 1024u)

typedef struct { char *data; size_t size; } Buf;
typedef enum { RESULT_HEADER, RESULT_SHOW, RESULT_EPISODE } ResultType;
typedef struct {
    ResultType type;
    int rank;
    int order;
    long date_ts;
    char title[MAX_FIELD], episode[MAX_FIELD], artist[MAX_FIELD];
    char feed[MAX_URL], collection_url[MAX_URL], artwork[MAX_URL], episode_url[MAX_URL];
    char collection_id[64], track_id[64], episode_guid[MAX_FIELD];
} Show;
typedef struct { char title[MAX_FIELD], audio[MAX_URL]; long date_ts; } Ep;

static Show shows[MAX_SEARCH_RESULTS];
static Ep eps[MAX_EPS];
static int show_count=0, ep_count=0, sel=0, mode=0, editing=0, paused=0, playing_ep=-1, current_volume=100, last_show_sel=0;
static int apple_show_count=0, apple_episode_count=0, podcastindex_episode_count=0, podcastindex_search_done=0;
static int list_searching=0, list_search_len=0;
static int list_top=0, last_show_top=0;
static char query[256]="";
static char list_query[256]="";
static char status[512]="Press i to search Apple Podcasts.";
static long status_flash_until=0;
static char playing_audio[MAX_URL]="";
static double play_pos=0, play_dur=0;
static double selected_resume_pos=0;
static pid_t mpv_pid=-1;
static char mpv_socket[sizeof(((struct sockaddr_un *)0)->sun_path)] = "";
static char mpv_socket_tmpdir[PATH_MAX] = "";
static int network_ui_ready=0;
static _Atomic int network_cancel_requested=0;

static void mpv_command(const char *json);
static int update_progress(void);
static void fmt_time(double sec, char *out, size_t n);
static void draw_screen(void);
static unsigned long hash_url(const char *s);
static int snprintf_ok(int n, size_t size);
static int contains_icase(const char *haystack, const char *needle);

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

static void cleanup_mpv_socket_path(void){
    if(mpv_socket[0])
        unlink(mpv_socket);
    if(mpv_socket_tmpdir[0]){
        rmdir(mpv_socket_tmpdir);
        mpv_socket_tmpdir[0]=0;
    }
}

static int set_private_tmp_mpv_socket_path(void){
    struct sockaddr_un addr;
    const char *bases[2]={getenv("TMPDIR"),"/tmp"};
    for(size_t i=0;i<2;i++){
        const char *base=bases[i];
        char tmpl[PATH_MAX];
        char *dir;
        int n;
        if(!base||!*base)continue;
        if(i==1&&bases[0]&&*bases[0]&&!strcmp(bases[0],"/tmp"))continue;
        n=snprintf(tmpl,sizeof(tmpl),"%s/simplepod-mpv-XXXXXX",base);
        if(n<0||(size_t)n>=sizeof(tmpl))continue;
        dir=mkdtemp(tmpl);
        if(!dir)continue;
        n=snprintf(mpv_socket,sizeof(mpv_socket),"%s/socket",dir);
        if(n>=0&&(size_t)n<sizeof(mpv_socket)&&(size_t)n<sizeof(addr.sun_path)){
            snprintf(mpv_socket_tmpdir,sizeof(mpv_socket_tmpdir),"%s",dir);
            return 1;
        }
        rmdir(dir);
        mpv_socket[0]=0;
    }
    return 0;
}

static void init_mpv_socket_path(void){
    struct sockaddr_un addr;
    const char *runtime=getenv("XDG_RUNTIME_DIR");
    int n=-1;
    if(runtime&&*runtime)
        n=snprintf(mpv_socket,sizeof(mpv_socket),"%s/simplepod-mpv-%ld.sock",runtime,(long)getpid());
    if(n<0||(size_t)n>=sizeof(mpv_socket)||(size_t)n>=sizeof(addr.sun_path)){
        if(!set_private_tmp_mpv_socket_path())mpv_socket[0]=0;
    }
    if(mpv_socket[0])
        unlink(mpv_socket);
    atexit(cleanup_mpv_socket_path);
}

static int fetch_cancel_cb(void *unused,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d){
    (void)unused;(void)a;(void)b;(void)c;(void)d;
    return network_cancel_requested;
}

static char *fetch_url_timeout_blocking(const char *url, long timeout_sec){
    CURL*c=curl_easy_init(); if(!c)return NULL;
    Buf b={0};
    curl_easy_setopt(c,CURLOPT_URL,url);
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,write_cb);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,&b);
    curl_easy_setopt(c,CURLOPT_USERAGENT,"simplepod/0.1");
    curl_easy_setopt(c,CURLOPT_TIMEOUT,timeout_sec);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,timeout_sec < 3L ? timeout_sec : 3L);
    curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,fetch_cancel_cb);
    CURLcode r=curl_easy_perform(c);
    curl_easy_cleanup(c);
    if(r!=CURLE_OK){ free(b.data); return NULL; }
    return b.data;
}

typedef struct { const char *url; long timeout; char *data; int done; pthread_mutex_t lock; } FetchJob;

static void *fetch_worker(void *arg){
    FetchJob *job=arg;
    job->data=fetch_url_timeout_blocking(job->url,job->timeout);
    pthread_mutex_lock(&job->lock);job->done=1;pthread_mutex_unlock(&job->lock);
    return NULL;
}

static char *fetch_url_timeout(const char *url, long timeout_sec){
    FetchJob job={.url=url,.timeout=timeout_sec,.lock=PTHREAD_MUTEX_INITIALIZER};
    pthread_t thread;int done=0;
    network_cancel_requested=0;
    if(pthread_create(&thread,NULL,fetch_worker,&job)!=0)
        return fetch_url_timeout_blocking(url,timeout_sec);
    while(!done){
        pthread_mutex_lock(&job.lock);done=job.done;pthread_mutex_unlock(&job.lock);
        if(done)break;
        if(network_ui_ready){
            draw_screen();
            timeout(SUI_NETWORK_POLL_MS);int ch=getch();timeout(SUI_PLAYBACK_POLL_MS);
            if(ch==27){network_cancel_requested=1;snprintf(status,sizeof(status),"Cancelling request...");}
        }else{
            sui_sleep_ms(SUI_NETWORK_POLL_MS);
        }
    }
    pthread_join(thread,NULL);pthread_mutex_destroy(&job.lock);
    network_cancel_requested=0;
    return job.data;
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

static int json_scalar_field(const char*src,const char*key,char*out,size_t outsz){
    char pat[128]; snprintf(pat,sizeof(pat),"\"%s\":",key);
    char*p=strstr((char*)src,pat); if(!p)return 0;
    p+=strlen(pat);
    while(*p&&isspace((unsigned char)*p))p++;

    size_t i=0;
    if(*p=='"'){
        p++;
        while(*p&&i+1<outsz){ if(*p=='"'&&p[-1]!='\\')break; out[i++]=*p++; }
    } else {
        while(*p&&*p!=','&&*p!='}'&&!isspace((unsigned char)*p)&&i+1<outsz)
            out[i++]=*p++;
    }

    out[i]=0;
    clean(out);
    return i>0;
}

static int next_json_object(const char **cursor, char **out){
    const char *p=strchr(*cursor,'{');
    if(!p)return 0;

    int depth=0, in_string=0, escaped=0;
    for(const char *q=p;*q;q++){
        char ch=*q;
        if(in_string){
            if(escaped)escaped=0;
            else if(ch=='\\')escaped=1;
            else if(ch=='"')in_string=0;
            continue;
        }

        if(ch=='"'){
            in_string=1;
        } else if(ch=='{'){
            depth++;
        } else if(ch=='}'){
            depth--;
            if(depth==0){
                size_t len=(size_t)(q-p+1);
                char *chunk=malloc(len+1);
                if(!chunk)return 0;
                memcpy(chunk,p,len);
                chunk[len]=0;
                *cursor=q+1;
                *out=chunk;
                return 1;
            }
        }
    }
    return 0;
}

static const char *json_results_start(const char *json){
    const char *p=strstr(json,"\"results\"");
    if(!p)return json;
    p=strchr(p,'[');
    return p ? p+1 : json;
}

static size_t trimmed_span(const char *s, const char **start){
    if(!s){ *start=""; return 0; }
    while(*s&&isspace((unsigned char)*s))s++;
    const char *end=s+strlen(s);
    while(end>s&&isspace((unsigned char)end[-1]))end--;
    *start=s;
    return (size_t)(end-s);
}

static int equals_icase_trim(const char *a, const char *b){
    const char *as, *bs;
    size_t alen=trimmed_span(a,&as);
    size_t blen=trimmed_span(b,&bs);
    if(alen!=blen)return 0;
    for(size_t i=0;i<alen;i++){
        if(tolower((unsigned char)as[i])!=tolower((unsigned char)bs[i]))
            return 0;
    }
    return 1;
}

static int show_result_rank(const Show *s, const char *term){
    if(equals_icase_trim(s->title,term))return 0;
    if(equals_icase_trim(s->artist,term))return 1;
    if(contains_icase(s->title,term))return 2;
    if(contains_icase(s->artist,term))return 3;
    return 4;
}

static int month_number(const char *s){
    static const char *months[]={
        "jan","feb","mar","apr","may","jun",
        "jul","aug","sep","oct","nov","dec"
    };
    for(int i=0;i<12;i++)
        if(s&&strlen(s)>=3&&tolower((unsigned char)s[0])==months[i][0] &&
           tolower((unsigned char)s[1])==months[i][1] &&
           tolower((unsigned char)s[2])==months[i][2])
            return i+1;
    return 0;
}

static long days_from_civil(int y, unsigned m, unsigned d){
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long)era * 146097L + (long)doe - 719468L;
}

static long unix_time_utc(int y, int mon, int day, int hour, int min, int sec){
    if(y<1970||mon<1||mon>12||day<1||day>31||hour<0||hour>23||
       min<0||min>59||sec<0||sec>60)
        return 0;
    return days_from_civil(y,(unsigned)mon,(unsigned)day) * 86400L +
           hour * 3600L + min * 60L + sec;
}

static long parse_date_value(const char *s){
    int y=0, mon=0, day=0, hour=0, min=0, sec=0;
    char month[16];
    char *end;
    long epoch;

    if(!s||!*s)return 0;
    while(*s&&isspace((unsigned char)*s))s++;
    epoch=strtol(s,&end,10);
    if(end>s && (!*end || isspace((unsigned char)*end) || *end==',' || *end=='}')){
        if(epoch>100000000000L)epoch/=1000L;
        return epoch>0 ? epoch : 0;
    }
    if(sscanf(s,"%d-%d-%dT%d:%d:%d",&y,&mon,&day,&hour,&min,&sec)>=3)
        return unix_time_utc(y,mon,day,hour,min,sec);
    if(sscanf(s,"%d-%d-%d %d:%d:%d",&y,&mon,&day,&hour,&min,&sec)>=3)
        return unix_time_utc(y,mon,day,hour,min,sec);
    if(sscanf(s,"%*[^,], %d %15s %d %d:%d:%d",&day,month,&y,&hour,&min,&sec)==6){
        mon=month_number(month);
        return unix_time_utc(y,mon,day,hour,min,sec);
    }
    if(sscanf(s,"%d %15s %d %d:%d:%d",&day,month,&y,&hour,&min,&sec)==6){
        mon=month_number(month);
        return unix_time_utc(y,mon,day,hour,min,sec);
    }
    return 0;
}

static int episode_result_rank(const Show *s, const char *term, const char *desc, const char *short_desc){
    if(contains_icase(s->episode,term))return 0;
    if(contains_icase(s->title,term))return 1;
    if(contains_icase(s->artist,term))return 2;
    if(contains_icase(short_desc,term))return 3;
    if(contains_icase(desc,term))return 4;
    return 5;
}

static int result_sort_cmp(const void *a, const void *b){
    const Show *ra=a, *rb=b;
    if(ra->rank!=rb->rank)return ra->rank-rb->rank;
    return ra->order-rb->order;
}

static int episode_date_desc_cmp(const void *a, const void *b){
    const Show *ra=a, *rb=b;
    if(ra->date_ts && rb->date_ts && ra->date_ts!=rb->date_ts)
        return rb->date_ts > ra->date_ts ? 1 : -1;
    if(ra->date_ts && !rb->date_ts)return -1;
    if(!ra->date_ts && rb->date_ts)return 1;
    return result_sort_cmp(a,b);
}

static int ep_date_desc_cmp(const void *a, const void *b){
    const Ep *ea=a, *eb=b;
    if(ea->date_ts && eb->date_ts && ea->date_ts!=eb->date_ts)
        return eb->date_ts > ea->date_ts ? 1 : -1;
    if(ea->date_ts && !eb->date_ts)return -1;
    if(!ea->date_ts && eb->date_ts)return 1;
    return 0;
}

static int same_nonempty(const char *a, const char *b){
    return a&&b&&a[0]&&b[0]&&strcmp(a,b)==0;
}

static int same_title_collection(const Show *a, const Show *b){
    return equals_icase_trim(a->episode,b->episode) &&
           equals_icase_trim(a->title,b->title);
}

static int duplicate_show_result(const Show *r){
    for(int i=0;i<show_count;i++){
        Show *s=&shows[i];
        if(s->type==RESULT_HEADER)continue;
        if(s->type!=RESULT_SHOW)continue;
        if(same_nonempty(r->feed,s->feed))return 1;
        if(same_nonempty(r->collection_id,s->collection_id))return 1;
        if(same_nonempty(r->track_id,s->track_id))return 1;
    }
    return 0;
}

static int duplicate_episode_result_pair(const Show *a, const Show *b){
    if(same_nonempty(a->episode_guid,b->episode_guid))return 1;
    if(same_nonempty(a->episode_url,b->episode_url))return 1;
    if(a->episode[0]&&a->title[0]&&same_title_collection(a,b))return 1;
    if(same_nonempty(a->track_id,b->track_id))return 1;
    return 0;
}

static int duplicate_episode_result(const Show *r){
    for(int i=0;i<show_count;i++){
        Show *s=&shows[i];
        if(s->type!=RESULT_EPISODE)continue;
        if(duplicate_episode_result_pair(r,s))return 1;
    }
    return 0;
}

static int duplicate_episode_array(const Show *results, int count, const Show *r){
    for(int i=0;i<count;i++)
        if(duplicate_episode_result_pair(r,&results[i]))return 1;
    return 0;
}

static int add_search_header(const char *title){
    if(show_count>=MAX_SEARCH_RESULTS)return 0;
    Show *s=&shows[show_count++];
    memset(s,0,sizeof(*s));
    s->type=RESULT_HEADER;
    snprintf(s->title,sizeof(s->title),"%s",title);
    return 1;
}

static int add_search_result(const Show *r){
    if(show_count>=MAX_SEARCH_RESULTS)return 0;
    if(r->type==RESULT_SHOW && duplicate_show_result(r))return 0;
    if(r->type==RESULT_EPISODE && duplicate_episode_result(r))return 0;
    shows[show_count++]=*r;
    return 1;
}

static void fill_artwork_field(const char *chunk, Show *s){
    json_field(chunk,"artworkUrl100",s->artwork,sizeof(s->artwork));
    if(!s->artwork[0])json_field(chunk,"artworkUrl160",s->artwork,sizeof(s->artwork));
    if(!s->artwork[0])json_field(chunk,"artworkUrl600",s->artwork,sizeof(s->artwork));
    if(!s->artwork[0])json_field(chunk,"artworkUrl60",s->artwork,sizeof(s->artwork));
}

static void parse_show_result(const char *chunk, Show *s, int order, const char *term){
    memset(s,0,sizeof(*s));
    s->type=RESULT_SHOW;
    s->order=order;
    json_field(chunk,"collectionName",s->title,sizeof(s->title));
    if(!s->title[0])json_field(chunk,"trackName",s->title,sizeof(s->title));
    json_field(chunk,"artistName",s->artist,sizeof(s->artist));
    json_field(chunk,"feedUrl",s->feed,sizeof(s->feed));
    json_field(chunk,"collectionViewUrl",s->collection_url,sizeof(s->collection_url));
    if(!s->collection_url[0])json_field(chunk,"trackViewUrl",s->collection_url,sizeof(s->collection_url));
    fill_artwork_field(chunk,s);
    json_scalar_field(chunk,"collectionId",s->collection_id,sizeof(s->collection_id));
    json_scalar_field(chunk,"trackId",s->track_id,sizeof(s->track_id));
    s->rank=show_result_rank(s,term);
}

static int parse_episode_result(const char *chunk, Show *s, int order, const char *term){
    char desc[MAX_FIELD]="", short_desc[MAX_FIELD]="", date[MAX_FIELD]="";
    memset(s,0,sizeof(*s));
    s->type=RESULT_EPISODE;
    s->order=order;
    json_field(chunk,"collectionName",s->title,sizeof(s->title));
    json_field(chunk,"trackName",s->episode,sizeof(s->episode));
    json_field(chunk,"artistName",s->artist,sizeof(s->artist));
    json_field(chunk,"feedUrl",s->feed,sizeof(s->feed));
    json_field(chunk,"episodeUrl",s->episode_url,sizeof(s->episode_url));
    if(!s->episode_url[0])json_field(chunk,"previewUrl",s->episode_url,sizeof(s->episode_url));
    json_field(chunk,"collectionViewUrl",s->collection_url,sizeof(s->collection_url));
    if(!s->collection_url[0])json_field(chunk,"trackViewUrl",s->collection_url,sizeof(s->collection_url));
    fill_artwork_field(chunk,s);
    json_scalar_field(chunk,"collectionId",s->collection_id,sizeof(s->collection_id));
    json_scalar_field(chunk,"trackId",s->track_id,sizeof(s->track_id));
    json_field(chunk,"episodeGuid",s->episode_guid,sizeof(s->episode_guid));
    json_field(chunk,"releaseDate",date,sizeof(date));
    s->date_ts=parse_date_value(date);
    json_field(chunk,"description",desc,sizeof(desc));
    json_field(chunk,"shortDescription",short_desc,sizeof(short_desc));
    s->rank=episode_result_rank(s,term,desc,short_desc);

    return s->episode[0] &&
           (s->episode_url[0]||s->feed[0]||s->collection_url[0]);
}

static int itunes_search_url(char *url, size_t urlsz, const char *term, const char *entity, int limit){
    CURL*c=curl_easy_init(); if(!c)return 0;
    char*esc=curl_easy_escape(c,term,0);
    int ok=snprintf_ok(snprintf(url,urlsz,
        "https://itunes.apple.com/search?media=podcast&entity=%s&limit=%d&term=%s",
        entity,limit,esc?esc:term),urlsz);
    if(esc)
        curl_free(esc);
    curl_easy_cleanup(c);
    return ok;
}

static void search_apple(const char*term){
    show_count=0; ep_count=0; mode=0; sel=0; list_top=0; last_show_top=0;
    apple_show_count=0; apple_episode_count=0; podcastindex_episode_count=0; podcastindex_search_done=0;
    list_searching=0; list_query[0]=0; list_search_len=0;
    if(!term[0]){ snprintf(status,sizeof(status),"Empty search."); return; }

    snprintf(status,sizeof(status),"Searching... Esc cancels");
    char url[MAX_URL];
    if(!itunes_search_url(url,sizeof(url),term,"podcast",MAX_ITUNES_SHOWS)){
        snprintf(status,sizeof(status),"Search failed.");
        return;
    }

    char*json=fetch_url_timeout(url,APPLE_SEARCH_TIMEOUT);
    if(!json){ snprintf(status,sizeof(status),"Search failed."); return; }

    Show *show_results=calloc(MAX_ITUNES_SHOWS,sizeof(*show_results));
    if(!show_results){
        free(json);
        snprintf(status,sizeof(status),"Search failed.");
        return;
    }

    int show_results_count=0;
    const char*p=json_results_start(json);
    char *chunk=NULL;
    while(show_results_count<MAX_ITUNES_SHOWS && next_json_object(&p,&chunk)){
        if(!strstr(chunk,"\"wrapperType\":\"track\"")){
            free(chunk);
            chunk=NULL;
            continue;
        }

        Show s;
        parse_show_result(chunk,&s,show_results_count,term);
        if(s.title[0]&&(s.feed[0]||s.collection_url[0]))
            show_results[show_results_count++]=s;

        free(chunk);
        chunk=NULL;
    }
    free(json);

    qsort(show_results,(size_t)show_results_count,sizeof(*show_results),result_sort_cmp);
    int added_shows=0;
    if(show_results_count>0 && add_search_header("Shows")){
        for(int i=0;i<show_results_count;i++)
            if(add_search_result(&show_results[i]))added_shows++;
    }
    apple_show_count=added_shows;
    free(show_results);

    int added_episodes=0, episode_results_count=0;
    if(itunes_search_url(url,sizeof(url),term,"podcastEpisode",MAX_ITUNES_EPISODES)){
        json=fetch_url_timeout(url,APPLE_SEARCH_TIMEOUT);
        Show *episode_results=calloc(MAX_ITUNES_EPISODES,sizeof(*episode_results));
        if(json&&episode_results){
            p=json_results_start(json);
            chunk=NULL;
            while(episode_results_count<MAX_ITUNES_EPISODES && next_json_object(&p,&chunk)){
                if(!strstr(chunk,"\"wrapperType\":\"podcastEpisode\"")){
                    free(chunk);
                    chunk=NULL;
                    continue;
                }

                Show s;
                if(parse_episode_result(chunk,&s,episode_results_count,term))
                    episode_results[episode_results_count++]=s;

                free(chunk);
                chunk=NULL;
            }
        }
        qsort(episode_results,(size_t)episode_results_count,sizeof(*episode_results),episode_date_desc_cmp);
        if(episode_results_count>0 && show_count<MAX_SEARCH_RESULTS)
            add_search_header("Episodes & Appearances");
        for(int i=0;i<episode_results_count && show_count<MAX_SEARCH_RESULTS;i++)
            if(add_search_result(&episode_results[i]))added_episodes++;
        free(episode_results);
        free(json);
    }
    apple_episode_count=added_episodes;

    if(show_count>0 && shows[0].type==RESULT_HEADER)
        sel = show_count>1 ? 1 : 0;

    snprintf(status,sizeof(status),"Apple: %d %s, %d %s.",
             apple_show_count,apple_show_count==1?"show":"shows",
             apple_episode_count,apple_episode_count==1?"episode":"episodes");
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

static int cache_root_dir(char *out, size_t n){
    char base[PATH_MAX];
    const char *xdg=getenv("XDG_CACHE_HOME");
    if(xdg && *xdg && xdg[0]=='/')
        snprintf(base,sizeof(base),"%s",xdg);
    else if(!home_path(base,sizeof(base),".cache"))
        return 0;
    if(!snprintf_ok(snprintf(out,n,"%s/simplepod",base),n))return 0;
    return mkdirs(out);
}

static void write_podcastindex_debug(FILE *f, const char *url, long http_code,
                                     CURLcode curl_code, const char *curl_error,
                                     const char *auth_date, const char *key_prefix,
                                     const char *auth_prefix, const char *body){
    fprintf(f,"[%ld] PodcastIndex request failed\n",(long)time(NULL));
    fprintf(f,"endpoint: %s\n",url?url:"");
    fprintf(f,"X-Auth-Date: %s\n",auth_date&&*auth_date?auth_date:"<none>");
    fprintf(f,"api_key_prefix: %s\n",key_prefix&&*key_prefix?key_prefix:"<none>");
    fprintf(f,"sha1_prefix: %s\n",auth_prefix&&*auth_prefix?auth_prefix:"<none>");
    fprintf(f,"http_status: %ld\n",http_code);
    fprintf(f,"curl_code: %d\n",(int)curl_code);
    if(curl_error&&*curl_error)
        fprintf(f,"curl_error: %s\n",curl_error);
    fprintf(f,"auth_headers: X-Auth-Key, X-Auth-Date, Authorization, User-Agent\n");
    fprintf(f,"accept: not sent for GET\n");
    fprintf(f,"content_type: not sent for GET\n");
    fprintf(f,"response_body:\n%s\n\n",body&&*body?body:"<empty>");
}

static void append_podcastindex_log(const char *url, long http_code,
                                    CURLcode curl_code, const char *curl_error,
                                    const char *auth_date, const char *key_prefix,
                                    const char *auth_prefix, const char *body){
    char dir[PATH_MAX], path[PATH_MAX];
    FILE *f=NULL;

    write_podcastindex_debug(stderr,url,http_code,curl_code,curl_error,
                             auth_date,key_prefix,auth_prefix,body);

    if(cache_root_dir(dir,sizeof(dir)) &&
       snprintf_ok(snprintf(path,sizeof(path),"%s/podcastindex.log",dir),sizeof(path)))
        f=fopen(path,"ab");
    if(f){
        write_podcastindex_debug(f,url,http_code,curl_code,curl_error,
                                 auth_date,key_prefix,auth_prefix,body);
        fclose(f);
    }
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

static char *fetch_feed_cached_timeout(const char *url, long timeout_sec, int quiet){
    char path[PATH_MAX];
    if(!cache_file_path(url,path,sizeof(path)))
        return fetch_url_timeout(url,timeout_sec);

    struct stat st;
    time_t now=time(NULL);

    if(stat(path,&st)==0 && now-st.st_mtime < RSS_CACHE_TTL){
        char *cached=NULL;
        if(read_file(path,&cached)){
            if(!quiet)snprintf(status,sizeof(status),"Loaded cached feed.");
            return cached;
        }
    }

    char *fresh=fetch_url_timeout(url,timeout_sec);
    if(fresh){
        write_file(path,fresh);
        if(!quiet)snprintf(status,sizeof(status),"Downloaded and cached feed.");
    }
    return fresh;
}

static char *fetch_feed_cached(const char *url){
    return fetch_feed_cached_timeout(url,25L,0);
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
        {
            char date[MAX_FIELD]="";
            if(!tag_text(chunk,"pubDate",date,sizeof(date)))
                if(!tag_text(chunk,"published",date,sizeof(date)))
                    tag_text(chunk,"dc:date",date,sizeof(date));
            e->date_ts=parse_date_value(date);
        }
        if(e->title[0]&&e->audio[0])ep_count++;

        free(chunk); p++;
    }
    qsort(eps,(size_t)ep_count,sizeof(*eps),ep_date_desc_cmp);
    free(xml); mode=1;
    snprintf(status,sizeof(status),"%d episodes.",ep_count);
}

static char *trim_ws(char *s){
    while(*s&&isspace((unsigned char)*s))s++;
    char *end=s+strlen(s);
    while(end>s&&isspace((unsigned char)end[-1]))*--end=0;
    return s;
}

static void copy_trimmed_value(char *out, size_t outsz, const char *src){
    if(!out||outsz==0)return;
    out[0]=0;
    if(!src)return;
    while(*src&&isspace((unsigned char)*src))src++;
    const char *end=src+strlen(src);
    while(end>src&&isspace((unsigned char)end[-1]))end--;
    size_t len=(size_t)(end-src);
    if(len>=outsz)len=outsz-1;
    memcpy(out,src,len);
    out[len]=0;
}

static void read_podcastindex_config(char *key, size_t keysz, char *secret, size_t secretsz){
    char path[PATH_MAX], *data=NULL;
    if(!home_path(path,sizeof(path),".config/simplepod/config"))return;
    if(!read_file(path,&data))return;

    char *line=data;
    while(line&&*line){
        char *next=strchr(line,'\n');
        if(next)*next++=0;
        char *hash=strchr(line,'#');
        if(hash)*hash=0;
        char *eq=strchr(line,'=');
        if(eq){
            *eq=0;
            char *name=trim_ws(line);
            char *value=trim_ws(eq+1);
            if(!key[0]&&!strcmp(name,"podcastindex_key"))
                copy_trimmed_value(key,keysz,value);
            else if(!secret[0]&&!strcmp(name,"podcastindex_secret"))
                copy_trimmed_value(secret,secretsz,value);
        }
        line=next;
    }
    free(data);
}

static int podcastindex_credentials(char *key, size_t keysz, char *secret, size_t secretsz){
    const char *env_key=getenv("PODCASTINDEX_KEY");
    const char *env_secret=getenv("PODCASTINDEX_SECRET");
    key[0]=0;
    secret[0]=0;
    if(env_key&&*env_key)copy_trimmed_value(key,keysz,env_key);
    if(env_secret&&*env_secret)copy_trimmed_value(secret,secretsz,env_secret);
    if(!key[0]||!secret[0])read_podcastindex_config(key,keysz,secret,secretsz);
    return key[0]&&secret[0];
}

static void sha1_hex(const char *input, char out[SHA_DIGEST_LENGTH*2+1]){
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char*)input,strlen(input),digest);
    for(int i=0;i<SHA_DIGEST_LENGTH;i++)
        snprintf(out+(i*2),3,"%02x",digest[i]);
    out[SHA_DIGEST_LENGTH*2]=0;
}

static void prefix_chars(char *out, size_t outsz, const char *src, size_t count){
    if(!out||outsz==0)return;
    out[0]=0;
    if(!src)return;
    size_t len=strlen(src);
    if(len>count)len=count;
    if(len>=outsz)len=outsz-1;
    memcpy(out,src,len);
    out[len]=0;
}

static int podcastindex_search_url(char *url, size_t urlsz, const char *term){
    CURL*c=curl_easy_init(); if(!c)return 0;
    char*esc=curl_easy_escape(c,term,0);
    int ok=snprintf_ok(snprintf(url,urlsz,
        "https://api.podcastindex.org/api/1.0/search/byperson?q=%s&max=200",
        esc?esc:term),urlsz);
    if(esc)curl_free(esc);
    curl_easy_cleanup(c);
    return ok;
}

static int deep_stop_word(const char *s){
    static const char *words[]={
        "a","an","and","by","for","in","of","on","or","the","to","with",
        "podcast","episode","episodes","show","guest"
    };
    for(size_t i=0;i<sizeof(words)/sizeof(words[0]);i++)
        if(equals_icase_trim(s,words[i]))return 1;
    return 0;
}

static void add_deep_term(char terms[][128], int *count, const char *src){
    char tmp[128];
    if(*count>=MAX_DEEP_TERMS)return;
    snprintf(tmp,sizeof(tmp),"%s",src?src:"");
    char *term=trim_ws(tmp);
    if(strlen(term)<2 || deep_stop_word(term))return;
    for(int i=0;i<*count;i++)
        if(equals_icase_trim(terms[i],term))return;
    snprintf(terms[*count],128,"%s",term);
    (*count)++;
}

static int parse_deep_terms(const char *src, char terms[][128]){
    int count=0;
    const char *p=src;
    while(p&&*p&&count<MAX_DEEP_TERMS){
        if(*p=='"'){
            char quoted[128];
            size_t q=0;
            p++;
            while(*p&&*p!='"'&&q+1<sizeof(quoted))
                quoted[q++]=*p++;
            quoted[q]=0;
            add_deep_term(terms,&count,quoted);
            if(*p=='"')p++;
        } else if(isalnum((unsigned char)*p)||*p=='\''||*p=='-') {
            char word[128];
            size_t w=0;
            while(*p&&(isalnum((unsigned char)*p)||*p=='\''||*p=='-')&&w+1<sizeof(word))
                word[w++]=*p++;
            word[w]=0;
            add_deep_term(terms,&count,word);
        } else {
            p++;
        }
    }

    if(count==0){
        char tmp[128];
        size_t len=src?strlen(src):0;
        if(len>=sizeof(tmp))len=sizeof(tmp)-1;
        if(len)memcpy(tmp,src,len);
        tmp[len]=0;
        char *term=trim_ws(tmp);
        if(*term){
            snprintf(terms[count],128,"%s",term);
            count++;
        }
    }
    return count;
}

static int deep_term_in_result(const Show *s, const char *desc, const char *term){
    return contains_icase(s->episode,term) ||
           contains_icase(desc,term) ||
           contains_icase(s->title,term) ||
           contains_icase(s->artist,term);
}

static int deep_result_matches_terms(const Show *s, const char *desc,
                                     char terms[][128], int term_count){
    for(int i=0;i<term_count;i++)
        if(!deep_term_in_result(s,desc,terms[i]))return 0;
    return 1;
}

static int deep_terms_in_episode_text(const Show *s, const char *desc,
                                      char terms[][128], int term_count){
    for(int i=0;i<term_count;i++)
        if(!contains_icase(s->episode,terms[i])&&!contains_icase(desc,terms[i]))
            return 0;
    return 1;
}

static int deep_match_rank(const Show *s, const char *desc,
                           char terms[][128], int term_count){
    int title_hits=0;
    for(int i=0;i<term_count;i++)
        if(contains_icase(s->episode,terms[i]))title_hits++;
    if(title_hits==term_count)return 0;
    if(deep_terms_in_episode_text(s,desc,terms,term_count))return 1;
    return 2;
}

static char *fetch_podcastindex_url_blocking(const char *url, const char *key, const char *secret,
                                             long *http_code){
    char path[PATH_MAX];
    struct stat st;
    time_t now=time(NULL);
    char date[32], auth[SHA_DIGEST_LENGTH*2+1], key_prefix[9], auth_prefix[9];
    if(http_code)*http_code=0;
    if(cache_file_path(url,path,sizeof(path)) &&
       stat(path,&st)==0 && now-st.st_mtime < PODCASTINDEX_CACHE_TTL){
        char *cached=NULL;
        if(read_file(path,&cached)){
            if(http_code)*http_code=200;
            return cached;
        }
    }

    snprintf(date,sizeof(date),"%ld",(long)now);
    prefix_chars(key_prefix,sizeof(key_prefix),key,8);

    size_t auth_len=strlen(key)+strlen(secret)+strlen(date)+1;
    char *auth_src=malloc(auth_len);
    if(!auth_src){
        append_podcastindex_log(url,0,CURLE_OUT_OF_MEMORY,"auth buffer allocation failed",
                                date,key_prefix,"",NULL);
        return NULL;
    }
    snprintf(auth_src,auth_len,"%s%s%s",key,secret,date);
    sha1_hex(auth_src,auth);
    free(auth_src);
    prefix_chars(auth_prefix,sizeof(auth_prefix),auth,8);

    CURL*c=curl_easy_init();
    if(!c){
        append_podcastindex_log(url,0,CURLE_FAILED_INIT,"curl_easy_init failed",
                                date,key_prefix,auth_prefix,NULL);
        return NULL;
    }
    Buf b={0};
    char error[CURL_ERROR_SIZE]="";

    char h_key[512], h_date[80], h_auth[128];
    snprintf(h_key,sizeof(h_key),"X-Auth-Key: %s",key);
    snprintf(h_date,sizeof(h_date),"X-Auth-Date: %s",date);
    snprintf(h_auth,sizeof(h_auth),"Authorization: %s",auth);

    struct curl_slist *headers=NULL;
    headers=curl_slist_append(headers,"User-Agent: SimplePod");
    headers=curl_slist_append(headers,h_key);
    headers=curl_slist_append(headers,h_date);
    headers=curl_slist_append(headers,h_auth);

    curl_easy_setopt(c,CURLOPT_URL,url);
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,write_cb);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,&b);
    curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(c,CURLOPT_ERRORBUFFER,error);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,PODCASTINDEX_TIMEOUT);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,5L);
    curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,fetch_cancel_cb);
    CURLcode r=curl_easy_perform(c);
    long code=0;
    curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if(http_code)*http_code=code;

    if(r!=CURLE_OK || code<200 || code>=300){
        append_podcastindex_log(url,code,r,error,date,key_prefix,auth_prefix,b.data);
        free(b.data);
        return NULL;
    }
    if(cache_file_path(url,path,sizeof(path)))
        write_file(path,b.data);
    return b.data;
}

typedef struct {
    const char *url,*key,*secret;long *http_code;char *data;int done;pthread_mutex_t lock;
} PodcastIndexJob;

static void *podcastindex_worker(void *arg){
    PodcastIndexJob *job=arg;
    job->data=fetch_podcastindex_url_blocking(job->url,job->key,job->secret,job->http_code);
    pthread_mutex_lock(&job->lock);job->done=1;pthread_mutex_unlock(&job->lock);
    return NULL;
}

static char *fetch_podcastindex_url(const char *url,const char *key,const char *secret,long *http_code){
    PodcastIndexJob job={.url=url,.key=key,.secret=secret,.http_code=http_code,
                         .lock=PTHREAD_MUTEX_INITIALIZER};
    pthread_t thread;int done=0;
    network_cancel_requested=0;
    if(pthread_create(&thread,NULL,podcastindex_worker,&job)!=0)
        return fetch_podcastindex_url_blocking(url,key,secret,http_code);
    while(!done){
        pthread_mutex_lock(&job.lock);done=job.done;pthread_mutex_unlock(&job.lock);
        if(done)break;
        if(network_ui_ready){
            draw_screen();timeout(SUI_NETWORK_POLL_MS);int ch=getch();timeout(SUI_PLAYBACK_POLL_MS);
            if(ch==27){network_cancel_requested=1;snprintf(status,sizeof(status),"Cancelling request...");}
        }else{sui_sleep_ms(SUI_NETWORK_POLL_MS);}
    }
    pthread_join(thread,NULL);pthread_mutex_destroy(&job.lock);
    network_cancel_requested=0;
    return job.data;
}

static const char *json_array_start(const char *json, const char *key){
    char pat[128];
    snprintf(pat,sizeof(pat),"\"%s\"",key);
    const char *p=strstr(json,pat);
    if(!p)return json;
    p=strchr(p,'[');
    return p ? p+1 : json;
}

static int parse_podcastindex_episode(const char *chunk, Show *s, int order,
                                      char *desc, size_t descsz){
    char date[64]="";
    memset(s,0,sizeof(*s));
    s->type=RESULT_EPISODE;
    s->order=order;
    json_field(chunk,"feedTitle",s->title,sizeof(s->title));
    json_field(chunk,"title",s->episode,sizeof(s->episode));
    json_field(chunk,"feedAuthor",s->artist,sizeof(s->artist));
    json_field(chunk,"feedUrl",s->feed,sizeof(s->feed));
    json_field(chunk,"enclosureUrl",s->episode_url,sizeof(s->episode_url));
    json_field(chunk,"guid",s->episode_guid,sizeof(s->episode_guid));
    json_field(chunk,"image",s->artwork,sizeof(s->artwork));
    if(!s->artwork[0])json_field(chunk,"feedImage",s->artwork,sizeof(s->artwork));
    json_scalar_field(chunk,"datePublished",date,sizeof(date));
    if(!date[0])json_scalar_field(chunk,"datePublishedPretty",date,sizeof(date));
    s->date_ts=parse_date_value(date);
    if(desc&&descsz)json_field(chunk,"description",desc,descsz);
    return s->episode[0]&&(s->episode_url[0]||s->feed[0]);
}

static void start_deep_search(void){
    if(mode!=0){
        snprintf(status,sizeof(status),"Back to search results before deep search.");
        return;
    }
    if(!query[0]){
        snprintf(status,sizeof(status),"Search first, then press D for deep search.");
        return;
    }
    if(podcastindex_search_done){
        snprintf(status,sizeof(status),"PodcastIndex: %d deep episodes",
                 podcastindex_episode_count);
        return;
    }

    char key[256], secret[256];
    if(!podcastindex_credentials(key,sizeof(key),secret,sizeof(secret))){
        snprintf(status,sizeof(status),"Deep search needs PodcastIndex key/secret.");
        return;
    }

    char url[MAX_URL];
    if(!podcastindex_search_url(url,sizeof(url),query)){
        snprintf(status,sizeof(status),"PodcastIndex failed: bad URL");
        return;
    }

    char terms[MAX_DEEP_TERMS][128];
    int term_count=parse_deep_terms(query,terms);

    snprintf(status,sizeof(status),"PodcastIndex deep search... Esc cancels");
    if(stdscr)draw_screen();

    long http_code=0;
    char *json=fetch_podcastindex_url(url,key,secret,&http_code);
    if(!json){
        snprintf(status,sizeof(status),"PodcastIndex failed: HTTP %ld",http_code);
        return;
    }

    Show *candidates=calloc(MAX_DEEP_RESULTS,sizeof(*candidates));
    if(!candidates){
        free(json);
        snprintf(status,sizeof(status),"PodcastIndex failed: out of memory");
        return;
    }

    int candidate_count=0, parsed=0;
    const char *p=json_array_start(json,"items");
    char *chunk=NULL;
    while(candidate_count<MAX_DEEP_RESULTS && next_json_object(&p,&chunk)){
        Show r;
        char desc[MAX_FIELD*8]="";
        if(parse_podcastindex_episode(chunk,&r,parsed,desc,sizeof(desc)) &&
           deep_result_matches_terms(&r,desc,terms,term_count)){
            r.rank=deep_match_rank(&r,desc,terms,term_count);
            r.order=parsed;
            if(!duplicate_episode_array(candidates,candidate_count,&r))
                candidates[candidate_count++]=r;
        }
        parsed++;
        free(chunk);
        chunk=NULL;
    }
    free(json);

    qsort(candidates,(size_t)candidate_count,sizeof(*candidates),episode_date_desc_cmp);

    int added=0, header_added=0;
    for(int i=0;i<candidate_count&&show_count<MAX_SEARCH_RESULTS;i++){
        if(duplicate_episode_result(&candidates[i]))continue;
        if(!header_added){
            add_search_header("PodcastIndex Deep Results");
            header_added=1;
        }
        if(add_search_result(&candidates[i]))added++;
    }

    free(candidates);
    podcastindex_episode_count=added;
    podcastindex_search_done=1;
    snprintf(status,sizeof(status),"PodcastIndex: %d deep episodes",
             podcastindex_episode_count);
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
    playing_ep = mode==1 ? sel : -1;
    snprintf(playing_audio,sizeof(playing_audio),"%s",url);
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

    playing_ep = mode==1 ? sel : -1;
    snprintf(playing_audio,sizeof(playing_audio),"%s",url);

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

static int update_progress(void){
    double old_pos=play_pos,old_dur=play_dur;
    if(mpv_pid <= 0) return 0;
    if(waitpid(mpv_pid,NULL,WNOHANG)==mpv_pid){
        mpv_pid=-1;
        paused=0;
        return 1;
    }

    double p = mpv_get_double_property("time-pos");
    double d = mpv_get_double_property("duration");

    if(p >= 0) play_pos = p;
    if(d > 0) play_dur = d;
    return (int)old_pos!=(int)play_pos || (int)old_dur!=(int)play_dur;
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

static int list_item_selectable(int idx){
    if(mode==0){
        return idx>=0 && idx<show_count && shows[idx].type!=RESULT_HEADER;
    }
    return idx>=0 && idx<ep_count;
}

static int nearest_selectable(int start){
    int count=current_list_count();
    if(count<=0)return -1;
    if(start<0)start=0;
    if(start>=count)start=count-1;
    for(int radius=0;radius<count;radius++){
        int down=start+radius;
        int up=start-radius;
        if(down<count&&list_item_selectable(down))return down;
        if(up>=0&&list_item_selectable(up))return up;
    }
    return -1;
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
        if(shows[idx].type==RESULT_HEADER)return 0;
        if(shows[idx].type==RESULT_EPISODE){
            return contains_icase(shows[idx].episode, needle) ||
                   contains_icase(shows[idx].title, needle) ||
                   contains_icase(shows[idx].artist, needle);
        }
        return contains_icase(shows[idx].title, needle) ||
               contains_icase(shows[idx].artist, needle);
    }

    if(idx < 0 || idx >= ep_count) return 0;
    return contains_icase(eps[idx].title, needle);
}

static void start_list_search(void){
    if(current_list_count() <= 0 || nearest_selectable(sel)<0){
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

    if(count <= 0 || nearest_selectable(sel)<0){
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

    int nearest=nearest_selectable(sel);
    if(nearest>=0)sel=nearest;

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

    int next=sel;
    do {
        next += delta;
    } while(next>=0 && next<count && !list_item_selectable(next));

    if(next < 0) next = nearest_selectable(0);
    else if(next >= count) next = nearest_selectable(count-1);

    if(next>=0)sel=next;

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

static void select_loaded_episode_by_title(const char *title){
    if(!title||!*title)return;

    for(int i=0;i<ep_count;i++){
        if(equals_icase_trim(eps[i].title,title)){
            sel=i;
            list_clamp_view();
            return;
        }
    }

    for(int i=0;i<ep_count;i++){
        if(contains_icase(eps[i].title,title)){
            sel=i;
            list_clamp_view();
            return;
        }
    }
}


static const char *selected_audio_url(void){
    if(mode==1 && sel>=0 && sel<ep_count && eps[sel].audio[0])
        return eps[sel].audio;

    if(mode==0 && sel>=0 && sel<show_count &&
       shows[sel].type==RESULT_EPISODE &&
       shows[sel].episode_url[0])
        return shows[sel].episode_url;

    return NULL;
}

static void resume_selected_episode(void){
    const char *url = selected_audio_url();

    if(!url || !*url){
        snprintf(status,sizeof(status),"Select an episode with audio to resume.");
        return;
    }

    double pos = resume_get(url);
    if(pos >= 60) play_resume_url(url, pos);
    else snprintf(status,sizeof(status),"No resume data for this episode.");
}


static void open_search_result(int idx){
    if(idx<0||idx>=show_count||shows[idx].type==RESULT_HEADER)return;

    Show *r=&shows[idx];
    if(r->type==RESULT_SHOW){
        if(!r->feed[0]){
            snprintf(status,sizeof(status),"No feed URL for this show.");
            return;
        }
        last_show_sel=sel;
        last_show_top=list_top;
        load_feed(r->feed);
        return;
    }

    if(r->type==RESULT_EPISODE){
        if(r->episode_url[0]){
            play_url(r->episode_url);
            return;
        }
        if(r->feed[0]){
            char episode_title[MAX_FIELD];
            snprintf(episode_title,sizeof(episode_title),"%s",r->episode);
            last_show_sel=sel;
            last_show_top=list_top;
            load_feed(r->feed);
            select_loaded_episode_by_title(episode_title);
            snprintf(status,sizeof(status),"Opened feed for: %.180s",episode_title);
            return;
        }
        snprintf(status,sizeof(status),"No episode URL or feed URL.");
    }
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
    mvprintw(1,0,"s search | D deep | f find | n/N next/prev | Enter play | r resume | Space pause | PgUp/PgDn volume | b back | q quit");

    move(2,0); clrtoeol();
    if(list_searching || list_query[0])
        mvprintw(2,0,"Find: %s%s",list_query,list_searching?"_":"");

    move(3,0); clrtoeol();
    mvprintw(3,0,"Search: %s%s",query,editing?"_":"");

    move(4,0); clrtoeol();
    mvprintw(4,0,"%s",status);

    move(5,0); clrtoeol();
    selected_resume_pos = 0;
    {
        const char *resume_url = selected_audio_url();
        if(resume_url && *resume_url){
            selected_resume_pos = resume_get(resume_url);
            if(selected_resume_pos >= 60){
                char rt[32];
                fmt_time(selected_resume_pos,rt,sizeof(rt));
                mvprintw(5,0,"Resume: %s available - press r",rt);
            } else {
                mvprintw(5,0,"Resume: none");
            }
        }
    }

    move(6,0); clrtoeol();
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
        if(mode==0 && shows[idx].type==RESULT_EPISODE && playing_audio[0] &&
           strcmp(shows[idx].episode_url, playing_audio)==0)
            is_playing = 1;

        char line[MAX_FIELD + MAX_FIELD + MAX_FIELD + 16];

        if(mode==0 && shows[idx].type==RESULT_HEADER)
            snprintf(line,sizeof(line),"%s",shows[idx].title);
        else if(mode==0 && shows[idx].type==RESULT_EPISODE)
            snprintf(line,sizeof(line),"  %-45.45s  %-30.30s  %-24.24s",
                     shows[idx].episode,shows[idx].title,shows[idx].artist);
        else if(mode==0)
            snprintf(line,sizeof(line),"%-45.45s  %-30.30s",shows[idx].title,shows[idx].artist);
        else
            snprintf(line,sizeof(line),"%s",eps[idx].title);

        if(mode==0 && shows[idx].type==RESULT_HEADER)
            attron(A_BOLD);
        else if(is_playing && idx==sel)
            attron(COLOR_PAIR(4) | A_BOLD);
        else if(is_playing)
            attron(COLOR_PAIR(3) | A_BOLD);
        else if(idx==sel && list_item_selectable(idx))
            attron(A_REVERSE);

        mvprintw(y,0,"%-*.*s",w,w,line);

        if(mode==0 && shows[idx].type==RESULT_HEADER)
            attroff(A_BOLD);
        else if(is_playing && idx==sel)
            attroff(COLOR_PAIR(4) | A_BOLD);
        else if(is_playing)
            attroff(COLOR_PAIR(3) | A_BOLD);
        else if(idx==sel && list_item_selectable(idx))
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
    set_escdelay(SUI_ESCAPE_DELAY_MS); notimeout(stdscr, FALSE); curs_set(0); leaveok(stdscr,TRUE); timeout(SUI_PLAYBACK_POLL_MS);
    network_ui_ready=1;

    SuiLoop playback_loop;
    sui_loop_init(&playback_loop,SUI_PLAYBACK_POLL_MS);

    while(1){
        if(sui_loop_take_dirty(&playback_loop))
            draw_screen();

        if(mpv_pid>0)
            timeout(sui_loop_timeout(&playback_loop,SUI_PLAYBACK_POLL_MS));
        else
            timeout(-1);
        int ch=getch();
        if(ch==ERR){
            if(mpv_pid>0 && sui_loop_tick_due(&playback_loop)){
                if(update_progress())sui_loop_mark_dirty(&playback_loop);
            }
            continue;
        }

        sui_loop_mark_dirty(&playback_loop);

        int count=mode==0?show_count:ep_count;

        if(editing){
            int len=strlen(query);

            if(ch==27 || ch==7){
                editing=0;
                query[0]=0;
                snprintf(status,sizeof(status),"Search cancelled.");
                draw_screen();
                napms(1000);
                snprintf(status,sizeof(status),"Press i to search Apple Podcasts.");
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
            snprintf(status,sizeof(status),"Skipped ahead 30 sec.");
        }
        else if(ch==KEY_LEFT){
            seek_relative(-15);
            snprintf(status,sizeof(status),"Rewound 15 sec.");
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
        else if(ch=='i'){ editing=1; list_searching=0; query[0]=0; sel=0; list_top=0; }
        else if(ch=='D')start_deep_search();
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
                if(show_count<=0)sel=0;
                else {
                    if(sel<0)sel=0;
                    if(sel>=show_count)sel=show_count-1;
                }
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
                snprintf(status,sizeof(status),"Press i to search Apple Podcasts.");
            }
        }
        else if(ch=='r'){
            resume_selected_episode();
        }
        else if(ch==' ')toggle_pause();
        else if((ch=='\n'||ch=='\r'||ch==KEY_ENTER)&&count>0&&list_item_selectable(sel)){
            if(mode==0)open_search_result(sel);
            else play_url(eps[sel].audio);
        }
    }

    update_progress();
    save_current_resume();

    network_ui_ready=0;
    endwin();
    if(mpv_pid>0)kill(mpv_pid,SIGTERM);
    unlink(mpv_socket);
    curl_global_cleanup();
    return 0;
}
