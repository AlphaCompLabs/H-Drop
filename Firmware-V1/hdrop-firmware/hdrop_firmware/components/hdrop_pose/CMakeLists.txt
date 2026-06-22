idf_component_register(
    SRCS "hdrop_pose.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos hdrop_heading esp_timer hdrop_motor
)
