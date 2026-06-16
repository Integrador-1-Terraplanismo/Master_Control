idf_component_register(SRCS "main.c" "system_logic.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES led_ctrl servo_ctrl wifi_tcp_mgr nfc_reader storage_mgr button_matrix)