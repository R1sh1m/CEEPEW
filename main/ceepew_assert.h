/* main/ceepew_assert.h
 *
 * Project-wide assert macros and the CeePewErr_t type.
 *
 * Hosted in main/ so all components can include it without depending on
 * the OLED driver. Previously lived in components/ceepew_oled/. The
 * implementation of ceepew_log_assert() is in main/ceepew_assert.c.
 *
 * CeePewErr_t itself is defined in hal_ui_types.h (components/ceepew_oled).
 * We keep the transitive include here intentionally: ~100 TUs in the
 * codebase use CeePewErr_t without including hal_ui_types.h directly,
 * so removing the include would require a separate sweep. The cost
 * of the transitive include is one extra preprocessor pass per TU
 * and zero runtime cost.
 */
#ifndef CEEPEW_ASSERT_H
#define CEEPEW_ASSERT_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_ui_types.h"

/* Forward declaration for assertion logger. Implemented in
 * main/ceepew_assert.c. */
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
