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
#ifndef _FACE_DETECTION_SCRFD_H
#define _FACE_DETECTION_SCRFD_H

#include <iostream>
#include <vector>
#include "ai_utils.h"
#include "ai_base.h"
#include "face_detection_common.h"

using std::vector;

#define SCRFD_KPS_SIZE 10
#define SCRFD_LOC_SIZE 4
#define SCRFD_NUM_ANCHORS 2
#define SCRFD_NUM_FPN_LEVELS 3

typedef struct SCRFDAnchorCenter
{
    float x;
    float y;
} SCRFDAnchorCenter;

class FaceDetectionSCRFD : public AIBase
{
public:
    FaceDetectionSCRFD(const char *kmodel_file, float obj_thresh, float nms_thresh, FrameCHWSize image_size, int debug_mode);
    ~FaceDetectionSCRFD();

    bool pre_process(runtime_tensor &input_tensor);
    bool inference();
    void post_process(FrameCHWSize frame_size, vector<FaceDetectionInfo> &results);
    void draw_result(cv::Mat& src_img, vector<FaceDetectionInfo>& results, bool pic_mode = true);

private:
    void generate_anchor_centers();
    void distance2bbox(float *anchor_center, float *distance, float *bbox);
    void distance2kps(float *anchor_center, float *distance, float *kps);
    float box_iou(Bbox a, Bbox b);
    float box_intersection(Bbox a, Bbox b);
    float box_union(Bbox a, Bbox b);
    float overlap(float x1, float w1, float x2, float w2);
    void nms(vector<FaceDetectionInfo> &results);

    std::unique_ptr<ai2d_builder> ai2d_builder_;
    runtime_tensor ai2d_out_tensor_;
    FrameCHWSize image_size_;
    FrameCHWSize input_size_;

    float obj_thresh_;
    float nms_thresh_;
    
    int feat_stride_fpn_[SCRFD_NUM_FPN_LEVELS] = {8, 16, 32};
    int num_anchors_ = SCRFD_NUM_ANCHORS;
    
    vector<SCRFDAnchorCenter> anchor_centers_[SCRFD_NUM_FPN_LEVELS];
    int num_anchors_per_level_[SCRFD_NUM_FPN_LEVELS];
};

#endif
