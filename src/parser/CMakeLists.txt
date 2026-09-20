# Prefer regenerating the parser when Flex/Bison are available. The generated
# sources are committed as well so users can build the public repository
# without installing parser generators.
find_package(BISON QUIET)
find_package(FLEX QUIET)

if(BISON_FOUND AND FLEX_FOUND)
    bison_target(yacc yacc.y ${CMAKE_CURRENT_SOURCE_DIR}/yacc.tab.cpp
            DEFINES_FILE ${CMAKE_CURRENT_SOURCE_DIR}/yacc.tab.h)
    flex_target(lex lex.l ${CMAKE_CURRENT_SOURCE_DIR}/lex.yy.cpp)
    add_flex_bison_dependency(lex yacc)
    set(SOURCES ${BISON_yacc_OUTPUT_SOURCE} ${FLEX_lex_OUTPUTS} ast.cpp)
else()
    message(STATUS "Flex/Bison not found; using committed generated parser sources")
    set(SOURCES yacc.tab.cpp lex.yy.cpp ast.cpp)
endif()

add_library(parser STATIC ${SOURCES})

add_executable(test_parser test_parser.cpp)
target_link_libraries(test_parser parser)
add_test(NAME test_parser COMMAND test_parser
        WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
