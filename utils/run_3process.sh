# face_ai 第 6 个参数建议写绝对路径，例如 /sharefs/face_db（避免 cwd 变化）
./face_ai.elf face_detection_320.kmodel 0.38 0.30 GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel 68 /sharefs/face_db 0 face_antispoof.kmodel 0.50 real0 &
./face_video.elf 0 &
./face_event.elf /tmp/attendance.log
