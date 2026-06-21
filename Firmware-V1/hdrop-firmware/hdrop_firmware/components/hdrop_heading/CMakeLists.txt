idf_component_register(
    SRCS "hdrop_heading.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos nvs_flash hdrop_motor
)
