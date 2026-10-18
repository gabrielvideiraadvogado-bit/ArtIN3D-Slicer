set(_options "")
if (MSVC AND DEP_DEBUG)
    set(_options "FORWARD_CONFIG")
endif ()

orcaslicer_add_cmake_project(Draco
    ${_options}
    # ArtIN3D: GitHub's archive service (codeload) streams this 61 MB archive slowly to the
    # macOS runners and cuts it short ("Transferred a partial file"), even through its
    # other URL. A shallow git clone of the same tag goes through git's own transport.
    GIT_REPOSITORY https://github.com/google/draco.git
    GIT_TAG 1.5.7
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    CMAKE_ARGS
        # The encoder and decoder tools duplicate draco.lib; see deps-windows.cmake.
        "${DEP_LLD_FORCE_MULTIPLE}"
)