# Get all source files recursively from src directory
file(GLOB_RECURSE SRC_FILES "src/*.c")
list(REMOVE_ITEM SRC_FILES "${CMAKE_CURRENT_LIST_DIR}/src/lib/qrcode/qrcodegen.c")

idf_component_register(
    SRCS ${SRC_FILES}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES esp_timer
)
