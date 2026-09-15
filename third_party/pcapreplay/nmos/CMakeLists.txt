# pcapreplay_nmos -- IS-04 registration and Node API, IS-05 Connection API.
#
# Hand-rolled and dependency-free: a small JSON codec, a routing HTTP server and
# client, and DNS-SD on the native Windows API. No Conan, no vcpkg, no Bonjour
# SDK -- see the header comment in nmos_node.h for why, and for the NmosBackend
# seam that lets sony/nmos-cpp be dropped in later.
#
# dnsapi provides DnsServiceBrowse/Resolve/Register, which need Windows 10 1703
# or later (_WIN32_WINNT=0x0A00, set at the top level).

add_library(pcapreplay_nmos STATIC
    src/json.cpp
    src/http.cpp
    src/mdns.cpp
    src/mdns_win.cpp
    src/mdns_posix.cpp
    src/uuid.cpp
    src/nmos_node.cpp
    src/status_text.cpp
)

# Both mdns_*.cpp files are compiled on both platforms and each is empty on the
# one it is not for. Listing them unconditionally means neither can rot unnoticed
# behind a generator expression.
target_include_directories(pcapreplay_nmos PUBLIC include)
# For common/platform.h -- the socket layer is shared, not duplicated.
target_include_directories(pcapreplay_nmos PRIVATE ${CMAKE_SOURCE_DIR}/common/include)
if(WIN32)
    target_link_libraries(pcapreplay_nmos PRIVATE ws2_32 dnsapi)
else()
    target_link_libraries(pcapreplay_nmos PRIVATE pcapreplay_common Threads::Threads)
endif()
