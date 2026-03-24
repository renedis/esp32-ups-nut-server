idf_component_register(
    SRCS
        "main.c"
        "hid_parser.c"
        "ups_driver.c"
        "apc_subdriver.c"
        "nut_server.c"
        "nvs_config.c"
        "web_server.c"
        "mqtt_pub.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES
        "web/index.html"
        "web/config.html"
)
