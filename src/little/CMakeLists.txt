# 小核 face_gateway：由 k230 buildroot 包
# k230_sdk/src/little/buildroot-ext/package/face_gateway/face_gateway.mk 指向本目录
get_filename_component(FOLDER_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

set(EXE_TARGET face_gateway)

add_executable(${EXE_TARGET}
    src/main.cpp)

set(IPCMSG_PATH $ENV{CDK_SRC_DIR}/user/component/ipcmsg)
set(MPP_PATH $ENV{MPP_SRC_DIR})

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
