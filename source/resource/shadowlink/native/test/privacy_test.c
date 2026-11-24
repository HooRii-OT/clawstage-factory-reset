#define _GNU_SOURCE
#include "device.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d\n", __LINE__); exit(1); } } while (0)
int main(void)
{
    char path[]="/tmp/privacy-test-XXXXXX"; int fd=mkstemp(path); CHECK(fd>=0); close(fd);
    struct { const char *json; int legacy, camera, microphone; } cases[] = {
        {"{\"schema_version\":2,\"mute_stat\":1,\"camera_enabled\":true,\"microphone_enabled\":true}",1,1,1},
        {"{\"schema_version\":2,\"mute_stat\":0,\"camera_enabled\":false,\"microphone_enabled\":true}",0,0,1},
        {"{\"schema_version\":2,\"mute_stat\":0,\"camera_enabled\":false,\"microphone_enabled\":false}",0,0,0},
        {"{\"schema_version\":2,\"mute_stat\":1,\"camera_enabled\":true,\"microphone_enabled\":false}",1,1,0},
        {"{\"mute_stat\":0}",0,-1,-1},
        {"{\"schema_version\":2,\"camera_enabled\":true}",-1,-1,-1},
        {"{\"schema_version\":2,\"camera_enabled\":true,\"microphone_enabled\":0}",-1,-1,-1},
        {"{\"schema_version\":1,\"camera_enabled\":true,\"microphone_enabled\":true}",-1,-1,-1},
        {"{\"mute_stat\":true}",-1,-1,-1}, {"bad json",-1,-1,-1}
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
        FILE*f=fopen(path,"w"); CHECK(f); fputs(cases[i].json,f); fclose(f);
        int states[3]; sl_privacy_states(path,states);
        CHECK(states[0]==cases[i].legacy && states[1]==cases[i].camera && states[2]==cases[i].microphone);
    }
    unlink(path); int states[3]; sl_privacy_states(path,states);
    CHECK(states[0]==-1 && states[1]==-1 && states[2]==-1); return 0;
}
