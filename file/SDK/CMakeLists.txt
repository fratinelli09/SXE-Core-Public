# Strategy SDK: headers only (base class, contexts, export macro).
# A strategy links SDK and also gets Common (types, Logger, TimeUtils) and Settings (CReader)
add_library(SDK INTERFACE)

target_include_directories(SDK INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(SDK INTERFACE
    Common
    Settings
)

# C contract for strategies (sxe_strategy.h): installed next to sxe.h, for strategies built
# outside SXE with any compiler or written in another language
install(FILES sxe_strategy.h DESTINATION ${SXE_INCLUDE_DIR})
