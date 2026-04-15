set(common_ai_src ai_utils.cc ai_base.cc face_detection.cc face_recognition.cc anchors_320.cc anchors_640.cc)
set(face_ai_src face_ai_main.cc ipc_shm.cc ipc_lwp_syscall.c ${common_ai_src} face_antispoof.cc)

set(mpp_link -Wl,--start-group sys vicap vb isp vo connector sensor atomic gsdma -Wl,--end-group)
set(opencv_link opencv_imgcodecs opencv_imgproc opencv_core zlib libjpeg-turbo libopenjp2 libpng libtiff libwebp csi_cv)
set(nncase_link -Wl,--start-group rvv Nncase.Runtime.Native nncase.rt_modules.k230 functional_k230 sys atomic gsdma -Wl,--end-group)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${nncase_sdk_root}/riscv64/rvvlib/include)
include_directories(${k230_mpp_root}/userapps/api/)
include_directories(${k230_mpp_root}/include)
include_directories(${k230_mpp_root}/include/comm)
include_directories(${k230_mpp_root}/userapps/sample/sample_vo)
link_directories(${nncase_sdk_root}/riscv64/rvvlib/)

# 单进程原版（保留）
add_executable(face_recognition.elf main.cc video_pipeline.cc ${common_ai_src})
target_link_libraries(face_recognition.elf ${nncase_link} ${mpp_link} ${opencv_link})

# 三进程：视频采集与显示
add_executable(face_video.elf face_video_main.cc ipc_shm.cc ipc_lwp_syscall.c ipc_osd_draw.cc video_pipeline.cc)
target_link_libraries(face_video.elf ${mpp_link} ${opencv_link})

# 三进程：AI 推理（含可选活体 FaceAntiSpoof）
add_executable(face_ai.elf ${face_ai_src})
target_link_libraries(face_ai.elf ${nncase_link} ${opencv_link})

# 三进程：事件 / 考勤日志
add_executable(face_event.elf face_event_main.cc ipc_shm.cc ipc_lwp_syscall.c)
target_link_libraries(face_event.elf sys atomic)

install(TARGETS face_recognition.elf face_video.elf face_ai.elf face_event.elf DESTINATION bin)
