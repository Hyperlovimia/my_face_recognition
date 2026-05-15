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
#include "setting.h"

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

struct ScrfCandidate {
    float x1, y1, x2, y2;
    float score;
    float kps[10];
};

int scrfd_nms_comparator(const void *pa, const void *pb)
{
    const auto *a = static_cast<const ScrfCandidate *>(pa);
    const auto *b = static_cast<const ScrfCandidate *>(pb);
    float diff = a->score - b->score;
    if (diff < 0) return 1;
    if (diff > 0) return -1;
    return 0;
}

float scrfd_box_iou(const ScrfCandidate &a, const ScrfCandidate &b)
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

    const int strides[3] = {8, 16, 32};
    const int num_anchors = 2;
    const int input_size = static_cast<int>(input_size_.width);
    const float scale_x = static_cast<float>(frame_size.width) / static_cast<float>(input_size);
    const float scale_y = static_cast<float>(frame_size.height) / static_cast<float>(input_size);

    std::vector<ScrfCandidate> candidates;
    candidates.reserve(256);

    for (int idx = 0; idx < 3; ++idx) {
        int stride = strides[idx];
        float *scores = p_outputs_[idx];
        float *bboxes = p_outputs_[idx + 3];
        float *kps = p_outputs_[idx + 6];

        int height = input_size / stride;
        int width = input_size / stride;
        int num_cells = height * width * num_anchors;

        for (int h = 0; h < height; ++h) {
            for (int w = 0; w < width; ++w) {
                for (int a = 0; a < num_anchors; ++a) {
                    int i = h * width * num_anchors + w * num_anchors + a;
                    float score = scores[i];
                    if (score < obj_thresh_)
                        continue;

                    float cx = (static_cast<float>(w) + 0.5f) * static_cast<float>(stride);
                    float cy = (static_cast<float>(h) + 0.5f) * static_cast<float>(stride);

                    float dx1 = bboxes[i * 4 + 0];
                    float dy1 = bboxes[i * 4 + 1];
                    float dx2 = bboxes[i * 4 + 2];
                    float dy2 = bboxes[i * 4 + 3];

                    ScrfCandidate cand;
                    cand.x1 = (cx - dx1 * static_cast<float>(stride)) * scale_x;
                    cand.y1 = (cy - dy1 * static_cast<float>(stride)) * scale_y;
                    cand.x2 = (cx + dx2 * static_cast<float>(stride)) * scale_x;
                    cand.y2 = (cy + dy2 * static_cast<float>(stride)) * scale_y;
                    cand.score = score;

                    for (int k = 0; k < 5; ++k) {
                        float kpx = kps[i * 10 + k * 2 + 0];
                        float kpy = kps[i * 10 + k * 2 + 1];
                        cand.kps[2 * k + 0] = (cx + kpx * static_cast<float>(stride)) * scale_x;
                        cand.kps[2 * k + 1] = (cy + kpy * static_cast<float>(stride)) * scale_y;
                    }

                    candidates.push_back(cand);
                }
            }
        }
    }

    if (candidates.empty())
        return;

    std::qsort(candidates.data(), candidates.size(), sizeof(ScrfCandidate), scrfd_nms_comparator);

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
            if (scrfd_box_iou(candidates[i], candidates[j]) >= nms_thresh_)
                candidates[j].score = 0.0f;
        }

        results.push_back(obj);
    }
}

void FaceDetection::draw_result(cv::Mat& src_img, vector<FaceDetectionInfo>& results, bool pic_mode)
{
    int src_w = src_img.cols;
    int src_h = src_img.rows;
    int ref_w = static_cast<int>(image_size_.width);
    int ref_h = static_cast<int>(image_size_.height);
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
                float px = l.points[2 * ll + 0];
                float py = l.points[2 * ll + 1];
                int32_t x0, y0;
                if (DISPLAY_ROTATE) {
                    x0 = static_cast<int32_t>((ref_h - py) / ref_h * src_w);
                    y0 = static_cast<int32_t>(px / ref_w * src_h);
                } else {
                    x0 = static_cast<int32_t>(px / ref_w * src_w);
                    y0 = static_cast<int32_t>(py / ref_h * src_h);
                }
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
            int x, y, w, h;
            if (DISPLAY_ROTATE) {
                x = static_cast<int>((ref_h - (b.y + b.h)) / ref_h * src_w);
                y = static_cast<int>(b.x / ref_w * src_h);
                w = static_cast<int>(b.h / ref_h * src_w);
                h = static_cast<int>(b.w / ref_w * src_h);
            } else {
                x = static_cast<int>(b.x / ref_w * src_w);
                y = static_cast<int>(b.y / ref_h * src_h);
                w = static_cast<int>(b.w / ref_w * src_w);
                h = static_cast<int>(b.h / ref_h * src_h);
            }
            cv::rectangle(src_img, cv::Rect(x, y, w, h), cv::Scalar(255, 255, 255, 255), 6, 2, 0);
        }
    }
}