macro(generate_build_info)
    find_package(Git QUIET)

    # Gets a UTC timstamp and sets the provided variable to it
    function(get_timestamp _var)
        string(TIMESTAMP timestamp UTC)
        set(${_var} "${timestamp}" PARENT_SCOPE)
    endfunction()
    get_timestamp(BUILD_DATE)

    list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/externals/cmake-modules")

    if (EXISTS "${CMAKE_SOURCE_DIR}/GIT-COMMIT" AND EXISTS "${CMAKE_SOURCE_DIR}/GIT-TAG")
        file(READ "${CMAKE_SOURCE_DIR}/GIT-COMMIT" GIT_REV_RAW LIMIT 64)
        string(STRIP "${GIT_REV_RAW}" GIT_REV)
        string(SUBSTRING "${GIT_REV_RAW}" 0 9 GIT_DESC)
        set(GIT_BRANCH "HEAD")
    # A linked worktree has a .git file pointing at the real gitdir, not a .git directory, so
    # testing for objects/ inside it silently downgrades every worktree build to "UNKNOWN".
    elseif (EXISTS "${CMAKE_SOURCE_DIR}/.git")
        # Find the package here with the known path so that the GetGit commands can find it as well
        find_package(Git QUIET PATHS "${GIT_EXECUTABLE}")

        # only use Git to check revision info when source is obtained via Git
        include(GetGitRevisionDescription)
        get_git_head_revision(GIT_REF_SPEC GIT_REV)
        # In a linked worktree HEAD lives in the worktree's own gitdir, but the ref it names does
        # not, so the lookup above finds no hash. Ask git, which knows where to look.
        if (NOT GIT_REV MATCHES "^[0-9a-f]+$")
            execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
                            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                            OUTPUT_VARIABLE GIT_REV
                            OUTPUT_STRIP_TRAILING_WHITESPACE
                            ERROR_QUIET)
        endif()
        git_describe(GIT_DESC --always --long --dirty)
        git_branch_name(GIT_BRANCH)
        if (DEFINED ENV{CITRA_USE_TAG_AS_VERSION})
            git_describe(GIT_TAG --tags --dirty)
        endif()
        # Zeste: its version is the latest release tag (v0.4.1). A build at the tag is "0.4.1",
        # a later one "0.4.1-3-g3159690". The update check compares the numbers.
        execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*"
                        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                        OUTPUT_VARIABLE ZESTE_VERSION
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET)
        string(REGEX REPLACE "^v" "" ZESTE_VERSION "${ZESTE_VERSION}")
    else()
        # self-packed archive?
        set(GIT_REV "UNKNOWN")
        set(GIT_DESC "UNKNOWN")
        set(GIT_BRANCH "UNKNOWN")
    endif()
    string(SUBSTRING "${GIT_REV}" 0 7 GIT_SHORT_REV)
    if (NOT ZESTE_VERSION)
        set(ZESTE_VERSION "unknown")
    endif()
    # What the title bar shows: "0.4.5" at a release tag, "0.4.5-dev" after it, the commit
    # without git tags.
    if (ZESTE_VERSION MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)(-.*)?$")
        set(ZESTE_DISPLAY_VERSION "${CMAKE_MATCH_1}")
        if (CMAKE_MATCH_2)
            string(APPEND ZESTE_DISPLAY_VERSION "-dev")
        endif()
    else()
        set(ZESTE_DISPLAY_VERSION "${GIT_SHORT_REV}")
    endif()

    # Set build version
    set(REPO_NAME "")
    set(BUILD_VERSION "0")
    set(BUILD_FULLNAME "${GIT_SHORT_REV}")
    if (NOT DEFINED GIT_TAG)
        if ((DEFINED ENV{CI} AND DEFINED ENV{GITHUB_ACTIONS}))
            if ($ENV{GITHUB_REF_TYPE} STREQUAL "tag")
                set(GIT_TAG $ENV{GITHUB_REF_NAME})
            endif()
        elseif (EXISTS "${CMAKE_SOURCE_DIR}/GIT-COMMIT" AND EXISTS "${CMAKE_SOURCE_DIR}/GIT-TAG")
            file(READ "${CMAKE_SOURCE_DIR}/GIT-TAG" GIT_TAG)
            string(STRIP ${GIT_TAG} GIT_TAG)
        endif()
    endif()

    if (DEFINED GIT_TAG AND NOT "${GIT_TAG}" STREQUAL "unknown")
        set(BUILD_VERSION "${GIT_TAG}")
        set(BUILD_FULLNAME "${BUILD_VERSION}")
    endif()

    # Set build variant
    set(BUILD_VARIANT "") # Empty string on non-Windows platforms
    if (WIN32)
        if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND MINGW)
            set(BUILD_VARIANT "MXE")
        elseif(MINGW)
            set(BUILD_VARIANT "MSYS2")
        elseif(MSVC)
            set(BUILD_VARIANT "MSVC")
        else()
            set(BUILD_VARIANT "Unknown")
        endif()
    endif()
endmacro()
