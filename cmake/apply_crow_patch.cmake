execute_process(
    COMMAND git -C "${CROW_SOURCE_DIR}" apply --check "${CROW_PATCH_FILE}"
    RESULT_VARIABLE can_apply
    OUTPUT_QUIET ERROR_QUIET
)
if(can_apply EQUAL 0)
    execute_process(
        COMMAND git -C "${CROW_SOURCE_DIR}" apply "${CROW_PATCH_FILE}"
        RESULT_VARIABLE applied
    )
    if(NOT applied EQUAL 0)
        message(FATAL_ERROR "Could not apply WARC Studio's Crow patch")
    endif()
else()
    execute_process(
        COMMAND git -C "${CROW_SOURCE_DIR}" apply --reverse --check "${CROW_PATCH_FILE}"
        RESULT_VARIABLE already_applied
        OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT already_applied EQUAL 0)
        message(FATAL_ERROR "Crow source does not match WARC Studio's patch")
    endif()
endif()
