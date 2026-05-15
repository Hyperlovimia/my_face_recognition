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
#ifndef _FACE_RECOGNITION_H
#define _FACE_RECOGNITION_H

#include <cstddef>
#include <string>
#include <vector>
#include "ai_utils.h"
#include "ai_base.h"
#include "face_detection.h"

using std::vector;

typedef struct FaceRecognitionInfo
{
    int id = -1;                         // 人脸识别结果对应 ID；-1 为未识别
    float score = 0.f;                   // 人脸识别得分（与 top1 模板相似度，未识别时仍为 top1 分）
    string name;                         // 人脸识别结果对应人名
    int top1_id = -1;                    // 库内相似度最高者下标；无库或未搜索时为 -1
    bool ambiguous_match = false;        // 库内≥2 人且 top1/top2 歧义导致未识别（不宜做滞回提拔）
} FaceRecognitionInfo;

/**
 * @brief 基于mobile_facenet的人脸识别
 * 主要封装了对于每一帧图片，从预处理、运行到后处理给出结果的过程
 */
class FaceRecognition : public AIBase
{
public:
    /**
     * @brief FaceRecognition构造函数，加载kmodel,并初始化kmodel输入、输出和人脸检测阈值(for isp)
     * @param kmodel_file       kmodel文件路径
     * @param thresh            人脸识别阈值
     * @param image_size        输入大小（chw）
     * @param debug_mode        0（不调试）、 1（只显示时间）、2（显示所有打印信息）
     * @return None
     */
    FaceRecognition(char *kmodel_file, float thresh, FrameCHWSize isp_shape, int debug_mode,
                    float db_top2_margin = 5.0f);

    /**
     * @brief FaceRecognition析构函数
     * @return None
     */
    ~FaceRecognition();

    bool pre_process(runtime_tensor& input_tensor, float *sparse_points);

    /**
     * @brief 在 pre_process 成功后调用：将识别模型输入（仿射对齐后的 NCHW uint8，R/G/B 平面）转为 BGR，供活体等与识别同源对齐的链路使用。
     * @return false 表示维度过期或映射失败
     */
    bool aligned_face_to_bgr(cv::Mat &bgr) const;

    /**
     * @brief kmodel推理
     * @return false 表示 nncase 推理/读输出失败
     */
    bool inference();



    //--------------------------for database------------------------------------
    /**
     * @brief 人脸数据库加载接口
     * @param db_pth 数据库目录
     * @return None
     */
    void database_init(char *db_pth);

    /**
    * @brief 人脸数据库在线注册，运行过程中，增加新的人脸特征
    * @param name      注册人脸名称
    * @param feature   人脸特征向量
    * @return None
    */
    void database_add(std::string& name, float* feature);

    /**
     * @brief 在线注册：保存特征与画廊快照（整幅 ISP BGR，横版未旋转；与 face_video dump_img 一致，无检测框）
     * @param full_isp_bgr_landscape 注册当帧 camera/MPI dump，BGR；内部按 DISPLAY_ROTATE 与预览对齐后再写 JPEG
     */
    void database_add(std::string& name, char* db_path, const cv::Mat &full_isp_bgr_landscape);

    /**
     * @brief 从静态导入图片入库：保存特征与原始导入图缩放后的 JPEG，不额外做 camera/OSD 方向旋转
     * @param import_bgr 已解码的 BGR 图像；为空时回退到 aligned_face_to_bgr
     */
    void database_add_import(std::string& name, char* db_path, const cv::Mat &import_bgr);

    /**
     * @brief 人脸数据库重置，清除所有数据库人脸特征
     * @param db_pth 数据库目录
     * @return None
     */
    void database_reset(char *db_pth);

    /**
     * @brief 查询注册人数
     * @param db_pth 数据库目录
     * @return None
     */
    int database_count(char* db_pth);

    /**
     * @brief 人脸数据库查询接口
     * @param result 人脸识别结果
     * @param query_l2norm 已 L2 归一化的查询特征；nullptr 则用当前推理输出
     * @param frame_face_count 当前帧检出人数；≥2 时略放宽两可判决、与 face_ai 多人单帧特征策略配合
     * @return None
     */
    void database_search(FaceRecognitionInfo &result, const float *query_l2norm = nullptr,
                         int frame_face_count = 1);

    /** 当前推理输出特征的 L2 归一化拷贝（需在 inference() 成功后调用） */
    void export_query_l2_normalized(float *dst) const;

    int feature_dim() const { return feature_num_; }

    /** 识别阈值（与构造参数 thresh 一致，0～100 标尺） */
    float recognition_threshold() const { return obj_thresh_; }

    /** 当前已注册人脸数量 */
    int registered_face_count() const { return valid_register_face_; }

    /** 已注册姓名拷贝；idx 越界返回空串 */
    string registered_name_at(int idx) const;

    /**
     * @brief 将处理好的轮廓画到原图
     * @param src_img     原图
     * @param bbox        识别人脸的检测框位置
     * @param result      人脸识别结果
     * @param pic_mode    ture(原图片)，false(osd)
     * @return None
     */
    void draw_result(cv::Mat &draw_img, Bbox &bbox, FaceRecognitionInfo &result);

private:
    /** 
     * @brief svd
     * @param a     原始矩阵
     * @param u     左奇异向量
     * @param s     对角阵
     * @param v     右奇异向量
     * @return None
     */
    void svd22(const float a[4], float u[4], float s[2], float v[4]);
    
    /**
    * @brief 使用Umeyama算法计算仿射变换矩阵
    * @param src  原图像点位置
    * @param dst  目标图像（112*112）点位置。
    * @return None
    */
    void image_umeyama_112(float* src, float* dst);

    /**
    * @brief 获取affine变换矩阵
    * @param sparse_points  原图像人脸五官点位置
    * @return None
    */
    void get_affine_matrix(float* sparse_points);

    /**
    * @brief 使用L2范数对数据进行归一化
    * @param src  原始数据
    * @param dst  L2归一化后的数据
    * @param len  原始数据长度
    * @return None
    */
    void l2_normalize(const float* src, float* dst, int len) const;

    /**
    * @brief 计算两特征的余弦距离
    * @param feature_0    第一个特征
    * @param feature_1    第二个特征
    * @param feature_len  特征长度
    * @return 余弦距离
    */
    float cal_cosine_distance(const float* feature_0, const float* feature_1, int feature_len);
    
    std::unique_ptr<ai2d_builder> ai2d_builder_; // ai2d构建器
    runtime_tensor ai2d_out_tensor_;             // ai2d输出tensor
    
    FrameCHWSize image_size_;                     
    FrameCHWSize input_size_;
    bool input_is_nhwc_ = false;
    float matrix_dst_[10];                       // 人脸affine的变换矩阵
    int feature_num_;                             // 人脸识别提取特征长度
    float obj_thresh_;                            // 人脸识别阈值
    vector<string> names_;                        // 人脸数据库名字
    vector<vector<float>> features_;              // 人脸数据库特征
    int valid_register_face_;                     // 数据库中实际人脸个数
    float db_top2_margin_;                        // 第一、二高分至少相差此值(0~100 标尺)才确认身份，抑库内互串
};
#endif
