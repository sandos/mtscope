cmake_minimum_required(VERSION 3.16)
project(meshat_monitor VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SQLite3 REQUIRED)
find_package(Protobuf REQUIRED)
find_package(OpenSSL QUIET)
find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h REQUIRED)
find_library(MOSQUITTO_LIBRARY mosquitto REQUIRED)

set(MESHTASTIC_PROTOBUF_DIR "/home/sandos/projs/protobufs" CACHE PATH "Meshtastic protobuf workspace")
file(GLOB MESHTASTIC_PROTO_FILES CONFIGURE_DEPENDS "${MESHTASTIC_PROTOBUF_DIR}/meshtastic/*.proto")
list(FILTER MESHTASTIC_PROTO_FILES EXCLUDE REGEX "/deviceonly(_legacy)?\\.proto$")
set(MESHTASTIC_PROTO_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(MESHTASTIC_PROTO_SOURCES)
set(MESHTASTIC_PROTO_HEADERS)
foreach(PROTO_FILE IN LISTS MESHTASTIC_PROTO_FILES)
	get_filename_component(PROTO_NAME "${PROTO_FILE}" NAME_WE)
	list(APPEND MESHTASTIC_PROTO_SOURCES "${MESHTASTIC_PROTO_GENERATED_DIR}/meshtastic/${PROTO_NAME}.pb.cc")
	list(APPEND MESHTASTIC_PROTO_HEADERS "${MESHTASTIC_PROTO_GENERATED_DIR}/meshtastic/${PROTO_NAME}.pb.h")
endforeach()
add_custom_command(
	OUTPUT ${MESHTASTIC_PROTO_SOURCES} ${MESHTASTIC_PROTO_HEADERS}
	COMMAND ${CMAKE_COMMAND} -E make_directory "${MESHTASTIC_PROTO_GENERATED_DIR}"
	COMMAND ${Protobuf_PROTOC_EXECUTABLE}
		--proto_path=${MESHTASTIC_PROTOBUF_DIR}
		--cpp_out=${MESHTASTIC_PROTO_GENERATED_DIR}
		${MESHTASTIC_PROTO_FILES}
	DEPENDS ${MESHTASTIC_PROTO_FILES}
	VERBATIM
)
add_custom_target(meshtastic-protobufs DEPENDS ${MESHTASTIC_PROTO_SOURCES} ${MESHTASTIC_PROTO_HEADERS})

add_executable(meshat-monitor src/main.cpp ${MESHTASTIC_PROTO_SOURCES})
add_dependencies(meshat-monitor meshtastic-protobufs)
target_include_directories(meshat-monitor PRIVATE ${MOSQUITTO_INCLUDE_DIR} "${MESHTASTIC_PROTO_GENERATED_DIR}")
target_link_libraries(meshat-monitor PRIVATE ${MOSQUITTO_LIBRARY} SQLite::SQLite3 protobuf::libprotobuf)
if(OpenSSL_FOUND)
	target_link_libraries(meshat-monitor PRIVATE OpenSSL::Crypto)
	target_compile_definitions(meshat-monitor PRIVATE MTSCOPE_HAVE_OPENSSL=1)
endif()
target_compile_options(meshat-monitor PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS meshat-monitor RUNTIME DESTINATION bin)

if(BUILD_TESTING)
	include(FetchContent)
	FetchContent_Declare(
		googletest
		URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	)
	set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
	FetchContent_MakeAvailable(googletest)

	add_executable(meshat-monitor-tests tests/parser_test.cpp ${MESHTASTIC_PROTO_SOURCES})
	add_dependencies(meshat-monitor-tests meshtastic-protobufs)
	target_include_directories(meshat-monitor-tests PRIVATE ${MOSQUITTO_INCLUDE_DIR} "${MESHTASTIC_PROTO_GENERATED_DIR}")
	target_link_libraries(meshat-monitor-tests PRIVATE GTest::gtest_main ${MOSQUITTO_LIBRARY} SQLite::SQLite3 protobuf::libprotobuf)
	if(OpenSSL_FOUND)
		target_link_libraries(meshat-monitor-tests PRIVATE OpenSSL::Crypto)
		target_compile_definitions(meshat-monitor-tests PRIVATE MTSCOPE_HAVE_OPENSSL=1)
	endif()
	target_compile_options(meshat-monitor-tests PRIVATE -Wall -Wextra -Wpedantic)
	include(GoogleTest)
	gtest_discover_tests(meshat-monitor-tests)
endif()