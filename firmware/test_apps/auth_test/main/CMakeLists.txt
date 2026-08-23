set(RW_MAIN "${CMAKE_CURRENT_LIST_DIR}/../../../main")

idf_component_register(
    SRCS "test_auth.c"
         "${RW_MAIN}/auth.c"
         "${RW_MAIN}/canon_json.c"
         "${RW_MAIN}/b64url.c"
    INCLUDE_DIRS "." "${RW_MAIN}"
    REQUIRES unity json libsodium
    WHOLE_ARCHIVE
)
