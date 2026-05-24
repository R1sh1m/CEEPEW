/* main/ceepew_assert.h */
#ifndef CEEPEW_ASSERT_H
#define CEEPEW_ASSERT_H

#include <stdint.h>
#include <stdbool.h>

typedef enum{
    CEEPEW_OK = 0,
    CEEPEW_ERR_NULL_PTR = 1,
    CEEPEW_ERR_BOUNDS = 2,
    CEEPEW_ERR_PARAM = 3,
    CEEPEW_ERR_NONCE_EXHAUSTED = 4,
    CEEPEW_ERR_CRYPTO = 5,
    CEEPEW_ERR_TRANSPORT = 6,
    CEEPEW_ERR_FEC = 7,
    CEEPEW_ERR_PINS = 8,
    CEEPEW_ERR_ALLOC = 9,
    CEEPEW_ERR_INTERNAL = 10,
    CEEPEW_ERR_TIMEOUT = 11,
    CEEPEW_ERR_OVERFLOW = 12,
    CEEPEW_ERR_UNSUPPORTED = 13,
    CEEPEW_ERR_BUSY = 14,
    CEEPEW_ERR_HW = 15,
    CEEPEW_ERR_NOENT = 16,
    CEEPEW_ERR_REPLAY = 17,
    CEEPEW_ERR_SIG_FAIL = 18,
    CEEPEW_ERR_MAX_RETRIES = 19,
    CEEPEW_ERR_AUTH_FAIL = 20,
    CEEPEW_ERR_FEC_UNCORRECT = 21
} CeePewErr_t;

/* Forward declaration for assertion logger. Implemented elsewhere. */
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
