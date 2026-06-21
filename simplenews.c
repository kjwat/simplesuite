#define _POSIX_C_SOURCE 200809L

#include <ncurses.h>
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>

#define RESPONSE_LIMIT (16u * 1024u * 1024u)
#define BODY_LIMIT (1024u * 1024u)
#define MAX_LINKS 256

typedef struct { char *data; size_t len, cap; } Buffer;
typedef struct { char *label, *url; } Link;
typedef struct {
    char *title, *date, *url, *source, *body;
    Link *links; size_t link_count, link_cap;
} Article;
typedef struct {
    char *url, *resolved_url, *tag, *title, *host, *error;
    Article *articles; size_t article_count, article_cap;
} Feed;
typedef enum { VIEW_FEEDS, VIEW_ARTICLES, VIEW_ARTICLE } View;
typedef struct {
    Feed *feeds; size_t feed_count, feed_cap;
    char *browser, *user_agent, config_dir[4096], cache_dir[4096];
    long timeout; size_t max_articles;
    View view; size_t feed_sel, article_sel, top; int article_scroll, show_failed;
    pthread_t refresh_thread;
    pthread_mutex_t lock;
    int refreshing, refresh_thread_started;
    _Atomic int stop_refresh;
    size_t refresh_index, refresh_done, refresh_ok;
    char status[512];
} App;

static char *xstrndup(const char *s, size_t n) {
    char *p = malloc(n + 1); if (!p) return NULL;
    memcpy(p, s, n); p[n] = '\0'; return p;
}
static char *xstrdup(const char *s) { return xstrndup(s ? s : "", strlen(s ? s : "")); }
static void replace(char **p, char *v) { free(*p); *p = v; }

static int buf_reserve(Buffer *b, size_t need) {
    if (need <= b->cap) return 1;
    size_t n = b->cap ? b->cap : 256;
    while (n < need) { if (n > SIZE_MAX / 2) return 0; n *= 2; }
    char *p = realloc(b->data, n); if (!p) return 0;
    b->data = p; b->cap = n; return 1;
}
static int buf_addn(Buffer *b, const char *s, size_t n) {
    if (!buf_reserve(b, b->len + n + 1)) return 0;
    memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = 0; return 1;
}
static int buf_addc(Buffer *b, char c) { return buf_addn(b, &c, 1); }

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
static int ascii_eqn(const char *a, const char *b, size_t n) {
    for (size_t i=0;i<n;i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}
static const char *ci_find(const char *s, const char *end, const char *needle) {
    if (!s || !end || s >= end || !needle) return NULL;
    size_t n = strlen(needle); if (!n) return s;
    for (; s <= end && (size_t)(end - s) >= n; s++)
        if (ascii_eqn(s, needle, n)) return s;
    return NULL;
}
static int mkdir_one(const char *p) { return mkdir(p, 0700) == 0 || errno == EEXIST; }
static int mkdirs(const char *path) {
    char p[4096]; if (strlen(path) >= sizeof p) return 0; strcpy(p, path);
    for (char *q=p+1; *q; q++) if (*q=='/') { *q=0; if (!mkdir_one(p)) return 0; *q='/'; }
    return mkdir_one(p);
}

static uint64_t url_hash(const char *s) {
    uint64_t h=UINT64_C(14695981039346656037);
    while (*s) { h ^= (unsigned char)*s++; h *= UINT64_C(1099511628211); } return h;
}
static void cache_path(const App *a, const Feed *f, char *out, size_t n) {
    snprintf(out,n,"%s/%016llx.xml",a->cache_dir,(unsigned long long)url_hash(f->url));
}
static char *read_file(const char *path, size_t *len) {
    FILE *fp=fopen(path,"rb"); if (!fp) return NULL;
    Buffer b={0}; char chunk[8192]; size_t n;
    while ((n=fread(chunk,1,sizeof chunk,fp))>0) {
        if (b.len+n>RESPONSE_LIMIT || !buf_addn(&b,chunk,n)) { free(b.data); fclose(fp); return NULL; }
    }
    if (ferror(fp)) { free(b.data); b.data=NULL; } fclose(fp);
    if (len) *len=b.len;
    return b.data;
}
static int write_atomic(const char *path, const char *data, size_t len) {
    char tmp[4300]; snprintf(tmp,sizeof tmp,"%s.tmp.%ld",path,(long)getpid());
    FILE *fp=fopen(tmp,"wb"); if (!fp) return 0;
    int ok=fwrite(data,1,len,fp)==len && fflush(fp)==0 && fclose(fp)==0;
    if (ok) ok=rename(tmp,path)==0;
    if (!ok) unlink(tmp);
    return ok;
}

static size_t curl_write(char *p, size_t sz, size_t nm, void *ud) {
    Buffer *b=ud; if (nm && sz>SIZE_MAX/nm) return 0; size_t n=sz*nm;
    if (b->len+n>RESPONSE_LIMIT || !buf_addn(b,p,n)) return 0;
    return n;
}
static int curl_should_stop(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    App *a = clientp;
    return a && a->stop_refresh;
}

static char *fetch_once(const App *a,const char *url,const char *user_agent,const char *accept,
        int browser_mode,size_t *len,long *status,char *effective,size_t effective_size,
        char *content_type,size_t type_size,char *err,size_t en) {
    CURL *c=curl_easy_init(); Buffer b={0}; long code=0; char curl_error[CURL_ERROR_SIZE]={0};
    struct curl_slist *headers=NULL;char accept_header[512];
    if (!c) { snprintf(err,en,"curl initialization failed"); return NULL; }
    snprintf(accept_header,sizeof accept_header,"Accept: %s",accept);headers=curl_slist_append(headers,accept_header);
    curl_easy_setopt(c,CURLOPT_URL,url); curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(c,CURLOPT_MAXREDIRS,10L); curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,a->timeout);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,a->timeout); curl_easy_setopt(c,CURLOPT_USERAGENT,user_agent);
    curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers); curl_easy_setopt(c,CURLOPT_ERRORBUFFER,curl_error);
    curl_easy_setopt(c,CURLOPT_ACCEPT_ENCODING,""); curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,curl_write);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,&b); curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"http,https");
    curl_easy_setopt(c,CURLOPT_REDIR_PROTOCOLS_STR,"http,https");curl_easy_setopt(c,CURLOPT_AUTOREFERER,1L);
    curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(c,CURLOPT_COOKIEFILE,"");
    curl_easy_setopt(c,CURLOPT_TCP_KEEPALIVE,1L);
    curl_easy_setopt(c,CURLOPT_TCP_NODELAY,1L);
    curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,curl_should_stop);
    curl_easy_setopt(c,CURLOPT_XFERINFODATA,(void*)a);
    if(browser_mode)curl_easy_setopt(c,CURLOPT_HTTP_VERSION,(long)CURL_HTTP_VERSION_1_1);
    CURLcode rc=curl_easy_perform(c);curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&code);
    char *value=NULL;curl_easy_getinfo(c,CURLINFO_EFFECTIVE_URL,&value);snprintf(effective,effective_size,"%s",value?value:url);
    value=NULL;curl_easy_getinfo(c,CURLINFO_CONTENT_TYPE,&value);snprintf(content_type,type_size,"%s",value?value:"");
    curl_easy_cleanup(c);curl_slist_free_all(headers);*status=code;
    if (rc!=CURLE_OK) { snprintf(err,en,"%s",curl_error[0]?curl_error:curl_easy_strerror(rc)); free(b.data); return NULL; }
    if (code<200 || code>=300) { snprintf(err,en,"HTTP %ld at %.180s",code,effective); free(b.data); return NULL; }
    if (!b.len) { snprintf(err,en,"empty response at %.180s",effective); free(b.data); return NULL; }
    *len=b.len; return b.data;
}
static char *slash_variant(const char *url) {
    size_t n=strlen(url);const char *suffix=strpbrk(url,"?#");size_t path_end=suffix?(size_t)(suffix-url):n;
    const char *scheme=strstr(url,"://");size_t root=scheme?(size_t)(scheme-url)+3:0;
    if(path_end==0)return NULL;
    Buffer b={0};
    if(url[path_end-1]=='/'){if(path_end<=root)return NULL;buf_addn(&b,url,path_end-1);}
    else{buf_addn(&b,url,path_end);buf_addc(&b,'/');}
    if(suffix)buf_addn(&b,suffix,n-path_end);
    return b.data;
}

static char *www_variant(const char *url) {
    const char *scheme = strstr(url, "://");
    if (!scheme) return NULL;

    const char *host = scheme + 3;
    const char *host_end = host + strcspn(host, "/?#");
    Buffer b = {0};

    buf_addn(&b, url, (size_t)(host - url));

    if ((size_t)(host_end - host) > 4 && ascii_eqn(host, "www.", 4)) {
        buf_addn(&b, host + 4, (size_t)(host_end - host - 4));
    } else {
        buf_addn(&b, "www.", 4);
        buf_addn(&b, host, (size_t)(host_end - host));
    }

    buf_addn(&b, host_end, strlen(host_end));
    return b.data;
}
static char *fetch(const App *a,const char *url,size_t *len,char *effective,size_t effective_size,
        char *content_type,size_t type_size,char *err,size_t en) {
    const char *agents[]={
        a->user_agent && *a->user_agent ? a->user_agent : "Mozilla/5.0 (X11; Linux x86_64; rv:151.0) Gecko/20100101 Firefox/151.0",
        "Mozilla/5.0 (X11; Linux x86_64; rv:151.0) Gecko/20100101 Firefox/151.0",
        "Newsboat/2.43",
        "SimpleNews/0.1 (RSS reader)"};
    static const char *accepts[]={
        "application/rss+xml, application/atom+xml, application/xml, text/xml, */*;q=0.8",
        "application/rss+xml, application/atom+xml, application/xml, text/xml, text/html;q=0.9, */*;q=0.8",
        "*/*",
        "*/*"};
    char *variant=slash_variant(url);
    char *www=www_variant(url);
    const char *urls[]={url,variant,www};
    char last[256]="request failed";char *saved_html=NULL;size_t saved_len=0;char saved_effective[4096]="",saved_type[256]="";
    for(size_t u=0;u<3;u++){if(!urls[u])continue;for(size_t i=0;i<4;i++){
        long status=0;char *body=fetch_once(a,urls[u],agents[i],accepts[i],i==1,len,&status,effective,effective_size,content_type,type_size,last,sizeof last);
        if(body){size_t probe=*len<4096?*len:4096;int html=ci_find(body,body+probe,"<html")||ci_find(body,body+probe,"<!doctype html");if(!html){free(saved_html);free(variant);free(www);return body;}if(!saved_html){saved_html=body;saved_len=*len;snprintf(saved_effective,sizeof saved_effective,"%s",effective);snprintf(saved_type,sizeof saved_type,"%s",content_type);}else free(body);continue;}
        if(status!=0&&status!=400&&status!=403&&status!=404&&status!=406&&status!=429&&status<500)break;
    }}
    if(saved_html){*len=saved_len;snprintf(effective,effective_size,"%s",saved_effective);snprintf(content_type,type_size,"%s",saved_type);free(variant);free(www);return saved_html;}
    snprintf(err,en,"%s",last);free(variant);free(www);return NULL;
}

static void link_free(Link *l) { free(l->label); free(l->url); }
static void article_free(Article *a) {
    free(a->title); free(a->date); free(a->url); free(a->source); free(a->body);
    for(size_t i=0;i<a->link_count;i++) link_free(&a->links[i]);
    free(a->links); memset(a,0,sizeof *a);
}
static void feed_clear(Feed *f) {
    if (!f) return;
    for(size_t i=0;i<f->article_count;i++) article_free(&f->articles[i]);
    free(f->articles); f->articles=NULL; f->article_count=f->article_cap=0;
}
static void feed_free(Feed *f) {
    feed_clear(f);
    free(f->url); free(f->resolved_url); free(f->tag); free(f->title); free(f->host); free(f->error);
    memset(f,0,sizeof *f);
}
static int add_link(Article *a, const char *label, const char *url) {
    if (!url || !*url || a->link_count>=MAX_LINKS) return 1;
    for(size_t i=0;i<a->link_count;i++) if (!strcmp(a->links[i].url,url)) return 1;
    if (a->link_count==a->link_cap) { size_t n=a->link_cap?a->link_cap*2:8; Link *p=realloc(a->links,n*sizeof *p); if(!p)return 0; a->links=p;a->link_cap=n; }
    Link *l=&a->links[a->link_count++]; l->label=xstrdup(label&&*label?label:url); l->url=xstrdup(url);
    return l->label&&l->url;
}

static int utf8_put(Buffer *b, unsigned long cp) {
    char x[4]; size_t n;
    if(cp<=0x7f){x[0]=(char)cp;n=1;} else if(cp<=0x7ff){x[0]=(char)(0xc0|(cp>>6));x[1]=(char)(0x80|(cp&63));n=2;}
    else if(cp<=0xffff){x[0]=(char)(0xe0|(cp>>12));x[1]=(char)(0x80|((cp>>6)&63));x[2]=(char)(0x80|(cp&63));n=3;}
    else if(cp<=0x10ffff){x[0]=(char)(0xf0|(cp>>18));x[1]=(char)(0x80|((cp>>12)&63));x[2]=(char)(0x80|((cp>>6)&63));x[3]=(char)(0x80|(cp&63));n=4;} else return 0;
    return buf_addn(b,x,n);
}
static void decode_entity(Buffer *b, const char *s, size_t n) {
    if(n==3&&!memcmp(s,"amp",3))buf_addc(b,'&'); else if(n==2&&!memcmp(s,"lt",2))buf_addc(b,'<');
    else if(n==2&&!memcmp(s,"gt",2))buf_addc(b,'>'); else if(n==4&&!memcmp(s,"quot",4))buf_addc(b,'"');
    else if(n==4&&!memcmp(s,"apos",4))buf_addc(b,'\''); else if(n>1&&s[0]=='#'){
        char t[32]; if(n<sizeof t){memcpy(t,s+1,n-1);t[n-1]=0;char *e;unsigned long cp=strtoul(t,&e,(t[0]=='x'||t[0]=='X')?16:10);if(*e==0&&cp)utf8_put(b,cp);}
    } else {buf_addc(b,'&');buf_addn(b,s,n);buf_addc(b,';');}
}

static char *clean_utf8_punct(char *s) {
    if (!s) return s;
    Buffer b = {0};
    for (size_t i = 0; s[i]; ) {
        unsigned char c = (unsigned char)s[i];

        if (c == 0xE2 && (unsigned char)s[i+1] == 0x80) {
            unsigned char d = (unsigned char)s[i+2];

            if (d == 0x98 || d == 0x99) { buf_addc(&b, '\''); i += 3; continue; }
            if (d == 0x9C || d == 0x9D) { buf_addc(&b, '"');  i += 3; continue; }
            if (d == 0x93 || d == 0x94) { buf_addc(&b, '-');  i += 3; continue; }
            if (d == 0xA6)              { buf_addn(&b, "...", 3); i += 3; continue; }
        }

        buf_addc(&b, s[i++]);
    }

    free(s);
    return b.data ? b.data : xstrdup("");
}

static char *decode_text(const char *s, size_t n) {
    Buffer b={0}; size_t i=0; int space=1;
    if(n>=12&&!memcmp(s,"<![CDATA[",9)&&!memcmp(s+n-3,"]]>",3)){s+=9;n-=12;}
    while(i<n){
        if(s[i]=='&'){const char *q=memchr(s+i+1,';',n-i-1);if(q){decode_entity(&b,s+i+1,(size_t)(q-s-i-1));i=(size_t)(q-s)+1;space=0;continue;}}
        unsigned char c=(unsigned char)s[i++]; if(isspace(c)){if(!space){buf_addc(&b,' ');space=1;}}else{buf_addc(&b,(char)c);space=0;}
    }
    while(b.len&&b.data[b.len-1]==' ')b.data[--b.len]=0;
    return clean_utf8_punct(b.data?b.data:xstrdup(""));
}
static char *attr_value(const char *s, const char *end, const char *name) {
    const char *p=s; size_t nl=strlen(name);
    while((p=ci_find(p,end,name))){
        if(p>s&&(isalnum((unsigned char)p[-1])||p[-1]=='-'||p[-1]=='_')){p++;continue;}
        const char *q=p+nl;while(q<end&&isspace((unsigned char)*q))q++;if(q>=end||*q!='='){p++;continue;}q++;
        while(q<end&&isspace((unsigned char)*q))q++;
        if(q>=end)return NULL;
        char quote=(*q=='\''||*q=='"')?*q++:0;const char *v=q;
        while(q<end&&(quote?*q!=quote:!isspace((unsigned char)*q)&&*q!='>'))q++;
        return decode_text(v,(size_t)(q-v));
    } return NULL;
}
static int block_tag(const char *s,size_t n){
    static const char *tags[]={"br","p","div","li","tr","h1","h2","h3","blockquote",NULL};
    while(n&&(*s=='/'||isspace((unsigned char)*s)))s++,n--;
    for(int i=0;tags[i];i++){size_t z=strlen(tags[i]);if(n>=z&&ascii_eqn(s,tags[i],z)&&(n==z||isspace((unsigned char)s[z])||s[z]=='/'||s[z]=='>'))return 1;}
    return 0;
}
static char *html_plain(const char *s, Article *a) {
    Buffer b={0}; size_t n=strlen(s),i=0; int ws=1;
    while(i<n){
        if(s[i]=='<'){const char *q=memchr(s+i,'>',n-i);if(!q)break;size_t tn=(size_t)(q-(s+i+1));
            if(tn>=1&&tolower((unsigned char)s[i+1])=='a'){char *u=attr_value(s+i+2,q,"href");if(u){add_link(a,u,u);free(u);}}
            if(block_tag(s+i+1,tn)&&b.len&&b.data[b.len-1]!='\n'){buf_addc(&b,'\n');ws=1;}i=(size_t)(q-s)+1;continue;}
        if(s[i]=='&'){const char *q=memchr(s+i+1,';',n-i-1);if(q){decode_entity(&b,s+i+1,(size_t)(q-s-i-1));i=(size_t)(q-s)+1;ws=0;continue;}}
        unsigned char c=(unsigned char)s[i++];if(isspace(c)){if(!ws){buf_addc(&b,' ');ws=1;}}else{buf_addc(&b,(char)c);ws=0;}
    }
    while(b.len&&(b.data[b.len-1]==' '||b.data[b.len-1]=='\n'))b.data[--b.len]=0;
    return clean_utf8_punct(b.data?b.data:xstrdup(""));
}

static const char *tag_end(const char *p, const char *end) {
    if (!p || !end || p >= end) return NULL;
    char quote=0;
    for(;p<end;p++){
        if(quote){if(*p==quote)quote=0;}
        else if(*p=='\''||*p=='"')quote=*p;
        else if(*p=='>')return p;
    }
    return NULL;
}
static const char *find_tag(const char *s,const char *end,const char *wanted,int closing,const char **gt) {
    if (!s || !end || s >= end || !wanted) return NULL;
    const char *want=strrchr(wanted,':');want=want?want+1:wanted;size_t wn=strlen(want);
    while(s < end && (s=memchr(s,'<',(size_t)(end-s)))){
        const char *p=s+1;if(p>=end)return NULL;
        if(*p=='/'){if(!closing){s=p+1;continue;}p++;}else if(closing){s=p;continue;}
        while(p<end&&isspace((unsigned char)*p))p++;
        if(p>=end||*p=='!'||*p=='?'){s=p;continue;}
        const char *start=p,*local=p;
        while(p<end&&(isalnum((unsigned char)*p)||*p=='_'||*p=='-'||*p=='.'||*p==':')){if(*p==':')local=p+1;p++;}
        if(p==start){s=start;continue;}
        size_t ln=(size_t)(p-local);
        if(ln==wn&&ascii_eqn(local,want,wn)&&(p==end||isspace((unsigned char)*p)||*p=='/'||*p=='>')){
            const char *finish=tag_end(p,end);if(!finish)return NULL;if(gt)*gt=finish;return s;
        }
        s=p;
    }
    return NULL;
}
static char *element(const char *s,const char *end,const char *name) {
    if (!s || !end || s >= end || !name) return NULL;
    const char *open_end,*close_end;const char *p=find_tag(s,end,name,0,&open_end);if(!p||!open_end||open_end+1>=end)return NULL;
    const char *q=find_tag(open_end+1,end,name,1,&close_end);(void)close_end;if(!q||q<open_end+1)return NULL;
    return xstrndup(open_end+1,(size_t)(q-open_end-1));
}
static char *first_element(const char *s,const char *end,const char **names) {
    for(size_t i=0;names[i];i++){char *v=element(s,end,names[i]);if(v)return v;}return NULL;
}
static char *atom_link(const char *s,const char *end) {
    const char *p=s,*q,*fallback=NULL;while((p=find_tag(p,end,"link",0,&q))){
        char *href=attr_value(p,q,"href"),*rel=attr_value(p,q,"rel");if(href&&(!rel||!strcmp(rel,"alternate"))){free(rel);free((char*)fallback);return href;}if(href&&!fallback)fallback=xstrdup(href);free(href);free(rel);p=q+1;}return (char*)fallback;
}
static int ci_contains(const char *s,const char *needle) {
    return s&&ci_find(s,s+strlen(s),needle)!=NULL;
}
static char *site_root(const char *url) {
    const char *scheme=strstr(url,"://");if(!scheme)return NULL;const char *host=scheme+3;
    const char *end=host+strcspn(host,"/?#");Buffer b={0};buf_addn(&b,url,(size_t)(end-url));buf_addc(&b,'/');return b.data;
}
static char *absolute_url(const char *base,const char *href) {
    if(!base||!*base||!href||!*href||href[0]=='#'||ci_contains(href,"javascript:"))return NULL;
    if(ci_find(href,href+strlen(href),"http://")==href||ci_find(href,href+strlen(href),"https://")==href)return xstrdup(href);
    const char *scheme=strstr(base,"://");if(!scheme)return NULL;const char *host=scheme+3;const char *authority_end=host+strcspn(host,"/?#");Buffer b={0};
    if(href[0]=='/'&&href[1]=='/'){buf_addn(&b,base,(size_t)(scheme-base)+1);buf_addn(&b,href,strlen(href));return b.data;}
    if(href[0]=='/'){buf_addn(&b,base,(size_t)(authority_end-base));buf_addn(&b,href,strlen(href));return b.data;}
    const char *base_end=base+strcspn(base,"?#");const char *slash=base_end;while(slash>authority_end&&slash[-1]!='/')slash--;
    buf_addn(&b,base,(size_t)(slash-base));buf_addn(&b,href,strlen(href));return b.data;
}
static char *discover_feed_url(const char *html,const char *base) {
    const char *end=html+strlen(html),*p=html,*gt;
    while((p=find_tag(p,end,"link",0,&gt))){
        char *rel=attr_value(p,gt,"rel"),*type=attr_value(p,gt,"type"),*href=attr_value(p,gt,"href");
        int alternate=ci_contains(rel,"alternate");int feed_type=ci_contains(type,"rss")||ci_contains(type,"atom")||ci_contains(type,"xml");
        if(href&&alternate&&feed_type){char *url=absolute_url(base,href);free(rel);free(type);free(href);return url;}
        free(rel);free(type);free(href);p=gt+1;
    }
    return NULL;
}
static char *discover_from_homepage(const App *a,const char *url,char *err,size_t en) {
    char *root=site_root(url);if(!root)return NULL;size_t len=0;char effective[4096],type[256],fetch_error[256];
    char *html=fetch(a,root,&len,effective,sizeof effective,type,sizeof type,fetch_error,sizeof fetch_error);free(root);
    if(!html){snprintf(err,en,"homepage request failed: %.200s",fetch_error);return NULL;}char *found=discover_feed_url(html,effective);free(html);
    if(!found)snprintf(err,en,"no RSS/Atom autodiscovery link on site homepage");
    return found;
}
static const char *feed_name(const Feed *f) {
    if (f->title && *f->title) return f->title;
    if (f->host && *f->host) return f->host;
    if (f->url && *f->url) return f->url;
    return f->tag && *f->tag ? f->tag : "(unnamed feed)";
}
static int parse_document(Feed *f,const char *xml,size_t len,size_t max,char *err,size_t en) {
    const char *end=xml+len;while(xml<end&&isspace((unsigned char)*xml))xml++;if(end-xml>=3&&(unsigned char)xml[0]==0xef&&(unsigned char)xml[1]==0xbb&&(unsigned char)xml[2]==0xbf)xml+=3;
    const char *container_end=NULL;const char *container=find_tag(xml,end,"feed",0,&container_end);
    const char *probe_end=NULL;int atom=container!=NULL||(!find_tag(xml,end,"item",0,&probe_end)&&find_tag(xml,end,"entry",0,&probe_end));
    if(!container)container=find_tag(xml,end,"channel",0,&container_end);
    const char *itemtag=atom?"entry":"item",*first_end=NULL;const char *first=find_tag(xml,end,itemtag,0,&first_end);(void)first_end;
    const char *title_end=first?first:end;
    const char *title_start=xml;
    if(container && container_end && container_end+1 < title_end) title_start=container_end+1;
    char *new_title=NULL,*ft=element(title_start,title_end,"title");if(ft){new_title=decode_text(ft,strlen(ft));free(ft);if(new_title&&!*new_title){free(new_title);new_title=NULL;}}
    Article *items=NULL;size_t count=0,cap=0;const char *p=xml;
    static const char *dates[]={"pubDate","published","updated","dc:date",NULL};static const char *bodies[]={"content:encoded","content","description","summary",NULL};
    while(count<max&&(p=find_tag(p,end,itemtag,0,&first_end))){const char *close_end=NULL;const char *q=find_tag(first_end+1,end,itemtag,1,&close_end);if(!q)break;Article a={0};
        char *raw=element(first_end+1,q,"title");a.title=raw?html_plain(raw,&a):xstrdup("(untitled)");free(raw);
        raw=first_element(first_end+1,q,dates);a.date=raw?decode_text(raw,strlen(raw)):xstrdup("");free(raw);
        raw=first_element(first_end+1,q,bodies);a.body=raw?html_plain(raw,&a):xstrdup("");free(raw);
        a.url=atom?atom_link(first_end+1,q):element(first_end+1,q,"link");if(!a.url&&!atom)a.url=atom_link(first_end+1,q);if(!a.url)a.url=element(first_end+1,q,"guid");if(a.url){char*d=decode_text(a.url,strlen(a.url));free(a.url);a.url=d;}
        if(!a.title || !*a.title || !strcmp(a.title,"(untitled)")){
            if(a.url && *a.url){
                replace(&a.title, xstrdup(a.url));
            }
        }

        a.source=xstrdup(new_title&&*new_title?new_title:feed_name(f));
        if(a.url)add_link(&a,a.title,a.url);

        if(a.title&&*a.title&&strcmp(a.title,"(untitled)")){
            if(count==cap){
                size_t nn=cap?cap*2:32;
                Article*z=realloc(items,nn*sizeof*z);
                if(!z){article_free(&a);break;}
                items=z;cap=nn;
            }
            items[count++]=a;
        }else article_free(&a);
        p=close_end+1;
    }
    if(!count&&container&&new_title){if(new_title)replace(&f->title,new_title);feed_clear(f);free(items);return 1;}
    if(!count){free(items);free(new_title);snprintf(err,en,"no usable %s elements",itemtag);return 0;}
    if(new_title)replace(&f->title,new_title);
    feed_clear(f);f->articles=items;f->article_count=count;f->article_cap=cap;return 1;
}

static char *url_hostname(const char *url) {
    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;
    const char *end = start + strcspn(start, "/?#");
    const char *at = NULL;
    for (const char *p = start; p < end; p++) if (*p == '@') at = p;
    if (at) start = at + 1;
    if (start < end && *start == '[') {
        const char *close = memchr(start + 1, ']', (size_t)(end - start - 1));
        if (close) return xstrndup(start + 1, (size_t)(close - start - 1));
    }
    const char *colon = memchr(start, ':', (size_t)(end - start));
    if (colon) end = colon;
    return end > start ? xstrndup(start, (size_t)(end - start)) : xstrdup("");
}
static int app_add_feed(App*a,const char*tag,const char*title,const char*url){
    if(a->feed_count==a->feed_cap){size_t n=a->feed_cap?a->feed_cap*2:16;Feed*p=realloc(a->feeds,n*sizeof*p);if(!p)return 0;a->feeds=p;a->feed_cap=n;}
    Feed*f=&a->feeds[a->feed_count++];memset(f,0,sizeof*f);f->url=xstrdup(url);f->tag=xstrdup(tag&&*tag?tag:"");f->title=xstrdup(title&&*title?title:"");f->host=url_hostname(url);return f->url&&f->tag&&f->title&&f->host;
}
static char *config_word(char **cursor) {
    char *p=*cursor;while(isspace((unsigned char)*p))p++;if(!*p){*cursor=p;return NULL;}
    Buffer b={0};char quote=0;if(*p=='\''||*p=='"')quote=*p++;
    while(*p&&(quote?*p!=quote:!isspace((unsigned char)*p))){if(*p=='\\'&&p[1]&&(quote||isspace((unsigned char)p[1])))p++;buf_addc(&b,*p++);}
    if(quote&&*p==quote)p++;
    *cursor=p;
    return b.data?b.data:xstrdup("");
}
static void load_urls(App*a){
    char path[4200];snprintf(path,sizeof path,"%s/urls",a->config_dir);FILE*fp=fopen(path,"r");if(!fp){snprintf(a->status,sizeof a->status,"Create %.480s",path);return;}
    char *line=NULL;size_t cap=0;while(getline(&line,&cap,fp)>=0){
        char*s=trim(line);if(!*s||*s=='#')continue;char*bar=strchr(s,'|');
        if(bar){
            *bar=0;
            char *left=trim(s), *right=trim(bar+1);
            int looks_like_tag=1;
            for(char *z=left; *z; z++){
                if(!(isupper((unsigned char)*z)||isdigit((unsigned char)*z)||isspace((unsigned char)*z)||*z=='-'||*z=='_')){
                    looks_like_tag=0;
                    break;
                }
            }
            if(looks_like_tag)
                app_add_feed(a,left,NULL,right);
            else
                app_add_feed(a,NULL,left,right);
            continue;
        }
        char *cursor=s,*url=config_word(&cursor);if(!url||!*url){free(url);continue;}Buffer tags={0};char *word;
        while((word=config_word(&cursor))){if(*word){if(tags.len)buf_addc(&tags,' ');buf_addn(&tags,word,strlen(word));}free(word);}
        app_add_feed(a,tags.data,NULL,url);free(tags.data);free(url);
    }free(line);fclose(fp);
}
static void load_config(App*a){
    a->browser=xstrdup("links");
    a->user_agent=xstrdup("Mozilla/5.0 (X11; Linux x86_64; rv:151.0) Gecko/20100101 Firefox/151.0");
    a->timeout=20;a->max_articles=200;char path[4200];snprintf(path,sizeof path,"%s/config",a->config_dir);FILE*fp=fopen(path,"r");if(!fp)return;
    char*line=NULL;size_t cap=0;while(getline(&line,&cap,fp)>=0){char*s=trim(line);if(!*s||*s=='#')continue;char*eq=strchr(s,'=');if(!eq)continue;*eq=0;char*k=trim(s),*v=trim(eq+1);if(!strcmp(k,"browser")){replace(&a->browser,xstrdup(v));}else if(!strcmp(k,"user_agent")){replace(&a->user_agent,xstrdup(v));}else if(!strcmp(k,"timeout")){long n=strtol(v,NULL,10);if(n>=1&&n<=300)a->timeout=n;}else if(!strcmp(k,"max_articles")){long n=strtol(v,NULL,10);if(n>=1&&n<=1000)a->max_articles=(size_t)n;}}
    free(line);fclose(fp);
}
static int load_cached(App*a,Feed*f){char path[4200],e[128];size_t n;cache_path(a,f,path,sizeof path);char*x=read_file(path,&n);if(!x)return 0;int ok=parse_document(f,x,n,a->max_articles,e,sizeof e);free(x);if(!ok)replace(&f->error,xstrdup(e));else replace(&f->error,NULL);return ok;}
static int refresh_feed(App*a,Feed*f){
    char e[256],path[4200],effective[4096],type[256];size_t n=0;const char *target=f->resolved_url&&*f->resolved_url?f->resolved_url:f->url;
    char*x=fetch(a,target,&n,effective,sizeof effective,type,sizeof type,e,sizeof e);
    if(!x&&target!=f->url)x=fetch(a,f->url,&n,effective,sizeof effective,type,sizeof type,e,sizeof e);
    char *discovered=NULL;
    if(!x){char first_error[256],discovery_error[256]="";snprintf(first_error,sizeof first_error,"%s",e);discovered=discover_from_homepage(a,f->url,discovery_error,sizeof discovery_error);if(discovered)x=fetch(a,discovered,&n,effective,sizeof effective,type,sizeof type,e,sizeof e);else snprintf(e,sizeof e,"%.100s; discovery: %.120s",first_error,discovery_error);}
    if(!x){replace(&f->error,xstrdup(e));free(discovered);return 0;}
    if(!parse_document(f,x,n,a->max_articles,e,sizeof e)){
        char *from_html=discover_feed_url(x,effective);free(x);x=NULL;
        if(from_html){free(discovered);discovered=from_html;x=fetch(a,discovered,&n,effective,sizeof effective,type,sizeof type,e,sizeof e);}
        if(!x||!parse_document(f,x,n,a->max_articles,e,sizeof e)){replace(&f->error,xstrdup(e));free(x);free(discovered);return 0;}
    }
    replace(&f->resolved_url,xstrdup(effective));cache_path(a,f,path,sizeof path);
    if(!a->refreshing && !write_atomic(path,x,n))snprintf(a->status,sizeof a->status,"Read feed; could not write cache");
    free(x);free(discovered);replace(&f->error,NULL);return 1;
}

static void shell_quote(Buffer*b,const char*s){buf_addc(b,'\'');for(;*s;s++){if(*s=='\'')buf_addn(b,"'\\''",4);else buf_addc(b,*s);}buf_addc(b,'\'');}
static void open_browser(App*a,const char*url){
    if(!url||!*url){snprintf(a->status,sizeof a->status,"No article URL");return;}Buffer cmd={0};const char*p=a->browser,*u;
    while((u=strstr(p,"%u"))){buf_addn(&cmd,p,(size_t)(u-p));shell_quote(&cmd,url);p=u+2;}buf_addn(&cmd,p,strlen(p));if(!strstr(a->browser,"%u")){buf_addc(&cmd,' ');shell_quote(&cmd,url);}
    def_prog_mode();endwin();pid_t pid=fork();if(pid==0){execl("/bin/sh","sh","-c",cmd.data,(char*)NULL);_exit(127);}if(pid>0){int st;while(waitpid(pid,&st,0)<0&&errno==EINTR){}}reset_prog_mode();refresh();free(cmd.data);
}
static size_t clip_utf8(const char*s,size_t max){size_t n=strlen(s);if(n<=max)return n;n=max;while(n&&((unsigned char)s[n]&0xc0)==0x80)n--;return n;}
static void put_clipped(int y,int x,const char*s,int width){if(width<=0)return;size_t n=clip_utf8(s?s:"",(size_t)width);mvaddnstr(y,x,s?s:"",(int)n);}
static void draw_rule(int y){int h,w;getmaxyx(stdscr,h,w);(void)h;for(int x=0;x<w;x++)mvaddch(y,x,ACS_HLINE);}
static size_t page_rows(void){int h,w;getmaxyx(stdscr,h,w);(void)w;return h>4?(size_t)(h-4):1;}
static void normalize_list(App*a,size_t count,size_t sel){size_t rows=page_rows();if(sel<a->top)a->top=sel;if(sel>=a->top+rows)a->top=sel-rows+1;if(!count)a->top=0;}

static int feed_visible(const App *a, size_t i) {
    if (i >= a->feed_count) return 0;

    if (a->feeds[i].error && !strcmp(a->feeds[i].error,"refreshing...")) return 1;

    /*
     * Default view is the clean reading list:
     * hide feeds that failed OR produced zero articles.
     * Press i to reveal the failed/empty feed morgue.
     */
    if (a->show_failed) return 1;

    return !a->feeds[i].error && a->feeds[i].article_count > 0;
}

static size_t visible_feed_count(const App *a) {
    size_t n = 0;
    for (size_t i = 0; i < a->feed_count; i++)
        if (feed_visible(a, i)) n++;
    return n;
}

static size_t visible_feed_index(const App *a, size_t rank) {
    size_t seen = 0, last = 0;
    for (size_t i = 0; i < a->feed_count && !a->stop_refresh; i++) {
        if (!feed_visible(a, i)) continue;
        last = i;
        if (seen == rank) return i;
        seen++;
    }
    return last;
}

static size_t feed_visible_rank(const App *a, size_t idx) {
    size_t rank = 0;
    for (size_t i = 0; i < a->feed_count && i < idx; i++)
        if (feed_visible(a, i)) rank++;
    return rank;
}

static void ensure_visible_feed(App *a) {
    if (!a->feed_count) return;
    if (feed_visible(a, a->feed_sel)) return;

    for (size_t i = a->feed_sel; i < a->feed_count; i++) {
        if (feed_visible(a, i)) {
            a->feed_sel = i;
            return;
        }
    }

    for (size_t i = a->feed_sel; i > 0; i--) {
        if (feed_visible(a, i - 1)) {
            a->feed_sel = i - 1;
            return;
        }
    }

    a->feed_sel = 0;
}

static void normalize_feeds(App *a) {
    size_t total = visible_feed_count(a);
    size_t rows = page_rows();

    if (!total) {
        a->top = 0;
        return;
    }

    ensure_visible_feed(a);
    size_t rank = feed_visible_rank(a, a->feed_sel);

    if (rank < a->top) a->top = rank;
    if (rank >= a->top + rows) a->top = rank - rows + 1;
}
static int visual_lines(const char *text, int width) {
    int lines = 0;
    const char *p = text;
    if (width < 1) width = 1;
    while (*p) {
        const char *q = strchr(p, '\n');
        size_t n = q ? (size_t)(q - p) : strlen(p);
        if (!n) lines++;
        while (n) {
            size_t take = clip_utf8(p, n < (size_t)width ? n : (size_t)width);
            if (!take) take = n < (size_t)width ? n : (size_t)width;
            p += take; n -= take; lines++;
        }
        if (!q) break;
        p = q + 1;
    }
    return lines ? lines : 1;
}
static void draw_wrapped(const char *text, int scroll, int first_y, int last_y, int width) {
    const char *p = text;
    int line = 0, y = first_y;
    if (width < 1) width = 1;
    while (*p && y < last_y) {
        const char *q = strchr(p, '\n');
        size_t left = q ? (size_t)(q - p) : strlen(p);
        if (!left) {
            if (line++ >= scroll) y++;
        }
        while (left && y < last_y) {
            size_t take = clip_utf8(p, left < (size_t)width ? left : (size_t)width);
            if (!take) take = left < (size_t)width ? left : (size_t)width;
            if (line++ >= scroll) mvaddnstr(y++, 0, p, (int)take);
            p += take; left -= take;
        }
        if (!q) break;
        p = q + 1;
    }
}
static void draw(App*a){
    erase();int h,w;getmaxyx(stdscr,h,w);const char*heading="Feeds";Feed*f=NULL;Article*ar=NULL;
    if(a->feed_count) f=&a->feeds[a->feed_sel];
    if(a->view!=VIEW_FEEDS&&f) heading=feed_name(f);
    if(a->view==VIEW_ARTICLE&&f&&f->article_count) ar=&f->articles[a->article_sel];
    attron(A_BOLD);put_clipped(0,0,heading,w);attroff(A_BOLD);draw_rule(1);
    if(a->view==VIEW_FEEDS){
        normalize_feeds(a);
        size_t total = visible_feed_count(a);

        if(!total){
            put_clipped(2,0,a->show_failed ? "No feeds." : "No working feeds. Press i to show failed feeds.",w);
        }

        for(int y=2;y<h-2 && (size_t)(y-2)+a->top<total;y++){
            size_t rank=(size_t)(y-2)+a->top;
            size_t i=visible_feed_index(a,rank);
            if(i==a->feed_sel)attron(A_REVERSE);

            char line[2048];
            Feed*x=&a->feeds[i];

            if(x->error && !strcmp(x->error,"refreshing..."))
                snprintf(line,sizeof line,"* [%zu/%zu] %s  (%zu)  refreshing...",i+1,a->feed_count,feed_name(x),x->article_count);
            else if(x->error)
                snprintf(line,sizeof line,"%s  (%zu)  !",feed_name(x),x->article_count);
            else
                snprintf(line,sizeof line,"%s  (%zu)",feed_name(x),x->article_count);

            put_clipped(y,0,line,w);
            if(i==a->feed_sel)attroff(A_REVERSE);
        }}
    else if(a->view==VIEW_ARTICLES&&f){normalize_list(a,f->article_count,a->article_sel);for(int y=2;y<h-2&&(size_t)(y-2)+a->top<f->article_count;y++){size_t i=(size_t)(y-2)+a->top;if(i==a->article_sel)attron(A_REVERSE);put_clipped(y,0,f->articles[i].title,w);if(i==a->article_sel)attroff(A_REVERSE);}}
    else if(ar){Buffer b={0};char head[4096];snprintf(head,sizeof head,"Title: %s\nDate: %s\nSource: %s\nURL: %s\n\n%s\n\nLinks:\n",ar->title,ar->date,ar->source,ar->url?ar->url:"",ar->body);buf_addn(&b,head,strlen(head));for(size_t i=0;i<ar->link_count;i++){char num[32];snprintf(num,sizeof num,"[%zu] ",i+1);buf_addn(&b,num,strlen(num));buf_addn(&b,ar->links[i].url,strlen(ar->links[i].url));buf_addc(&b,'\n');}
        int total=visual_lines(b.data?b.data:"",w), rows=h>4?h-4:1, max_scroll=total>rows?total-rows:0;
        if(a->article_scroll>max_scroll)a->article_scroll=max_scroll;
        draw_wrapped(b.data?b.data:"",a->article_scroll,2,h-2,w);free(b.data);}
    draw_rule(h-2);const char*help=a->view==VIEW_FEEDS?"Enter open  r refresh all  R refresh feed  i failed  q quit":a->view==VIEW_ARTICLES?"Enter open  Backspace back  o browser  R refresh":"Up/Down scroll  Backspace back  o browser";char failure[4096];const char*bottom=a->status[0]?a->status:help;if(!a->status[0]&&f&&f->error&&a->view!=VIEW_ARTICLE){snprintf(failure,sizeof failure,"%s | Failed: %s",f->url,f->error);bottom=failure;}put_clipped(h-1,0,bottom,w);refresh();
}

static void feed_swap_result(Feed *dst, Feed *src) {
    feed_clear(dst);
    dst->articles = src->articles;
    dst->article_count = src->article_count;
    dst->article_cap = src->article_cap;
    src->articles = NULL;
    src->article_count = src->article_cap = 0;

    replace(&dst->resolved_url, src->resolved_url); src->resolved_url = NULL;
    replace(&dst->title, src->title); src->title = NULL;
    replace(&dst->error, src->error); src->error = NULL;
}

typedef struct { App *app; size_t next; } RefreshPool;

static void *refresh_pool_worker(void *ud) {
    RefreshPool *pool = ud;
    App *a = pool->app;

    for (;;) {
        Feed tmp = {0};
        size_t i;

        pthread_mutex_lock(&a->lock);
        if (pool->next >= a->feed_count || a->stop_refresh) {
            pthread_mutex_unlock(&a->lock);
            break;
        }
        i = pool->next++;
        a->refresh_index = i + 1;
        snprintf(a->status, sizeof a->status, "Refreshing %zu/%zu...", i + 1, a->feed_count);
        tmp.url = xstrdup(a->feeds[i].url);
        tmp.resolved_url = xstrdup(a->feeds[i].resolved_url);
        tmp.tag = xstrdup(a->feeds[i].tag);
        tmp.title = xstrdup(a->feeds[i].title);
        tmp.host = xstrdup(a->feeds[i].host);
        replace(&a->feeds[i].error, xstrdup("refreshing..."));
        snprintf(a->status, sizeof a->status, "Refreshing: %zu / %zu", a->refresh_done, a->feed_count);
        pthread_mutex_unlock(&a->lock);

        int good = refresh_feed(a, &tmp);

        pthread_mutex_lock(&a->lock);
        if (good) {
            feed_swap_result(&a->feeds[i], &tmp);
            a->refresh_ok++;
        } else {
            replace(&a->feeds[i].error, xstrdup(tmp.error ? tmp.error : "refresh failed"));
        }
        a->refresh_done++;
        snprintf(a->status, sizeof a->status, "Refreshing: %zu / %zu", a->refresh_done, a->feed_count);
        pthread_mutex_unlock(&a->lock);

        feed_free(&tmp);
    }

    return NULL;
}

static void *refresh_worker(void *ud) {
    App *a = ud;
    RefreshPool pool = {a, 0};
    pthread_t workers[4];
    size_t count = a->feed_count < 4 ? a->feed_count : 4;
    size_t started = 0;

    while (started < count && pthread_create(&workers[started], NULL, refresh_pool_worker, &pool) == 0)
        started++;
    if (!started && count) refresh_pool_worker(&pool);
    for (size_t i = 0; i < started; i++) pthread_join(workers[i], NULL);

    pthread_mutex_lock(&a->lock);
    a->refreshing = 0;
    a->show_failed = 0;
    ensure_visible_feed(a);
    a->top = 0;
    snprintf(a->status, sizeof a->status,
             "Refreshed %zu/%zu feeds; %zu failed hidden - press i to show",
             a->refresh_ok, a->feed_count, a->feed_count - a->refresh_ok);
    pthread_mutex_unlock(&a->lock);

    return NULL;
}

static void start_refresh(App *a) {
    if (a->refreshing) {
        snprintf(a->status, sizeof a->status, "Already refreshing...");
        return;
    }

    if (a->refresh_thread_started) {
        pthread_join(a->refresh_thread, NULL);
        a->refresh_thread_started = 0;
    }

    a->stop_refresh = 0;
    a->refreshing = 1;
    a->refresh_index = 0;
    a->refresh_done = 0;
    a->refresh_ok = 0;

    if (pthread_create(&a->refresh_thread, NULL, refresh_worker, a) != 0) {
        a->refreshing = 0;
        snprintf(a->status, sizeof a->status, "Could not start refresh thread");
    } else a->refresh_thread_started = 1;
}

static Article *selected_article(App*a){if(a->feed_sel>=a->feed_count)return NULL;Feed*f=&a->feeds[a->feed_sel];if(a->article_sel>=f->article_count)return NULL;return &f->articles[a->article_sel];}
static void move_selection(App*a,int d){
    if(a->view==VIEW_ARTICLE){
        a->article_scroll+=d;
        if(a->article_scroll<0)a->article_scroll=0;
        return;
    }

    if(a->view==VIEW_FEEDS){
        size_t total=visible_feed_count(a);
        if(!total)return;

        ensure_visible_feed(a);
        size_t rank=feed_visible_rank(a,a->feed_sel);

        if(d<0&&rank)rank--;
        else if(d>0&&rank+1<total)rank++;

        a->feed_sel=visible_feed_index(a,rank);
        return;
    }

    size_t*n=&a->article_sel;
    size_t count=a->feeds[a->feed_sel].article_count;
    if(d<0&&*n)*n-=1;
    else if(d>0&&*n+1<count)*n+=1;
}
static void event_loop(App*a){
    for(int running=1;running;){pthread_mutex_lock(&a->lock);draw(a);pthread_mutex_unlock(&a->lock);int c=getch();pthread_mutex_lock(&a->lock);if(!a->refreshing)a->status[0]=0;if(c==KEY_UP||c=='k')move_selection(a,-1);else if(c==KEY_DOWN||c=='j')move_selection(a,1);
        else if(c=='g'){if(a->view==VIEW_ARTICLE)a->article_scroll=0;else if(a->view==VIEW_FEEDS)a->feed_sel=0;else a->article_sel=0;a->top=0;}
        else if(c=='G'){if(a->view==VIEW_ARTICLE)a->article_scroll=1000000;else if(a->view==VIEW_FEEDS&&a->feed_count)a->feed_sel=a->feed_count-1;else if(a->view==VIEW_ARTICLES&&a->feeds[a->feed_sel].article_count)a->article_sel=a->feeds[a->feed_sel].article_count-1;}
        else if(c=='\n'||c==KEY_ENTER||c=='\r'){if(a->view==VIEW_FEEDS&&visible_feed_count(a)){ensure_visible_feed(a);a->view=VIEW_ARTICLES;a->article_sel=a->top=0;}else if(a->view==VIEW_ARTICLES&&selected_article(a)){a->view=VIEW_ARTICLE;a->article_scroll=0;}}
        else if(c==KEY_BACKSPACE||c==127||c==8||c==KEY_LEFT||c=='h'){if(a->view==VIEW_ARTICLE)a->view=VIEW_ARTICLES;else if(a->view==VIEW_ARTICLES){a->view=VIEW_FEEDS;a->top=0;}}
        else if(c=='i'&&a->view==VIEW_FEEDS){
            a->show_failed=!a->show_failed;
            ensure_visible_feed(a);
            a->top=feed_visible_rank(a,a->feed_sel);
            snprintf(a->status,sizeof a->status,a->show_failed?"Showing failed feeds":"Hiding failed feeds");
        }
        else if(c=='o'){Article*ar=selected_article(a);open_browser(a,ar?ar->url:NULL);}
        else if(c=='R'&&a->feed_count){Feed*f=&a->feeds[a->feed_sel];snprintf(a->status,sizeof a->status,"Refreshing %s...",feed_name(f));draw(a);int ok=refresh_feed(a,f);if(ok)snprintf(a->status,sizeof a->status,"Refreshed %s",feed_name(f));else snprintf(a->status,sizeof a->status,"Refresh failed: %.220s | %.220s",f->error,f->url);}
        else if(c=='r')start_refresh(a);
        else if(c=='q'){a->stop_refresh=1;running=0;}
        pthread_mutex_unlock(&a->lock);
    }
}
static void app_free(App*a){pthread_mutex_lock(&a->lock);a->stop_refresh=1;pthread_mutex_unlock(&a->lock);if(a->refresh_thread_started)pthread_join(a->refresh_thread,NULL);pthread_mutex_destroy(&a->lock);for(size_t i=0;i<a->feed_count;i++)feed_free(&a->feeds[i]);free(a->feeds);free(a->browser);free(a->user_agent);}
int main(void){
    setlocale(LC_ALL,"");App a={0};const char*home=getenv("HOME"),*xc=getenv("XDG_CONFIG_HOME"),*xd=getenv("XDG_CACHE_HOME");if(!home&&!xc){fprintf(stderr,"simplenews: HOME is not set\n");return 1;}
    snprintf(a.config_dir,sizeof a.config_dir,"%s/simplenews",xc&&*xc?xc:home);if(!(xc&&*xc))snprintf(a.config_dir,sizeof a.config_dir,"%s/.config/simplenews",home);
    snprintf(a.cache_dir,sizeof a.cache_dir,"%s/simplenews",xd&&*xd?xd:home);if(!(xd&&*xd))snprintf(a.cache_dir,sizeof a.cache_dir,"%s/.cache/simplenews",home);
    if(!mkdirs(a.config_dir)||!mkdirs(a.cache_dir)){fprintf(stderr,"simplenews: cannot create configuration/cache directories\n");return 1;}load_config(&a);load_urls(&a);pthread_mutex_init(&a.lock,NULL);curl_global_init(CURL_GLOBAL_DEFAULT);size_t cached=0;for(size_t i=0;i<a.feed_count;i++)cached+=load_cached(&a,&a.feeds[i]);if(a.feed_count)snprintf(a.status,sizeof a.status,"Loaded %zu cached feeds; press r to refresh",cached);
    initscr();cbreak();noecho();keypad(stdscr,TRUE);timeout(100);curs_set(0);event_loop(&a);app_free(&a);curs_set(1);endwin();curl_global_cleanup();return 0;
}
