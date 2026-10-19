#define main simpleradio_program_main
#include "../simpleradio.c"
#undef main

#include <assert.h>

int main(void)
{
    int sockets[2];
    char response[256];
    static const char json[] =
        "{\"data\":\"Station title\",\"request_id\":99}\n";

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int flags = fcntl(sockets[0], F_GETFL, 0);
    assert(flags >= 0);
    assert(fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) == 0);
    assert(write(sockets[1], json, sizeof json - 1) ==
           (ssize_t)(sizeof json - 1));
    assert(mpv_read_response(sockets[0], response, sizeof response) > 0);
    assert(strstr(response, "Station title"));

    long long started = monotonic_millis();
    assert(mpv_read_response(sockets[0], response, sizeof response) == 0);
    assert(monotonic_millis() - started < 100);

    static const char snapshot[] =
        "{\"data\":\"Combined title\",\"request_id\":201}\n"
        "{\"data\":42.5,\"request_id\":203}\n"
        "{\"data\":false,\"request_id\":205}\n";
    char line[256];
    double position = 0.0;
    bool paused_for_cache = true;
    assert(copy_mpv_response_line(snapshot, 201, line, sizeof(line)));
    char *title = json_data_string(line);
    assert(title && strcmp(title, "Combined title") == 0);
    free(title);
    assert(copy_mpv_response_line(snapshot, 203, line, sizeof(line)));
    assert(json_data_double(line, &position) && position == 42.5);
    assert(copy_mpv_response_line(snapshot, 205, line, sizeof(line)));
    assert(json_data_bool(line, &paused_for_cache) && !paused_for_cache);

    close(sockets[0]);
    close(sockets[1]);
    const char *sample=getenv("SIMPLEPLAY_TEST_MEDIA");
    if(sample){
        char directory[]="/tmp/simpleradio-core-test.XXXXXX";
        assert(mkdtemp(directory));
        snprintf(mpv_socket_path,sizeof(mpv_socket_path),"%s/socket",directory);
        pid_t pid;
        assert(playback_spawn_mpv(sample,73,&pid)==0);
        bool idle=true;
        for(int n=0;n<150;n++){
            usleep(20000);
            if(mpv_get_bool_property("idle-active",&idle) && !idle)break;
        }
        assert(!idle);
        assert(mpv_command_raw("{\"command\":[\"set_property\",\"pause\",true]}"));
        bool is_paused=false;
        for(int n=0;n<50 && !is_paused;n++){
            usleep(10000);
            (void)mpv_get_bool_property("pause",&is_paused);
        }
        assert(is_paused);
        periodic_mpv_snapshot.sampled_at=0;
        const MpvPeriodicSnapshot *snapshot=refresh_periodic_mpv_snapshot(time(NULL));
        assert(snapshot->have_position);
        assert(snapshot->title[0]);
        assert(kill(pid,SIGTERM)==0);
        int status;
        assert(waitpid(pid,&status,0)==pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
        unlink(mpv_socket_path);mpv_socket_path[0]='\0';
        assert(rmdir(directory)==0);
    }
    return 0;
}
