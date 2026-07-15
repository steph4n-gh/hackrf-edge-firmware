add_executable(
  hackrf_edge_test
  test_hackrf_edge.c
  ../src/hackrf_edge.c)

target_compile_features(hackrf_edge_test PRIVATE c_std_90)
target_include_directories(hackrf_edge_test PRIVATE ../src)
if(MSVC)
  target_compile_options(hackrf_edge_test PRIVATE /W4 /WX)
else()
  target_compile_options(hackrf_edge_test PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME hackrf_edge_protocol COMMAND hackrf_edge_test)
