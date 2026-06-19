#include <ncurses.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define W 60
#define H 22
#define MAXE 12
#define MAXJ 20

typedef struct { int x,y,alive; } Ent;
typedef struct { int x,y,dx,dy,alive; char c; } Junk;

Ent e[MAXE];
Junk j[MAXJ];
int px=W/2, py=H/2, hp=10, score=0, tick=0;

int blocked(int x,int y){ return x<=0||y<=0||x>=W-1||y>=H-1; }

void spawn_enemy(){
    for(int i=0;i<MAXE;i++) if(!e[i].alive){
        e[i]=(Ent){1+rand()%(W-2),1+rand()%(H-2),1};
        return;
    }
}

void throw_junk(int dx,int dy){
    for(int i=0;i<MAXJ;i++) if(!j[i].alive){
        j[i]=(Junk){px+dx,py+dy,dx,dy,1,(rand()%2?'b':'c')};
        return;
    }
}

void draw(){
    erase();
    box(stdscr,0,0);
    mvprintw(0,2," simplegame  HP:%d  SCORE:%d  q quit ",hp,score);

    for(int i=0;i<MAXJ;i++) if(j[i].alive) mvaddch(j[i].y,j[i].x,j[i].c);
    for(int i=0;i<MAXE;i++) if(e[i].alive) mvaddch(e[i].y,e[i].x,'d');
    mvaddch(py,px,'@');

    mvprintw(H,2,"arrows/hjkl move | wasd throw bottle/chair");
    refresh();
}

void update_junk(){
    for(int i=0;i<MAXJ;i++) if(j[i].alive){
        j[i].x += j[i].dx;
        j[i].y += j[i].dy;
        if(blocked(j[i].x,j[i].y)){ j[i].alive=0; continue; }

        for(int k=0;k<MAXE;k++) if(e[k].alive && e[k].x==j[i].x && e[k].y==j[i].y){
            e[k].alive=0;
            j[i].alive=0;
            score += 10;
        }
    }
}

void update_enemies(){
    for(int i=0;i<MAXE;i++) if(e[i].alive){
        int dx=(px>e[i].x)-(px<e[i].x);
        int dy=(py>e[i].y)-(py<e[i].y);

        if(rand()%2) e[i].x += dx;
        else e[i].y += dy;

        if(e[i].x==px && e[i].y==py) hp--;
    }
}

int main(){
    srand(time(NULL));
    initscr(); noecho(); curs_set(0); keypad(stdscr,1); timeout(70);

    for(int i=0;i<5;i++) spawn_enemy();

    while(hp>0){
        int ch=getch();
        int nx=px, ny=py;

        if(ch=='q') break;
        else if(ch==KEY_UP||ch=='k') ny--;
        else if(ch==KEY_DOWN||ch=='j') ny++;
        else if(ch==KEY_LEFT||ch=='h') nx--;
        else if(ch==KEY_RIGHT||ch=='l') nx++;
        else if(ch=='w') throw_junk(0,-1);
        else if(ch=='s') throw_junk(0,1);
        else if(ch=='a') throw_junk(-1,0);
        else if(ch=='d') throw_junk(1,0);

        if(!blocked(nx,ny)){ px=nx; py=ny; }

        update_junk();
        if(tick++%5==0) update_enemies();
        if(tick%40==0) spawn_enemy();

        draw();
    }

    erase();
    mvprintw(H/2,W/2-8,"LAST CALL. Score: %d",score);
    mvprintw(H/2+2,W/2-10,"press any key");
    timeout(-1); getch();
    endwin();
}
