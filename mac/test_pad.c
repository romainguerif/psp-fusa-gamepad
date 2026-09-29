/* Gamepad report -> PPSSPP debugger messages (pad_forward.c) */
#include "pad_forward.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(void) {
    char out[4096];
    uint8_t rest[8] = { 131, 151, 127, 127, 0, 0, 0, 0 };  /* this PSP's stick at rest */
    uint8_t cross[8] = { 131, 151, 127, 127, 0x01, 0, 0, 0 };
    uint8_t mix[8] = { 255, 0, 127, 127, 0x01 | 0x10, 0x02 | 0x10, 0, 0 };

    /* resting stick drift stays inside the dead zone */
    CHECK(pad_axis(131) == 0 && pad_axis(151) == 0 && pad_axis(128) == 0);
    CHECK(pad_axis(255) == 1.0f && pad_axis(0) == -1.0f);

    CHECK(pad_messages(NULL, rest, out, sizeof(out)) == 0);
    CHECK(pad_messages(rest, cross, out, sizeof(out)) == 1);
    CHECK(!strcmp(out, "{\"event\":\"input.buttons.send\",\"buttons\":{\"cross\":true}}"));
    CHECK(pad_messages(cross, rest, out, sizeof(out)) == 1);
    CHECK(strstr(out, "\"cross\":false") != NULL);

    /* several buttons + full right/up stick */
    CHECK(pad_messages(rest, mix, out, sizeof(out)) == 2);
    CHECK(strstr(out, "\"ltrigger\":true") && strstr(out, "\"start\":true") && strstr(out, "\"up\":true"));
    const char *second = out + strlen(out) + 1;
    CHECK(!strcmp(second, "{\"event\":\"input.analog.send\",\"stick\":\"left\",\"x\":1.00,\"y\":1.00}"));

    /* pad unplugged: everything released */
    CHECK(pad_messages(mix, NULL, out, sizeof(out)) == 2);
    CHECK(strstr(out, "\"cross\":false") && strstr(out, "\"up\":false"));

    printf(failures ? "%d failure(s)\n" : "PAD OK\n", failures);
    return failures ? 1 : 0;
}
