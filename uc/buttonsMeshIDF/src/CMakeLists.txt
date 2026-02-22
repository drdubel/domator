idf_component_register(
    SRCS
        "domator_mesh.c"
        "mesh_init.c"
<<<<<<< HEAD
        "node_switch.c"
        "mesh_comm.c"
        "node_root.c"
        "node_relay.c"
        "health_ota.c"
        "telnet.c"
=======
        "mesh_comm.c"
        "node_root.c"
>>>>>>> 75e1902 (changed to my version)
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_wifi
        nvs_flash
        esp_netif
        esp_event
        esp_timer
<<<<<<< HEAD
        esp_https_ota
        esp_http_client
        led_strip
        mqtt
        app_update
        freertos
        driver
        json
        mdns
=======
        mqtt
        driver
        json
>>>>>>> 75e1902 (changed to my version)
)