/* components/ecc/ecc_arq.h */

#ifndef CEEPEW_ECC_ARQ_H
#define CEEPEW_ECC_ARQ_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

CeePewErr_t ecc_arq_init_hop_sync(void);
CeePewErr_t ecc_arq_reset(void);
CeePewErr_t ecc_arq_encode(const uint8_t *in, uint16_t in_len,
                           uint8_t *out, uint16_t *out_len,
                           uint16_t max_out_len);
CeePewErr_t ecc_arq_decode(const uint8_t *in, uint16_t in_len,
                           uint8_t *out, uint16_t *out_len,
                           uint16_t max_out_len, bool *corrected);
CeePewErr_t ecc_arq_send(const uint8_t peer_mac[6],
                         const uint8_t *payload, uint16_t len);

/* Commit tentative sequence advance after successful cryptographic verification */
CeePewErr_t ecc_arq_commit(void);

/* Roll back tentative sequence advance if downstream authentication fails */
CeePewErr_t ecc_arq_rollback(void);

#ifdef __cplusplus
}
#endif

#endif /* CEEPEW_ECC_ARQ_H */

