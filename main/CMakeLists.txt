idf_component_register(
    SRCS
        "main.c"
        "boot_guard.c"
        "dlmode.c"
        "hang_guard.c"
        "hang_guard_core.c"
        "halow.c"
        "mesh.c"
        "mesh_diag.c"
        "mesh_bridge.c"
        "usb_net.c"
        "usbnet_core.c"
        "wifi_ap.c"
        "nat.c"
        "nat_frag.c"
        "mudp.c"
        "mudp_classify.c"
        "cdc_out.c"
        "led.c"
        "cfg.c"
        "at.c"
        "bat_mode.c"
        "bat_port.c"
        "bat/bat_bcast.c"
        "bat/bat_codec.c"
        "bat/bat_core.c"
        "bat/bat_crc32c.c"
        "bat/bat_elp.c"
        "bat/bat_frag.c"
        "bat/bat_icmp.c"
        "bat/bat_ogm.c"
        "bat/bat_orig.c"
        "bat/bat_render.c"
        "bat/bat_tt.c"
        "bat/bat_unicast.c"
        "bat/bat_utvlv.c"
    INCLUDE_DIRS
        "."
        "bat"
    REQUIRES
        halow
        esp_netif
        esp_wifi
        esp_event
        lwip
        nvs_flash
        esp_tinyusb
        espcoredump
)

# TinyUSB's network class calls usb_net.c after each transfer, so its TX queue drains in the TinyUSB task.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=netd_xfer_cb")

# lwIP's IPv4 input hook (nat_frag.c) reassembles fragments to the Warthog or through NAPT, drops packets NAPT would overrun.
idf_component_get_property(lwip lwip COMPONENT_LIB)
target_compile_definitions(${lwip} PRIVATE "ESP_IDF_LWIP_HOOK_FILENAME=\"warthog_lwip_hooks.h\"")
target_include_directories(${lwip} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/lwip_hooks")

# AT+ASSERTTEST's assert records at.c's fileid (components/halow/components/mmosal_fileid.cmake).
# Only on sources that assert; main.c, first in SRCS, must not (pioarduino puts its defines on all).
include(${CMAKE_SOURCE_DIR}/components/halow/components/mmosal_fileid.cmake)
warthog_mmosal_fileid(${COMPONENT_LIB} ASSERTING)
