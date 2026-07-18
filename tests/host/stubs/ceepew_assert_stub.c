/* Stub for ceepew_log_assert -- host-compilation placeholder.
 * Does NOT abort — the macro returns the error code to the caller,
 * which should handle it gracefully. */
#include <stdio.h>
#include "hal_ui_types.h"

void ceepew_log_assert(const char *expr, const char *file, int line, CeePewErr_t code)
{
    fprintf(stderr, "ASSERT: %s (%s:%d) err=%d\n",
            expr, file, line, (int)code);
}
