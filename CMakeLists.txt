cmake_minimum_required(VERSION 3.20)

project(JunoAgentRuntime VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(JUNO_AGENT_BUILD_TESTS "Build Juno Agent Runtime SDK unit tests" OFF)
option(JUNO_AGENT_BUILD_EXAMPLES "Build Juno Agent Runtime SDK playground executables" ON)
option(JUNO_AGENT_ENABLE_LLAMA_CPP "Build the in-process llama.cpp model" ON)
option(JUNO_AGENT_FETCH_DEPS "Fetch third-party dependencies when needed" ON)
option(JUNO_AGENT_BUILD_LLAMA_SERVER "Build llama.cpp's local HTTP server" OFF)

add_library(juno_agent_runtime SHARED
    src/agent.cpp
    src/harness.cpp
    src/memory.cpp
    src/session_store.cpp
    src/llama_cpp_model.cpp)
add_library(Juno::Agent ALIAS juno_agent_runtime)
set_target_properties(juno_agent_runtime PROPERTIES EXPORT_NAME Agent)

target_compile_features(juno_agent_runtime PUBLIC cxx_std_20)
target_include_directories(juno_agent_runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(juno_agent_runtime PRIVATE JUNO_AGENT_HAS_LLAMA_CPP=$<BOOL:${JUNO_AGENT_ENABLE_LLAMA_CPP}>)

if(NOT TARGET nlohmann_json::nlohmann_json)
    if(NOT JUNO_AGENT_FETCH_DEPS)
        find_package(nlohmann_json CONFIG REQUIRED)
    else()
        include(FetchContent)
        FetchContent_Declare(nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(nlohmann_json)
    endif()
endif()
get_target_property(JUNO_NLOHMANN_INCLUDE_DIRS
    nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(juno_agent_runtime
    PUBLIC
        $<BUILD_INTERFACE:${JUNO_NLOHMANN_INCLUDE_DIRS}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(juno_agent_runtime PRIVATE nlohmann_json::nlohmann_json)

if(JUNO_AGENT_ENABLE_LLAMA_CPP)
    if(NOT TARGET llama)
        if(NOT JUNO_AGENT_FETCH_DEPS)
            find_package(Llama CONFIG REQUIRED)
        else()
            include(FetchContent)
            FetchContent_Declare(llama_cpp
                GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
                GIT_TAG 7cf1c54a96d4e950ffa614b94babf762803a8de7
                # This is a commit pin rather than a branch/tag. A shallow
                # clone may contain the commit object but not its full tree.
                GIT_PROGRESS TRUE)
            set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
            set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
            set(LLAMA_BUILD_TOOLS ${JUNO_AGENT_BUILD_LLAMA_SERVER} CACHE BOOL "" FORCE)
            set(LLAMA_BUILD_SERVER ${JUNO_AGENT_BUILD_LLAMA_SERVER} CACHE BOOL "" FORCE)
            set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)
            set(LLAMA_SUBPROCESS OFF CACHE BOOL "" FORCE)
            # Juno Agent Runtime is shared; embed static llama.cpp/ggml objects so an
            # installed SDK library does not point at its build tree.
            set(JUNO_AGENT_PREVIOUS_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
            set(BUILD_SHARED_LIBS OFF)
            FetchContent_MakeAvailable(llama_cpp)
            set(BUILD_SHARED_LIBS "${JUNO_AGENT_PREVIOUS_BUILD_SHARED_LIBS}")
        endif()
    endif()

    if(NOT TARGET llama-common)
        message(FATAL_ERROR
            "Juno Agent Runtime requires llama.cpp's llama-common target for Jinja chat-template options")
    endif()

    # Model dependencies stay private: the shared SDK library owns its
    # implementation linkage and keeps llama.cpp out of public headers.
    # Link through a build-tree file path so this private implementation detail
    # is not emitted as a required target in JunoAgentRuntimeTargets.cmake.
    add_dependencies(juno_agent_runtime llama llama-common llama-common-base)
    target_link_libraries(juno_agent_runtime PRIVATE
        "$<TARGET_FILE:llama-common>"
        "$<TARGET_FILE:llama-common-base>"
        "$<TARGET_FILE:llama>"
        "$<TARGET_GENEX_EVAL:llama,$<TARGET_PROPERTY:llama,INTERFACE_LINK_LIBRARIES>>")
    target_include_directories(juno_agent_runtime PRIVATE
        "$<TARGET_PROPERTY:llama-common,INTERFACE_INCLUDE_DIRECTORIES>"
        "$<TARGET_PROPERTY:llama,INTERFACE_INCLUDE_DIRECTORIES>"
        "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")
endif()

if(JUNO_AGENT_BUILD_TESTS)
    add_library(juno_sdk_test_support STATIC
        test_support/fake_model.cpp)
    add_library(Juno::AgentTestSupport ALIAS juno_sdk_test_support)
    target_link_libraries(juno_sdk_test_support PUBLIC Juno::Agent)
    target_include_directories(juno_sdk_test_support
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/test_support>)
endif()

if(JUNO_AGENT_BUILD_EXAMPLES)
    add_executable(juno_session_prep_planning_playground
        examples/session_prep_planning_playground.cpp)
    add_executable(juno_weather_playground
        examples/weather_playground.cpp)
    target_link_libraries(juno_session_prep_planning_playground PRIVATE Juno::Agent)
    target_link_libraries(juno_weather_playground PRIVATE
        Juno::Agent
        nlohmann_json::nlohmann_json)
    find_package(CURL REQUIRED)
    target_link_libraries(juno_weather_playground PRIVATE CURL::libcurl)
endif()

if(JUNO_AGENT_BUILD_TESTS)
    include(CTest)
    if(NOT TARGET Catch2::Catch2WithMain)
        if(NOT JUNO_AGENT_FETCH_DEPS)
            find_package(Catch2 3 CONFIG REQUIRED)
        else()
            include(FetchContent)
            FetchContent_Declare(catch2
                GIT_REPOSITORY https://github.com/catchorg/Catch2.git
                GIT_TAG 9827c148c397289df17d3967692619a198032a24
                GIT_SHALLOW TRUE)
            FetchContent_MakeAvailable(catch2)
        endif()
    endif()
    add_executable(juno_sdk_tests tests/agent_tests.cpp tests/harness_tests.cpp)
    target_link_libraries(juno_sdk_tests PRIVATE
        Juno::Agent
        juno_sdk_test_support
        Catch2::Catch2WithMain)
    add_test(NAME juno_sdk_tests COMMAND juno_sdk_tests)
endif()

install(TARGETS juno_agent_runtime EXPORT JunoAgentRuntimeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(DEFINED nlohmann_json_SOURCE_DIR)
    install(DIRECTORY "${nlohmann_json_SOURCE_DIR}/single_include/nlohmann"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
install(EXPORT JunoAgentRuntimeTargets
    FILE JunoAgentRuntimeTargets.cmake
    NAMESPACE Juno::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/JunoAgentRuntime)

configure_package_config_file(
    cmake/JunoAgentRuntimeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/JunoAgentRuntimeConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/JunoAgentRuntime)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/JunoAgentRuntimeConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/JunoAgentRuntimeConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/JunoAgentRuntimeConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/JunoAgentRuntime)
