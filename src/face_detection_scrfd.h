#ifndef _FACE_DETECTION_SCRFD_H
#define _FACE_DETECTION_SCRFD_H

#include <iostream>
#include <vector>
#include "ai_utils.h"
#include "ai_base.h"

using std::vector;

typedef struct BboxSCRFD
{
    float x;
    float y;
    float w;
    float h;
} BboxSCRFD;

typedef struct SparseLandmarksSCRFD
{
    float points[10];
} SparseLandmarksSCRFD;

typedef struct FaceDetectionSCRFDInfo
{
    BboxSCRFD bbox;
    SparseLandmarksSCRFD sparse_kps;
    float score;
} FaceDetectionSCRFDInfo;

class FaceDetectionSCRFD : public AIBase
{
public:
    FaceDetectionSCRFD(const char *kmodel_file, float obj_thresh, float nms_thresh, FrameCHWSize image_size, int debug_mode);
    ~FaceDetectionSCRFD();

    bool pre_process(runtime_tensor &input_tensor);
    bool inference();
    void post_process(FrameCHWSize frame_size, vector<FaceDetectionSCRFDInfo> &results);
    void draw_result(cv::Mat& src_img, vector<FaceDetectionSCRFDInfo>& results, bool pic_mode = true);

private:
    void decode_single_stride(float *scores, float *bboxes, float *kps, int stride, 
                               int feat_h, int feat_w, vector<FaceDetectionSCRFDInfo> &faces);
    float compute_iou(BboxSCRFD &a, BboxSCRFD &b);
    void nms(vector<FaceDetectionSCRFDInfo> &faces, float nms_thresh);

    std::unique_ptr<ai2d_builder> ai2d_builder_;
    runtime_tensor ai2d_out_tensor_;
    FrameCHWSize image_size_;
    FrameCHWSize input_size_;

    float obj_thresh_;
    float nms_thresh_;
    int input_size_scrfd_;
    int strides_[3];
    int num_anchors_;
};

#endif
