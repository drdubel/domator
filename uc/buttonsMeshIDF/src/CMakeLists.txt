if(CONFIG_DOMATOR_DEBUG_TELNET)
    set(TELNET_SOURCE "telnet.c")
else()
    set(TELNET_SOURCE "telnet_disabled.c")
endif()

if(CONFIG_IDF_TARGET_ESP32C6)
    set(BATTERY_SOURCE "battery_power.c")
    set(BATTERY_REQUIRES esp_driver_usb_serial_jtag)
endif()

idf_component_register(
    SRCS
        "domator_mesh.c"
        "credentials.c"
        "remote_switch.c"
        "mesh_init.c"
        "node_switch.c"
        "mesh_comm.c"
        "node_root.c"
        "node_relay.c"
        "health_ota.c"
        ${TELNET_SOURCE}
        ${BATTERY_SOURCE}
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_wifi
        nvs_flash
        esp_netif
        esp_event
        esp_timer
        esp_https_ota
        esp_http_client
        led_strip
        mqtt
        app_update
        freertos
        driver
        json
        mdns
    PRIV_REQUIRES
        ${BATTERY_REQUIRES}
)
