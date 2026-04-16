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
#include "face_detection_scrfd.h"
#include <algorithm>
#include <cmath>

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

FaceDetectionSCRFD::FaceDetectionSCRFD(const char *kmodel_file, float obj_thresh, float nms_thresh, FrameCHWSize image_size, int debug_mode)
    : obj_thresh_(obj_thresh), AIBase(kmodel_file, "FaceDetectionSCRFD", debug_mode)
{
    model_name_ = "FaceDetectionSCRFD";
    nms_thresh_ = nms_thresh;
    image_size_ = image_size;
    input_size_ = {input_shapes_[0][1], input_shapes_[0][2], input_shapes_[0][3]};
    
    ai2d_out_tensor_ = get_input_tensor(0);
    Utils::padding_resize_one_side_set(image_size_, input_size_, ai2d_builder_, cv::Scalar(104, 117, 123));
    
    generate_anchor_centers();
}

FaceDetectionSCRFD::~FaceDetectionSCRFD()
{
}

bool FaceDetectionSCRFD::pre_process(runtime_tensor &input_tensor)
{
    ScopedTiming st(model_name_ + " pre_process", debug_mode_);
    auto r = ai2d_builder_->invoke(input_tensor, ai2d_out_tensor_);
    return r.is_ok();
}

bool FaceDetectionSCRFD::inference()
{
    return try_run() && try_get_output();
}

void FaceDetectionSCRFD::generate_anchor_centers()
{
    int input_h = input_size_.height;
    int input_w = input_size_.width;
    
    for (int level = 0; level < SCRFD_NUM_FPN_LEVELS; level++) {
        int stride = feat_stride_fpn_[level];
        int feat_h = input_h / stride;
        int feat_w = input_w / stride;
        
        anchor_centers_[level].clear();
        
        for (int h = 0; h < feat_h; h++) {
            for (int w = 0; w < feat_w; w++) {
                for (int a = 0; a < num_anchors_; a++) {
                    SCRFDAnchorCenter center;
                    center.x = (w + 0.5f) * stride;
                    center.y = (h + 0.5f) * stride;
                    anchor_centers_[level].push_back(center);
                }
            }
        }
        
        num_anchors_per_level_[level] = anchor_centers_[level].size();
        
        if (debug_mode_ > 1) {
            std::cout << "Level " << level << " (stride " << stride << "): "
                      << num_anchors_per_level_[level] << " anchor centers" << std::endl;
        }
    }
}

void FaceDetectionSCRFD::distance2bbox(float *anchor_center, float *distance, float *bbox)
{
    float cx = anchor_center[0];
    float cy = anchor_center[1];
    
    float x0 = cx - distance[0];
    float y0 = cy - distance[1];
    float x1 = cx + distance[2];
    float y1 = cy + distance[3];
    
    bbox[0] = x0;
    bbox[1] = y0;
    bbox[2] = x1 - x0;
    bbox[3] = y1 - y0;
}

void FaceDetectionSCRFD::distance2kps(float *anchor_center, float *distance, float *kps)
{
    float cx = anchor_center[0];
    float cy = anchor_center[1];
    
    for (int i = 0; i < 5; i++) {
        kps[i * 2 + 0] = cx + distance[i * 2 + 0];
        kps[i * 2 + 1] = cy + distance[i * 2 + 1];
    }
}

float FaceDetectionSCRFD::overlap(float x1, float w1, float x2, float w2)
{
    float l1 = x1 - w1 / 2;
    float l2 = x2 - w2 / 2;
    float left = l1 > l2 ? l1 : l2;
    float r1 = x1 + w1 / 2;
    float r2 = x2 + w2 / 2;
    float right = r1 < r2 ? r1 : r2;
    return right - left;
}

float FaceDetectionSCRFD::box_intersection(Bbox a, Bbox b)
{
    float w = overlap(a.x + a.w / 2, a.w, b.x + b.w / 2, b.w);
    float h = overlap(a.y + a.h / 2, a.h, b.y + b.h / 2, b.h);
    if (w < 0 || h < 0)
        return 0;
    return w * h;
}

float FaceDetectionSCRFD::box_union(Bbox a, Bbox b)
{
    float i = box_intersection(a, b);
    float u = a.w * a.h + b.w * b.h - i;
    return u;
}

float FaceDetectionSCRFD::box_iou(Bbox a, Bbox b)
{
    return box_intersection(a, b) / box_union(a, b);
}

void FaceDetectionSCRFD::nms(vector<FaceDetectionInfo> &results)
{
    std::sort(results.begin(), results.end(), [](const FaceDetectionInfo &a, const FaceDetectionInfo &b) {
        return a.score > b.score;
    });
    
    vector<bool> suppressed(results.size(), false);
    
    for (size_t i = 0; i < results.size(); i++) {
        if (suppressed[i])
            continue;
        
        for (size_t j = i + 1; j < results.size(); j++) {
            if (suppressed[j])
                continue;
            
            float iou = box_iou(results[i].bbox, results[j].bbox);
            if (iou > nms_thresh_) {
                suppressed[j] = true;
            }
        }
    }
    
    vector<FaceDetectionInfo> new_results;
    for (size_t i = 0; i < results.size(); i++) {
        if (!suppressed[i]) {
            new_results.push_back(results[i]);
        }
    }
    results = new_results;
}

void FaceDetectionSCRFD::post_process(FrameCHWSize frame_size, vector<FaceDetectionInfo> &results)
{
    ScopedTiming st(model_name_ + " post_process", debug_mode_);
    results.clear();
    
    int score_offset = 0;
    int bbox_offset = 3;
    int kps_offset = 6;
    
    for (int level = 0; level < SCRFD_NUM_FPN_LEVELS; level++) {
        float *score_ptr = p_outputs_[score_offset + level];
        float *bbox_ptr = p_outputs_[bbox_offset + level];
        float *kps_ptr = p_outputs_[kps_offset + level];
        
        int num_anchors = num_anchors_per_level_[level];
        
        int output_shape_size = 1;
        for (auto dim : output_shapes_[score_offset + level]) {
            output_shape_size *= dim;
        }
        
        int actual_num = output_shape_size;
        
        for (int i = 0; i < actual_num && i < num_anchors; i++) {
            float score = score_ptr[i];
            
            if (score < obj_thresh_)
                continue;
            
            FaceDetectionInfo info;
            info.score = score;
            
            float anchor_center[2] = {anchor_centers_[level][i].x, anchor_centers_[level][i].y};
            
            float bbox_dist[4];
            for (int j = 0; j < 4; j++) {
                bbox_dist[j] = bbox_ptr[i * 4 + j];
            }
            
            float bbox[4];
            distance2bbox(anchor_center, bbox_dist, bbox);
            
            info.bbox.x = bbox[0];
            info.bbox.y = bbox[1];
            info.bbox.w = bbox[2];
            info.bbox.h = bbox[3];
            
            float kps_dist[10];
            for (int j = 0; j < 10; j++) {
                kps_dist[j] = kps_ptr[i * 10 + j];
            }
            
            distance2kps(anchor_center, kps_dist, info.sparse_kps.points);
            
            results.push_back(info);
        }
    }
    
    nms(results);
    
    int max_src_size = std::max(frame_size.width, frame_size.height);
    float scale_x = (float)frame_size.width / input_size_.width;
    float scale_y = (float)frame_size.height / input_size_.height;
    
    for (size_t i = 0; i < results.size(); i++) {
        auto &b = results[i].bbox;
        b.x = b.x * scale_x;
        b.y = b.y * scale_y;
        b.w = b.w * scale_x;
        b.h = b.h * scale_y;
        
        b.x = std::max(0.0f, std::min(b.x, (float)frame_size.width));
        b.y = std::max(0.0f, std::min(b.y, (float)frame_size.height));
        b.w = std::min(b.w, (float)frame_size.width - b.x);
        b.h = std::min(b.h, (float)frame_size.height - b.y);
        
        auto &l = results[i].sparse_kps;
        for (int j = 0; j < 5; j++) {
            l.points[j * 2 + 0] = l.points[j * 2 + 0] * scale_x;
            l.points[j * 2 + 1] = l.points[j * 2 + 1] * scale_y;
            
            l.points[j * 2 + 0] = std::max(0.0f, std::min(l.points[j * 2 + 0], (float)frame_size.width));
            l.points[j * 2 + 1] = std::max(0.0f, std::min(l.points[j * 2 + 1], (float)frame_size.height));
        }
    }
}

void FaceDetectionSCRFD::draw_result(cv::Mat& src_img, vector<FaceDetectionInfo>& results, bool pic_mode)
{
    int src_w = src_img.cols;
    int src_h = src_img.rows;
    
    for (size_t i = 0; i < results.size(); i++) {
        auto& l = results[i].sparse_kps;
        for (uint32_t ll = 0; ll < 5; ll++) {
            if (pic_mode) {
                int32_t x0 = l.points[2 * ll + 0];
                int32_t y0 = l.points[2 * ll + 1];
                cv::circle(src_img, cv::Point(x0, y0), 2, color_list_for_det[ll], 4);
            } else {
                int32_t x0 = l.points[2 * ll + 0] / image_size_.width * src_w;
                int32_t y0 = l.points[2 * ll + 1] / image_size_.height * src_h;
                cv::circle(src_img, cv::Point(x0, y0), 4, color_list_for_osd_det[ll], 8);
            }
        }

        auto& b = results[i].bbox;
        char text[10];
        sprintf(text, "%.2f", results[i].score);
        
        if (pic_mode) {
            cv::rectangle(src_img, cv::Rect(b.x, b.y, b.w, b.h), cv::Scalar(255, 255, 255), 2, 2, 0);
            cv::putText(src_img, text, {static_cast<int>(b.x), static_cast<int>(b.y)}, cv::FONT_HERSHEY_COMPLEX, 0.5, cv::Scalar(0, 255, 255), 1, 8, 0);
        } else {
            int x = b.x / image_size_.width * src_w;
            int y = b.y / image_size_.height * src_h;
            int w = b.w / image_size_.width * src_w;
            int h = b.h / image_size_.height * src_h;
            cv::rectangle(src_img, cv::Rect(x, y, w, h), cv::Scalar(255, 255, 255, 255), 6, 2, 0);
        }
    }
}
