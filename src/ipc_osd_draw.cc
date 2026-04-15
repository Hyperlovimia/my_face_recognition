#include "ipc_osd_draw.h"
#include "setting.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cstdio>

void ipc_draw_faces_osd(cv::Mat &draw_img, const ipc_ai_reply_t *reply)
{
    if (!reply || reply->magic != IPC_MAGIC || reply->status != IPC_STATUS_OK)
        return;

    const int ref_w = AI_FRAME_WIDTH;
    const int ref_h = AI_FRAME_HEIGHT;

    for (int i = 0; i < reply->num_faces && i < IPC_MAX_FACES; i++)
    {
        const ipc_face_bundle_t &f = reply->faces[i];
        float bx = f.bbox.x, by = f.bbox.y, bw = f.bbox.w, bh = f.bbox.h;
        int x = (int)(bx / ref_w * draw_img.cols);
        int y = (int)(by / ref_h * draw_img.rows);
        int w = (int)(bw / ref_w * draw_img.cols);
        int h = (int)(bh / ref_h * draw_img.rows);

        const bool spoof = (f.is_live == 0);
        const cv::Scalar box_color = spoof ? cv::Scalar(0, 0, 255, 255) : cv::Scalar(255, 255, 255, 255);
        cv::rectangle(draw_img, cv::Rect(x, y, w, h), box_color, 2, 2, 0);

        char text[112];
        if (spoof)
        {
            snprintf(text, sizeof(text), "SPOOF %.2f", f.liveness_real_score);
            cv::putText(draw_img, text, {x, std::max(y - 10, 0)}, cv::FONT_HERSHEY_COMPLEX, 0.9,
                        cv::Scalar(0, 0, 255, 255), 1, 8, 0);
        }
        else if (f.rec.id == -1)
        {
            cv::putText(draw_img, "unknown", {x, std::max(y - 10, 0)}, cv::FONT_HERSHEY_COMPLEX, 1.0,
                        cv::Scalar(255, 0, 255, 255), 1, 8, 0);
        }
        else
        {
            snprintf(text, sizeof(text), "%s:%.2f", f.rec.name, f.rec.score);
            cv::putText(draw_img, text, {x, std::max(y - 10, 0)}, cv::FONT_HERSHEY_COMPLEX, 1.0,
                        cv::Scalar(255, 255, 0, 255), 1, 8, 0);
        }
    }
}
