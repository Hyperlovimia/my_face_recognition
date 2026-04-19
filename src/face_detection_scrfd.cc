#include "face_detection_scrfd.h"
#include "k230_math.h"

cv::Scalar color_list_for_scrfd[] = {
    cv::Scalar(0, 0, 255),
    cv::Scalar(0, 255, 255),
    cv::Scalar(255, 0, 255),
    cv::Scalar(0, 255, 0),
    cv::Scalar(255, 0, 0)
};

cv::Scalar color_list_for_osd_scrfd[] = {
    cv::Scalar(255, 0, 0, 255),
    cv::Scalar(255, 0, 255, 255),
    cv::Scalar(255, 255, 0, 255),
    cv::Scalar(255, 0, 255, 0),
    cv::Scalar(255, 255, 0, 0)
};

FaceDetectionSCRFD::FaceDetectionSCRFD(const char *kmodel_file, float obj_thresh, float nms_thresh, 
                                        FrameCHWSize image_size, int debug_mode)
    : obj_thresh_(obj_thresh), nms_thresh_(nms_thresh), AIBase(kmodel_file, "FaceDetectionSCRFD", debug_mode)
{
    model_name_ = "FaceDetectionSCRFD";
    input_size_scrfd_ = input_shapes_[0][2];
    strides_[0] = 8;
    strides_[1] = 16;
    strides_[2] = 32;
    num_anchors_ = 2;
    
    image_size_ = image_size;
    input_size_ = {input_shapes_[0][1], input_shapes_[0][2], input_shapes_[0][3]};
    ai2d_out_tensor_ = get_input_tensor(0);
    Utils::padding_resize_one_side_set(image_size_, input_size_, ai2d_builder_, cv::Scalar(114, 114, 114));
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

void FaceDetectionSCRFD::decode_single_stride(float *scores, float *bboxes, float *kps, int stride,
                                               int feat_h, int feat_w, vector<FaceDetectionSCRFDInfo> &faces)
{
    for (int h = 0; h < feat_h; h++)
    {
        for (int w = 0; w < feat_w; w++)
        {
            for (int a = 0; a < num_anchors_; a++)
            {
                int idx = h * feat_w * num_anchors_ + w * num_anchors_ + a;
                float score = scores[idx];
                
                if (score > obj_thresh_)
                {
                    FaceDetectionSCRFDInfo face;
                    face.score = score;
                    
                    float cx = (w + 0.5f) * stride;
                    float cy = (h + 0.5f) * stride;
                    
                    float x1 = cx - bboxes[idx * 4 + 0] * stride;
                    float y1 = cy - bboxes[idx * 4 + 1] * stride;
                    float x2 = cx + bboxes[idx * 4 + 2] * stride;
                    float y2 = cy + bboxes[idx * 4 + 3] * stride;
                    
                    face.bbox.x = x1;
                    face.bbox.y = y1;
                    face.bbox.w = x2 - x1;
                    face.bbox.h = y2 - y1;
                    
                    for (int k = 0; k < 5; k++)
                    {
                        face.sparse_kps.points[k * 2] = cx + kps[idx * 10 + k * 2] * stride;
                        face.sparse_kps.points[k * 2 + 1] = cy + kps[idx * 10 + k * 2 + 1] * stride;
                    }
                    
                    faces.push_back(face);
                }
            }
        }
    }
}

float FaceDetectionSCRFD::compute_iou(BboxSCRFD &a, BboxSCRFD &b)
{
    float ax1 = a.x, ay1 = a.y, ax2 = a.x + a.w, ay2 = a.y + a.h;
    float bx1 = b.x, by1 = b.y, bx2 = b.x + b.w, by2 = b.y + b.h;
    
    float x1 = std::max(ax1, bx1);
    float y1 = std::max(ay1, by1);
    float x2 = std::min(ax2, bx2);
    float y2 = std::min(ay2, by2);
    
    float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    float area_a = a.w * a.h;
    float area_b = b.w * b.h;
    float union_area = area_a + area_b - inter;
    
    if (union_area <= 0.0f)
        return 0.0f;
    
    return inter / union_area;
}

void FaceDetectionSCRFD::nms(vector<FaceDetectionSCRFDInfo> &faces, float nms_thresh)
{
    if (faces.empty())
        return;
    
    std::sort(faces.begin(), faces.end(), [](const FaceDetectionSCRFDInfo &a, const FaceDetectionSCRFDInfo &b) {
        return a.score > b.score;
    });
    
    vector<bool> suppressed(faces.size(), false);
    
    for (size_t i = 0; i < faces.size(); i++)
    {
        if (suppressed[i])
            continue;
        
        for (size_t j = i + 1; j < faces.size(); j++)
        {
            if (suppressed[j])
                continue;
            
            float iou = compute_iou(faces[i].bbox, faces[j].bbox);
            if (iou >= nms_thresh)
            {
                suppressed[j] = true;
            }
        }
    }
    
    vector<FaceDetectionSCRFDInfo> remaining;
    for (size_t i = 0; i < faces.size(); i++)
    {
        if (!suppressed[i])
        {
            remaining.push_back(faces[i]);
        }
    }
    faces = remaining;
}

void FaceDetectionSCRFD::post_process(FrameCHWSize frame_size, vector<FaceDetectionSCRFDInfo> &results)
{
    ScopedTiming st(model_name_ + " post_process", debug_mode_);
    results.clear();
    
    vector<FaceDetectionSCRFDInfo> all_faces;
    
    for (int i = 0; i < 3; i++)
    {
        int stride = strides_[i];
        int feat_h = input_size_scrfd_ / stride;
        int feat_w = input_size_scrfd_ / stride;
        
        float *scores = p_outputs_[i];
        float *bboxes = p_outputs_[i + 3];
        float *kps = p_outputs_[i + 6];
        
        decode_single_stride(scores, bboxes, kps, stride, feat_h, feat_w, all_faces);
    }
    
    nms(all_faces, nms_thresh_);
    
    int max_src_size = std::max(frame_size.width, frame_size.height);
    float scale = (float)max_src_size / input_size_scrfd_;
    
    for (auto &face : all_faces)
    {
        float x1 = face.bbox.x * scale;
        float y1 = face.bbox.y * scale;
        float x2 = (face.bbox.x + face.bbox.w) * scale;
        float y2 = (face.bbox.y + face.bbox.h) * scale;
        
        x1 = std::max(0.0f, std::min(x1, (float)frame_size.width));
        y1 = std::max(0.0f, std::min(y1, (float)frame_size.height));
        x2 = std::max(0.0f, std::min(x2, (float)frame_size.width));
        y2 = std::max(0.0f, std::min(y2, (float)frame_size.height));
        
        float w = x2 - x1;
        float h = y2 - y1;
        
        if (w <= 0 || h <= 0)
            continue;
        
        face.bbox.x = x1;
        face.bbox.y = y1;
        face.bbox.w = w;
        face.bbox.h = h;
        
        for (int k = 0; k < 5; k++)
        {
            face.sparse_kps.points[k * 2] *= scale;
            face.sparse_kps.points[k * 2 + 1] *= scale;
        }
        
        results.push_back(face);
    }
}

void FaceDetectionSCRFD::draw_result(cv::Mat& src_img, vector<FaceDetectionSCRFDInfo>& results, bool pic_mode)
{
    int src_w = src_img.cols;
    int src_h = src_img.rows;
    
    for (size_t i = 0; i < results.size(); i++)
    {
        auto& l = results[i].sparse_kps;
        for (uint32_t ll = 0; ll < 5; ll++)
        {
            if (pic_mode)
            {
                int32_t x0 = l.points[2 * ll + 0];
                int32_t y0 = l.points[2 * ll + 1];
                cv::circle(src_img, cv::Point(x0, y0), 2, color_list_for_scrfd[ll], 4);
            }
            else
            {
                int32_t x0 = l.points[2 * ll + 0] / image_size_.width * src_w;
                int32_t y0 = l.points[2 * ll + 1] / image_size_.height * src_h;
                cv::circle(src_img, cv::Point(x0, y0), 4, color_list_for_osd_scrfd[ll], 8);
            }
        }

        auto& b = results[i].bbox;
        char text[10];
        sprintf(text, "%.2f", results[i].score);
        
        if (pic_mode)
        {
            cv::rectangle(src_img, cv::Rect(b.x, b.y, b.w, b.h), cv::Scalar(255, 255, 255), 2, 2, 0);
            cv::putText(src_img, text, cv::Point(b.x, b.y), cv::FONT_HERSHEY_COMPLEX, 0.5, cv::Scalar(0, 255, 255), 1, 8, 0);
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
