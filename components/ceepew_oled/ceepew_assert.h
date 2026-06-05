/* components/ceepew_oled/ceepew_assert.h
 *
 * Project-wide assert macros and the CeePewErr_t type.
 *
 * Lives in the ceepew_oled component (the lowest layer of the
 * dependency graph) so that:
 *  - ceepew_oled_gfx_primitives.h can use CeePewErr_t in its public
 *    function signatures without depending on main.
 *  - main/, ceepew_hal/, etc. include this header through the normal
 *    REQUIRES chain. There is no circular REQUIRES in the component
 *    DAG.
 *
 * CeePewErr_t itself is defined in hal_ui_types.h (same component).
 */
#ifndef CEEPEW_ASSERT_H
#define CEEPEW_ASSERT_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_ui_types.h"

/* Forward declaration for assertion logger. Implemented in
 * ceepew_assert.c, also in this component. */
void ceepew_log_assert(const char *expr, const char *file, int line, CeePewErr_t code);

/* CEEPEW_ASSERT: return 'err' in non-void functions when condition fails. */
#define CEEPEW_ASSERT(cond, err)                                 \
    do                                                           \
    {                                                            \
        if (!(cond))                                             \
        {                                                        \
            ceepew_log_assert(#cond, __FILE__, __LINE__, (err)); \
            return (err);                                        \
        }                                                        \
    } while (0)

/* CEEPEW_ASSERT_VOID: return from void functions when condition fails. */
#define CEEPEW_ASSERT_VOID(cond)                                               \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            ceepew_log_assert(#cond, __FILE__, __LINE__, CEEPEW_ERR_NULL_PTR); \
            return;                                                            \
        }                                                                      \
    } while (0)

/* CEEPEW_ASSERT_PTR: use in functions returning pointer; returns NULL on failure */
#define CEEPEW_ASSERT_PTR(cond, err)                             \
    do                                                           \
    {                                                            \
        if (!(cond))                                             \
        {                                                        \
            ceepew_log_assert(#cond, __FILE__, __LINE__, (err)); \
            return NULL;                                         \
        }                                                        \
    } while (0)

#endif /* CEEPEW_ASSERT_H */
