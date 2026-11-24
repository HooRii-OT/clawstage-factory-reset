#include "runtime.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
static int allowed = 1, calls;
int __wrap_sl_media_consume(json_object *userinfo, const char *id, const char *binding)
{
    assert(!strcmp(sl_string(userinfo,"hub_id"),"stage")); assert(strlen(id)==43); assert(strlen(binding)==64);
    calls++; return allowed;
}
int main(void)
{
    sl_runtime r = {0}; r.session.state=2;
    memset(r.session.transaction,'t',16); memset(r.session.initiator_id,'i',16); memset(r.responder_id,'r',16);
    r.session.initiator_public[0]=4; memset(r.session.initiator_public+1,'p',64);
    r.session.ephemeral_public[0]=4; memset(r.session.ephemeral_public+1,'x',32); memset(r.session.ephemeral_public+33,'y',32);
    char *hash=sl_media_binding(&r,"stage");
    assert(!strcmp(hash,"f586db5a91851d1fb00733bb7ec3349cdf5ebe43add253c12e68d8a8b7bcb431"));g_free(hash);
    char *root=g_dir_make_tmp("media-sync-test-XXXXXX",NULL);assert(root);
    snprintf(r.config,sizeof(r.config),"%s/config.json",root);
    json_object *config=json_tokener_parse("{\"userinfo\":{\"hub_id\":\"stage\",\"hub_token\":\"fixture-token\",\"union_id\":\"owner\"}}");
    assert(sl_json_write(r.config,config));json_object_put(config);
    hash=g_compute_checksum_for_string(G_CHECKSUM_SHA256,"owner:stage",-1);
    json_object *owner=json_object_new_object();json_object_object_add(owner,"scope",json_object_new_string(hash));g_free(hash);
    char *owner_path=g_build_filename(root,"byok-owner.json",NULL);assert(sl_json_write(owner_path,owner));json_object_put(owner);
    char *profile_path=g_build_filename(root,"byok-profile.json",NULL);
    json_object *profile=json_tokener_parse("{\"updatedAt\":12,\"asr\":{\"vendor\":\"deepgram\",\"apiKey\":\"fixture-asr\",\"extra\":\"no\"},\"tts\":{\"apiKey\":\"fixture-tts\"},\"llm\":{\"apiKey\":\"no\"}}");
    assert(sl_json_write(profile_path,profile));json_object_put(profile);
    json_object *request=json_tokener_parse("{\"operation\":\"read-media\",\"protocolVersion\":2,\"hubId\":\"stage\",\"authorizationId\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}");
    json_object *result=sl_media_read(&r,request,root),*value=NULL;assert(result && calls==1);
    assert(!json_object_object_get_ex(result,"llm",&value));
    assert(!json_object_object_get_ex(sl_section(result,"asr"),"extra",&value));json_object_put(result);
    allowed=0;assert(!sl_media_read(&r,request,root));allowed=1;
    json_object_object_add(request,"hubId",json_object_new_string("another"));assert(!sl_media_read(&r,request,root));
    json_object_object_add(request,"hubId",json_object_new_string("stage"));
    json_object_object_add(request,"hubToken",json_object_new_string("legacy"));assert(!sl_media_read(&r,request,root));
    json_object_put(request);remove(r.config);remove(owner_path);remove(profile_path);rmdir(root);
    g_free(root);g_free(owner_path);g_free(profile_path);puts("PASS native media read transcript, cloud gate and whitelist");return 0;
}
