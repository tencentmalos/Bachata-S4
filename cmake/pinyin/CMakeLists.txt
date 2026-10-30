# The decoder and dictionary are shared with SteamPSP through Foundation.
get_filename_component(_shad_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT TARGET spatial::foundation_pinyin)
    add_subdirectory("${_shad_root}/foundation/modules/pinyin" foundation)
endif()
get_target_property(_dictionary spatial::foundation_pinyin FOUNDATION_PINYIN_DICTIONARY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dictionary}")

# Embed the same checked dictionary on desktop and Android; no runtime downloads.
file(READ "${_dictionary}" DICTIONARY_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," DICTIONARY_BYTES "${DICTIONARY_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pinyin_dictionary.cpp"
    "#include <span>\nnamespace Libraries::Ime {\nstd::span<const unsigned char> BundledPinyinDictionary() {\nstatic constexpr unsigned char data[] = {${DICTIONARY_BYTES}};\nreturn data;\n}\n}\n")
add_library(shad_pinyin_dictionary STATIC "${CMAKE_CURRENT_BINARY_DIR}/pinyin_dictionary.cpp")
target_compile_features(shad_pinyin_dictionary PUBLIC cxx_std_20)
target_link_libraries(shad_pinyin_dictionary PUBLIC spatial::foundation_pinyin)
set_target_properties(shad_pinyin_dictionary PROPERTIES POSITION_INDEPENDENT_CODE ON)
