/*

The BDK (Beken's SDK) for ESPurna's code of arch/beken: its headers in the order they need and what its sources have
without a header. In place of LibreTiny's sdk_private.h / sdk_extern.h / WiFiPrivate.h (MIT, Kuba Szczodrzyński), which
this follows. C and C++.

*/

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// most of the BDK is here, before anything else
#include <include.h>

#include <arm_arch.h>
#include <bk_timer.h>
#include <bk_timer_pub.h>
#include <drv_model_pub.h>
#include <flash_pub.h>
#include <gpio_pub.h>
#include <manual_ps_pub.h>
#include <param_config.h>
#include <pwm_pub.h>
#include <rtos_pub.h>
#include <saradc_pub.h>
#include <start_type_pub.h>
#include <sys_ctrl.h>
#include <sys_ctrl_pub.h>
#include <sys_rtos.h>
#include <uart_pub.h>
#include <wdt_pub.h>

// BDK 3.0.76+ has a conflicting structure (not used by its sources)
#define wifi_event_sta_disconnected_t bk_wifi_event_sta_disconnected_t
#include <wlan_ui_pub.h>
#undef wifi_event_sta_disconnected_t

// library-lwip's port of the BDK (port/net.h), after lwIP's own
#include <lwip/netif.h>
#include <net.h>

// the Wi-Fi internals, in the order LibreTiny's WiFiPrivate.h took them (the scan callback of rw_msg_rx.h)
#include <common.h>
#include <config.h>
#include <main_none.h>
#include <rw_msg_pub.h>
#include <rw_msg_rx.h>
#include <sa_ap.h>
#include <vif_mgmt.h>
#include <wpa_supplicant_i.h>

// app/param_config.c
extern ap_param_t* g_ap_param_ptr;

// the old PWM driver (sddev_control) on BDK 3.0.33
#define BEKEN_NEW_PWM_DRIVER 0

// conflict with stl_algobase.h
#undef min
#undef max

// SDK, without a header
extern uint8_t system_mac[];
extern int uart_print_port;
uint32_t wdt_ctrl(uint32_t cmd, void* param);
void bk_send_byte(uint8_t uport, uint8_t data);
void uart_hw_set_change(uint8_t uport, bk_uart_config_t* uart_config);
int uart_rx_callback_set(int uport, uart_callback callback, void* param);
void uart1_exit(void);
void uart2_exit(void);
void ps_delay(volatile uint16_t times);
uint8_t bk_wlan_has_role(uint8_t role);
uint32_t func_init_extended(void);
void app_pre_start(void);
void flash_init(void);
uint32_t flash_ctrl(uint32_t cmd, void* parm);
PROTECT_TYPE get_flash_protect(void);
void flash_protection_op(uint8_t mode, PROTECT_TYPE type);

// the SDK's glue (framework-beken-bdk glue/fixups): wlan_ui.c, arch_main.c
OSStatus bk_wlan_start_sta_adv_fix(network_InitTypeDef_adv_st* inNetworkInitParaAdv);
extern unsigned char __bk_rf_is_init;

// FreeRTOS port: the interrupt mask (IRQ & FIQ bits of CPSR), nesting-safe
int port_disable_interrupts_flag(void);
void port_enable_interrupts_flag(int);

#ifdef __cplusplus
} // extern "C"
#endif
