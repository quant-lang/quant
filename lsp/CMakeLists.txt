# LSP/tooling utilities for Quant.
# Built as a subproject; links against libquant so it reuses the same
# front/middle-end pipeline as the main compiler.

add_executable(qud qud.cpp)
target_link_libraries(qud PRIVATE libquant quant_compiler_flags)

set_target_properties(qud PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

install(TARGETS qud RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
