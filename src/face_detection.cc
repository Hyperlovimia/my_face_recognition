/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "face_detection.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

cv::Scalar color_list_for_det[] = {
    cv::Scalar(0, 0, 255),
    cv::Scalar(0, 255, 255),
    cv::Scalar(255, 0, 255),
    cv::Scalar(0, 255, 0),
    cv::Scalar(255, 0, 0)
};

cv::Scalar color_list_for_osd_det[] = {
    cv::Scalar(255, 0, 0, 255),
    cv::Scalar(255, 0, 255, 255),
    cv::Scalar(255, 255, 0, 255),
    cv::Scalar(255, 0, 255, 0),
    cv::Scalar(255, 255, 0, 0)
};

namespace {

static inline float sigmoid(float x)
{
    if (x > 50.0f) return 1.0f;
    if (x < -50.0f) return 0.0f;
    return 1.0f / (1.0f + expf(-x));
}

struct YoloCandidate {
    float x1, y1, x2, y2;
    float score;
    float kps[10];
};

int yolo_nms_comparator(const void *pa, const void *pb)
{
    const auto *a = static_cast<const YoloCandidate *>(pa);
    const auto *b = static_cast<const YoloCandidate *>(pb);
    float diff = a->score - b->score;
    if (diff < 0) return 1;
    if (diff > 0) return -1;
    return 0;
}

float yolo_box_iou(const YoloCandidate &a, const YoloCandidate &b)
{
    float xx1 = std::max(a.x1, b.x1);
    float yy1 = std::max(a.y1, b.y1);
    float xx2 = std::min(a.x2, b.x2);
    float yy2 = std::min(a.y2, b.y2);
    float w = std::max(0.0f, xx2 - xx1);
    float h = std::max(0.0f, yy2 - yy1);
    float inter = w * h;
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float denom = area_a + area_b - inter;
    if (denom <= 0.0f) return 0.0f;
    return inter / denom;
}

} // namespace

FaceDetection::FaceDetection(const char *kmodel_file, float obj_thresh, float nms_thresh,
                             FrameCHWSize image_size, int debug_mode)
    : obj_thresh_(obj_thresh), AIBase(kmodel_file, "FaceDetection", debug_mode)
{
    model_name_ = "FaceDetection";
    nms_thresh_ = nms_thresh;
    image_size_ = image_size;
    input_size_ = {input_shapes_[0][1], input_shapes_[0][2], input_shapes_[0][3]};

    ai2d_out_tensor_ = get_input_tensor(0);
    Utils::resize_set(image_size_, input_size_, ai2d_builder_);
}

FaceDetection::~FaceDetection()
{
}

bool FaceDetection::pre_process(runtime_tensor &input_tensor)
{
    ScopedTiming st(model_name_ + " pre_process", debug_mode_);
    auto r = ai2d_builder_->invoke(input_tensor, ai2d_out_tensor_);
    return r.is_ok();
}

bool FaceDetection::inference()
{
    return try_run() && try_get_output();
}

void FaceDetection::post_process(FrameCHWSize frame_size, vector<FaceDetectionInfo> &results)
{
    ScopedTiming st(model_name_ + " post_process", debug_mode_);
    results.clear();

    float *output = p_outputs_[0];
    const int num_anchors = output_shapes_[0][2];
    const float scale_x = static_cast<float>(frame_size.width) / static_cast<float>(input_size_.width);
    const float scale_y = static_cast<float>(frame_size.height) / static_cast<float>(input_size_.height);

    std::vector<YoloCandidate> candidates;
    candidates.reserve(128);

    for (int i = 0; i < num_anchors; ++i) {
        float conf_logit = output[4 * num_anchors + i];
        float conf = sigmoid(conf_logit);
        if (conf < obj_thresh_)
            continue;

        float cx = output[0 * num_anchors + i];
        float cy = output[1 * num_anchors + i];
        float bw = output[2 * num_anchors + i];
        float bh = output[3 * num_anchors + i];

        YoloCandidate cand;
        cand.x1 = (cx - bw * 0.5f) * scale_x;
        cand.y1 = (cy - bh * 0.5f) * scale_y;
        cand.x2 = (cx + bw * 0.5f) * scale_x;
        cand.y2 = (cy + bh * 0.5f) * scale_y;
        cand.score = conf;

        for (int k = 0; k < 5; ++k) {
            cand.kps[2 * k + 0] = output[(5 + k * 3 + 0) * num_anchors + i] * scale_x;
            cand.kps[2 * k + 1] = output[(5 + k * 3 + 1) * num_anchors + i] * scale_y;
        }

        candidates.push_back(cand);
    }

    if (candidates.empty())
        return;

    std::qsort(candidates.data(), candidates.size(), sizeof(YoloCandidate), yolo_nms_comparator);

    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].score < obj_thresh_)
            continue;

        FaceDetectionInfo obj;
        obj.bbox.x = std::max(0.0f, std::min(candidates[i].x1, static_cast<float>(frame_size.width)));
        obj.bbox.y = std::max(0.0f, std::min(candidates[i].y1, static_cast<float>(frame_size.height)));
        float x2 = std::max(0.0f, std::min(candidates[i].x2, static_cast<float>(frame_size.width)));
        float y2 = std::max(0.0f, std::min(candidates[i].y2, static_cast<float>(frame_size.height)));
        obj.bbox.w = x2 - obj.bbox.x;
        obj.bbox.h = y2 - obj.bbox.y;
        obj.score = candidates[i].score;

        for (int k = 0; k < 10; ++k) {
            obj.sparse_kps.points[k] = candidates[i].kps[k];
        }

        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (candidates[j].score < obj_thresh_)
                continue;
            if (yolo_box_iou(candidates[i], candidates[j]) >= nms_thresh_)
                candidates[j].score = 0.0f;
        }

        results.push_back(obj);
    }
}

void FaceDetection::draw_result(cv::Mat& src_img, vector<FaceDetectionInfo>& results, bool pic_mode)
{
    int src_w = src_img.cols;
    int src_h = src_img.rows;
    for (int i = 0; i < results.size(); ++i)
    {
        auto& l = results[i].sparse_kps;
        for (uint32_t ll = 0; ll < 5; ll++)
        {
            if (pic_mode)
            {
                int32_t x0 = l.points[2 * ll + 0];
                int32_t y0 = l.points[2 * ll + 1];
                cv::circle(src_img, cv::Point(x0, y0), 2, color_list_for_det[ll], 4);
            }
            else
            {
                int32_t x0 = l.points[2 * ll] / image_size_.width * src_w;
                int32_t y0 = l.points[2 * ll + 1] / image_size_.height * src_h;
                cv::circle(src_img, cv::Point(x0, y0), 4, color_list_for_osd_det[ll], 8);
            }
        }

        auto& b = results[i].bbox;
        char text[10];
        sprintf(text, "%.2f", results[i].score);
        if (pic_mode)
        {
            cv::rectangle(src_img, cv::Rect(b.x, b.y, b.w, b.h), cv::Scalar(255, 255, 255), 2, 2, 0);
            cv::putText(src_img, text, {b.x, b.y}, cv::FONT_HERSHEY_COMPLEX, 0.5, cv::Scalar(0, 255, 255), 1, 8, 0);
        }
        else
        {
            int x = b.x / image_size_.width * src_w;
            int y = b.y / image_size_.height * src_h;
            int w = b.w / image_size_.width * src_w;
            int h = b.h / image_size_.height * src_h;
            cv::rectangle(src_img, cv::Rect(x, y, w, h), cv::Scalar(255, 255, 255, 255), 6, 2, 0);
        }
    }
}