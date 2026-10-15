# ArtIN3D: biblioteca aberta da Elegoo (Apache-2.0) para conectar impressoras
# Elegoo pela rede local e pela conta Elegoo. Fixada num commit para que a
# compilação seja reproduzível.
orcaslicer_add_cmake_project(elegoolink
    GIT_REPOSITORY      https://github.com/elegooofficial/elegoo-link.git
    GIT_TAG             46c7b814e055cf9675d58482d79f43d0bd2280da

    DEPENDS
        dep_ixwebsocket
        dep_PahoMqttCpp
        ${CURL_PKG}
        ${OPENSSL_PKG}

    CMAKE_ARGS
        -DBUILD_EXAMPLES=OFF
        -DBUILD_TESTS=OFF
        -DBUILD_SHARED_LIBS=OFF
        -DENABLE_CLOUD_FEATURES=ON
)
