#ifndef BSP_INFO_H
#define BSP_INFO_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Log a marker-fenced block of board/silicon/peripheral identity.
 *
 * Read-only and failure-tolerant. Call AFTER bsp_i2c_init() has run
 * (i.e. after display_init()). Probes touch by I2C and infers the
 * paired panel - never touches the MIPI-DSI bus.
 */
void bsp_info_log(void);

#ifdef __cplusplus
}
#endif

#endif // BSP_INFO_H