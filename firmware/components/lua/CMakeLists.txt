file(GLOB SRCS "src/*.c")
idf_component_register(SRCS ${SRCS} INCLUDE_DIRS "src")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-but-set-variable)
