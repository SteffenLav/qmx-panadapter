#pragma once
#include <stddef.h>
typedef enum { FT8_TX_IDLE = 0, FT8_TX_ARMED, FT8_TX_KEYED } ft8_tx_status_t;
ft8_tx_status_t ft8_tx_get_status(char *out, size_t out_len, int *pct);
