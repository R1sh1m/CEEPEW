/* main/ceepew_assert.h — Project-wide assert macros and CeePewErr_t type.
 *
 * CeePewErr_t is defined in hal_ui_types.h; we re-export it here so all
 * ~100 TUs can use it without an extra include.
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
