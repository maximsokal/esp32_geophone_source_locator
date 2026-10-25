find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
    WORKING_DIRECTORY "${NRF_REPO_ROOT}"
    RESULT_VARIABLE git_result OUTPUT_VARIABLE NRF_BUILD_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(LENGTH "${NRF_BUILD_GIT_COMMIT}" commit_length)
if(NOT git_result EQUAL 0 OR NOT commit_length EQUAL 40 OR
   NOT NRF_BUILD_GIT_COMMIT MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "Cannot identify the nRF source commit")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" diff-index --quiet --ignore-submodules=untracked HEAD --
    WORKING_DIRECTORY "${NRF_REPO_ROOT}" RESULT_VARIABLE dirty_result)
if(dirty_result EQUAL 0)
    set(NRF_BUILD_GIT_DIRTY 0)
elseif(dirty_result EQUAL 1)
    set(NRF_BUILD_GIT_DIRTY 1)
else()
    message(FATAL_ERROR "Cannot determine whether the nRF source checkout is dirty")
endif()
set(NRF_BUILD_GIT_COMMIT_BYTES "")
foreach(index RANGE 0 19)
    math(EXPR offset "${index} * 2")
    string(SUBSTRING "${NRF_BUILD_GIT_COMMIT}" ${offset} 2 byte)
    string(APPEND NRF_BUILD_GIT_COMMIT_BYTES "0x${byte},")
endforeach()
configure_file("${CMAKE_CURRENT_LIST_DIR}/nrf_build_info.h.in"
    "${NRF_BUILD_INFO_OUTPUT}" @ONLY)
