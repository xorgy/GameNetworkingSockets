if(SANITIZE)
	add_definitions(-DSANITIZER)
endif()

# Let's always link all tests statically with the lib
# To keep things simple.  (Since some of the tests MUST
# link statically)
set( GAMENETWORKINGSOCKETS_LIB "GameNetworkingSockets::static" )

add_executable(
	test_connection
	test_common.cpp
	test_connection.cpp)
set_target_common_gns_properties( test_connection )
target_link_libraries(test_connection ${GAMENETWORKINGSOCKETS_LIB})
add_sanitizers(test_connection)
add_test(NAME connection_quick COMMAND test_connection suite-quick WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
add_test(NAME connection_soak  COMMAND test_connection soak  WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

add_executable(
	test_crypto
	test_crypto.cpp
	)
set_target_common_gns_properties( test_crypto )
target_include_directories(test_crypto PRIVATE ../src ../src/public ../src/common ../include)
target_link_libraries(test_crypto GameNetworkingSockets::static)
add_sanitizers(test_crypto)
add_test(NAME crypto COMMAND test_crypto WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# Test data for the crypto test when the project is built
file(COPY aesgcmtestvectors DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# P2P test
if(ENABLE_ICE)
	add_executable(
		test_p2p
		test_common.cpp
		test_p2p.cpp
		../examples/trivial_signaling_client.cpp)
	set_target_common_gns_properties( test_p2p )
	target_link_libraries(test_p2p GameNetworkingSockets::static_mock)
	add_sanitizers(test_p2p)

	# Publish the test scripts
	set( P2P_TEST_SCRIPT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_p2p.py )
	set( P2P_TEST_SCRIPT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/test_p2p.py )
	set( STUN_SERVER_SCRIPT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stun_server.py )
	set( STUN_SERVER_SCRIPT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/stun_server.py )
	add_custom_command(
		OUTPUT ${P2P_TEST_SCRIPT}
		COMMAND ${CMAKE_COMMAND} -E copy ${P2P_TEST_SCRIPT_SRC} ${P2P_TEST_SCRIPT}
		DEPENDS ${P2P_TEST_SCRIPT_SRC} )
	add_custom_command(
		OUTPUT ${STUN_SERVER_SCRIPT}
		COMMAND ${CMAKE_COMMAND} -E copy ${STUN_SERVER_SCRIPT_SRC} ${STUN_SERVER_SCRIPT}
		DEPENDS ${STUN_SERVER_SCRIPT_SRC} )
	add_custom_target( publish_test_script ALL DEPENDS ${P2P_TEST_SCRIPT} ${STUN_SERVER_SCRIPT} )
	add_test(NAME p2p COMMAND ${P2P_TEST_SCRIPT} WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
endif()

add_executable(test_sharedpayload test_sharedpayload.cpp)
set_target_common_gns_properties(test_sharedpayload)
target_link_libraries(test_sharedpayload ${GAMENETWORKINGSOCKETS_LIB})
add_sanitizers(test_sharedpayload)
add_test(NAME shared_payload COMMAND test_sharedpayload WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

if(USE_IO_URING)
    add_executable(test_iouring test_iouring.cpp ../src/steamnetworkingsockets/clientlib/steamnetworkingsockets_iouring.cpp)
    set_target_common_gns_properties(test_iouring)
    target_compile_definitions(test_iouring PRIVATE STEAMNETWORKINGSOCKETS_USE_IO_URING)
    target_include_directories(test_iouring PRIVATE ../src/steamnetworkingsockets/clientlib)
    target_link_libraries(test_iouring PkgConfig::LIBURING)
    add_sanitizers(test_iouring)
    add_test(NAME iouring_udp COMMAND test_iouring WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
    set_tests_properties(iouring_udp PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_executable(test_service_fairness test_service_fairness.cpp)
set_target_common_gns_properties(test_service_fairness)
if(MSVC)
    target_compile_options(test_service_fairness PRIVATE /GR-)
else()
    target_compile_options(test_service_fairness PRIVATE -fno-rtti)
endif()
target_include_directories(test_service_fairness PRIVATE ../src ../src/public ../src/common ${PROJECT_BINARY_DIR}/src)
target_link_libraries(test_service_fairness GameNetworkingSockets::static)
add_sanitizers(test_service_fairness)
add_test(NAME service_fairness COMMAND test_service_fairness WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
