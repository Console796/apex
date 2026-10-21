include(doctest)

add_executable(apex_tests main.cpp core/version_test.cpp core/math_test.cpp core/handle_test.cpp
               core/result_test.cpp core/strings_test.cpp core/paths_test.cpp
               core/log_test.cpp core/job_pool_test.cpp core/kv1_test.cpp
               document/vmf_test.cpp document/document_test.cpp
               assets/vfs_test.cpp assets/game_profile_test.cpp assets/material_test.cpp
               assets/texture_test.cpp assets/fgd_test.cpp assets/material_cache_test.cpp
               assets/material_corpus_test.cpp core/json_test.cpp
               geometry/corpus_test.cpp geometry/convex_polyhedron_test.cpp
               geometry/face_matching_test.cpp)
target_link_libraries(apex_tests PRIVATE apex_assets sourcepp::vpkpp sourcepp::steampp
                                         sourcepp::vtfpp sourcepp::toolpp
                                         apex_document apex_geometry apex_core apex_warnings
                                         doctest::doctest)
doctest_discover_tests(apex_tests)

target_compile_definitions(apex_tests PRIVATE APEX_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
