/* TinyUSB's NCM/ECM drivers on the host, sized as esp_tinyusb sizes them (test_usbnet.c). */
#pragma once
#define CFG_TUSB_MCU OPT_MCU_NONE
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUSB_DEBUG 0
#define TUP_DCD_ENDPOINT_MAX 8
#define CFG_TUD_ENABLED 1
#define CFG_TUD_ENDPOINT0_SIZE 64
#if USBNET_TEST_ECM
#define CFG_TUD_ECM_RNDIS 1
#define CFG_TUD_NCM 0
#else
#define CFG_TUD_ECM_RNDIS 0
#define CFG_TUD_NCM 1
#endif
/* CONFIG_TINYUSB_NCM_{OUT,IN}_NTB_BUFFS_COUNT and _BUFF_MAX_SIZE in every generated sdkconfig */
#define CFG_TUD_NCM_OUT_NTB_N 3
#define CFG_TUD_NCM_IN_NTB_N 3
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE 3200
#define CFG_TUD_NCM_IN_NTB_MAX_SIZE 3200
