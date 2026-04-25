# 小核 face_gateway：由 k230 buildroot 包
# k230_sdk/src/little/buildroot-ext/package/face_gateway/face_gateway.mk 指向本目录
# Buildroot 会注入 CDK_SRC_DIR / MPP_SRC_DIR；若手工 cmake，则从本目录推 SDK 根
get_filename_component(FOLDER_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
# little/ -> .../k230_sdk: 6 levels up
get_filename_component(_K230_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../" ABSOLUTE)
if(DEFINED ENV{CDK_SRC_DIR} AND NOT "$ENV{CDK_SRC_DIR}" STREQUAL "")
  set(CDK_SRC_DIR "$ENV{CDK_SRC_DIR}")
else()
  set(CDK_SRC_DIR "${_K230_ROOT}/src/common/cdk")
endif()
if(DEFINED ENV{MPP_SRC_DIR} AND NOT "$ENV{MPP_SRC_DIR}" STREQUAL "")
  set(MPP_PATH "$ENV{MPP_SRC_DIR}")
else()
  set(MPP_PATH "${_K230_ROOT}/src/big/mpp")
endif()
set(IPCMSG_PATH "${CDK_SRC_DIR}/user/component/ipcmsg")

set(EXE_TARGET face_gateway)

add_executable(${EXE_TARGET}
    src/main.cpp)

target_include_directories(${EXE_TARGET}
    PRIVATE ${IPCMSG_PATH}/include
    PRIVATE ${MPP_PATH}/include
    PRIVATE ${MPP_PATH}/include/comm
    PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)

target_link_directories(${EXE_TARGET}
    PRIVATE ${IPCMSG_PATH}/host/lib)

target_link_libraries(${EXE_TARGET}
    PRIVATE pthread
    PRIVATE ipcmsg)

target_compile_features(${EXE_TARGET}
    PRIVATE cxx_std_17)

install(TARGETS ${EXE_TARGET}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
