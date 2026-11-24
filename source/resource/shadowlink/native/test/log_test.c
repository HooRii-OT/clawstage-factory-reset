#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "log test failed at line %d\n", __LINE__); exit(1); } } while (0)

int main(void)
{
    const char *messages[] = {
        "ssid=OfficeWiFi password=credential-marker",
        "wifi_password=credential-marker,a;b&c requestId=keep-id",
        "ssid_password=credential-marker",
        "{\"password\":\"credential-marker with spaces\",\"requestId\":\"keep-id\"}",
        "wifi-sec.psk: credential-marker",
        "ha_token='credential-marker' hub_id=keep-id",
        "apiKey=credential-marker",
        "Authorization: Bearer credential-marker",
        "request failed with Bearer credential-marker",
        "http://alice:credential-marker@example.com/path",
        "--password 'credential-marker with spaces' --ssid OfficeWiFi",
        "\"long_term_private_key\":\n \"credential-marker\"",
        "-----BEGIN RSA PRIVATE KEY-----\ncredential-marker\n-----END RSA PRIVATE KEY-----",
        "long_live_token: credential-marker",
        "kpersistent=credential-marker",
        "Cookie: session=credential-marker",
        "{\"api_key\":\"credential-marker\\\"suffix\",\"status\":\"FAILED\"}"
    };
    for (size_t i = 0; i < G_N_ELEMENTS(messages); ++i) {
        char *safe = sl_log_redact(messages[i]);
        CHECK(!strstr(safe, "credential-marker"));
        CHECK(strstr(safe, "[REDACTED"));
        g_free(safe);
    }
    const char *diagnostics = "ssid=OfficeWiFi requestId=keep-id endpoint=http://127.0.0.1:18789/path "
        "file=/home/pi/config.json device=AA:BB:CC:DD:EE:FF error=connection refused";
    char *safe = sl_log_redact(diagnostics);
    CHECK(!strcmp(safe, diagnostics)); g_free(safe);
    safe = sl_log_redact("ssid=OfficeWiFi password=\"credential-marker with spaces\" requestId=keep-id");
    CHECK(strstr(safe, "ssid=OfficeWiFi") && strstr(safe, "requestId=keep-id"));
    CHECK(!strstr(safe, "credential-marker")); g_free(safe);
    safe = sl_log_redact("password=part1,part2;part3&part4 requestId=keep-id");
    CHECK(!strstr(safe, "part1") && !strstr(safe, "part2") && !strstr(safe, "part3") && !strstr(safe, "part4"));
    CHECK(strstr(safe, "requestId=keep-id")); g_free(safe);
    return 0;
}
