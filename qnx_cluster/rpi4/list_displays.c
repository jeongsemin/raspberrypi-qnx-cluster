// Lists the displays QNX Screen knows: numeric ID (for vncserv -display=), id_string, size, attached.
#include <screen/screen.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    screen_context_t ctx;
    if (screen_create_context(&ctx, SCREEN_APPLICATION_CONTEXT) != 0) { perror("screen_create_context"); return 1; }
    int n = 0;
    screen_get_context_property_iv(ctx, SCREEN_PROPERTY_DISPLAY_COUNT, &n);
    screen_display_t *d = calloc(n, sizeof *d);
    screen_get_context_property_pv(ctx, SCREEN_PROPERTY_DISPLAYS, (void **)d);
    printf("displays: %d\n", n);
    for (int i = 0; i < n; i++) {
        int id = -1, size[2] = {0, 0}, attached = -1, type = -1;
        char name[128] = "";
        screen_get_display_property_iv(d[i], SCREEN_PROPERTY_ID, &id);
        screen_get_display_property_cv(d[i], SCREEN_PROPERTY_ID_STRING, sizeof name, name);
        screen_get_display_property_iv(d[i], SCREEN_PROPERTY_SIZE, size);
        screen_get_display_property_iv(d[i], SCREEN_PROPERTY_ATTACHED, &attached);
        screen_get_display_property_iv(d[i], SCREEN_PROPERTY_TYPE, &type);
        printf("  id=%d id_string=%s size=%dx%d attached=%d type=%d\n", id, name, size[0], size[1], attached, type);
    }
    screen_destroy_context(ctx);
    return 0;
}
