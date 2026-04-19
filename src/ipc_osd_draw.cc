#include "ipc_osd_draw.h"
#include "setting.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cstdio>

namespace {

cv::Rect bbox_to_osd_rect(float bx, float by, float bw, float bh, int ref_w, int ref_h, int dst_w, int dst_h)
{
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    if (DISPLAY_ROTATE) {
        x = (ref_h - (by + bh)) / ref_h * dst_w;
        y = bx / ref_w * dst_h;
        w = bh / ref_h * dst_w;
        h = bw / ref_w * dst_h;
    } else {
        x = bx / ref_w * dst_w;
        y = by / ref_h * dst_h;
        w = bw / ref_w * dst_w;
        h = bh / ref_h * dst_h;
    }

    int ix = std::max(0, static_cast<int>(x));
    int iy = std::max(0, static_cast<int>(y));
    int iw = std::max(1, static_cast<int>(w));
    int ih = std::max(1, static_cast<int>(h));

    if (ix >= dst_w || iy >= dst_h) {
        return cv::Rect(0, 0, 0, 0);
    }

    iw = std::min(iw, dst_w - ix);
    ih = std::min(ih, dst_h - iy);
    return cv::Rect(ix, iy, iw, ih);
}

/* OSD 缓冲按 ARGB8888 消费，OpenCV 这里只是“4 通道原始字节写入”，
 * 因此颜色常量需要按 A,R,G,B 的字节顺序构造。 */
cv::Scalar osd_argb(unsigned char a, unsigned char r, unsigned char g, unsigned char b)
{
    return cv::Scalar(a, r, g, b);
}

}  // namespace

void ipc_draw_faces_osd(cv::Mat &draw_img, const ipc_ai_reply_t *reply)
{
    if (!reply || reply->magic != IPC_MAGIC || reply->status != IPC_STATUS_OK)
        return;

    const int ref_w = AI_FRAME_WIDTH;
    const int ref_h = AI_FRAME_HEIGHT;

    for (int i = 0; i < reply->num_faces && i < IPC_MAX_FACES; i++)
    {
        const ipc_face_bundle_t &f = reply->faces[i];
        cv::Rect rect = bbox_to_osd_rect(f.bbox.x, f.bbox.y, f.bbox.w, f.bbox.h,
                                         ref_w, ref_h, draw_img.cols, draw_img.rows);
        if (rect.width <= 0 || rect.height <= 0)
            continue;

        const bool spoof = (f.is_live == 0);
        const cv::Scalar box_color = spoof ? osd_argb(255, 255, 0, 0)
                                           : osd_argb(255, 255, 255, 255);
        cv::rectangle(draw_img, rect, box_color, 2, 2, 0);

        char text[112];
        if (spoof)
        {
            snprintf(text, sizeof(text), "SPOOF %.2f", f.liveness_real_score);
            cv::putText(draw_img, text, {rect.x, std::max(rect.y - 10, 0)}, cv::FONT_HERSHEY_COMPLEX, 0.9,
                        osd_argb(255, 255, 0, 0), 1, 8, 0);
        }
        else if (f.rec.id == -1)
        {
            cv::putText(draw_img, "unknown", {rect.x, std::max(rect.y - 10, 0)}, cv::FONT_HERSHEY_COMPLEX, 1.0,
                        osd_argb(255, 255, 0, 255), 1, 8, 0);
        }
        else
        {
            snprintf(text, sizeof(text), "%s:%.2f", f.rec.name, f.rec.score);
            cv::putText(draw_img, text, {rect.x, std::max(rect.y - 10, 0)}, cv::FONT_HERSHEY_COMPLEX, 1.0,
                        osd_argb(255, 255, 255, 0), 1, 8, 0);
        }
    }
}
