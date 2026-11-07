/* Host tools: pick the sensor profile from the QS_PROFILE environment variable (default 0). */
#include <stdlib.h>
#include "qs_core.h"
static inline void qs_tool_profile(void)
{
    const char *p = getenv("QS_PROFILE");
    if (p && qs_set_profile(atoi(p)) == 0 && atoi(p) != 0)
        fprintf(stderr, "sensor profile: %s\n", qs_prof->name);
}
