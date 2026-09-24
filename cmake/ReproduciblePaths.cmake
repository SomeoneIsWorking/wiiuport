# Included after every project() in the runtime's build (CMAKE_PROJECT_INCLUDE,
# passed by tools/wiiuport/build.py).
#
# Compiled paths -- __FILE__ in asserts and logs, debug information -- are
# recorded relative to this checkout, so a package a player receives does not
# name the machine or the directory it was built in.
include_guard(GLOBAL)

get_filename_component(WIIUPORT_CHECKOUT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_compile_options(
  "$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang,GNU>:-ffile-prefix-map=${WIIUPORT_CHECKOUT}/=wiiuport/>"
  "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>:-ffile-prefix-map=${WIIUPORT_CHECKOUT}/=wiiuport/>")
