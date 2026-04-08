#ifndef MY_FACE_IPC_OSD_DRAW_H
#define MY_FACE_IPC_OSD_DRAW_H

#include "ipc_proto.h"
#include <opencv2/core.hpp>

void ipc_draw_faces_osd(cv::Mat &draw_img, const ipc_ai_reply_t *reply);

#endif
