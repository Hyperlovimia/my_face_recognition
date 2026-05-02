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
#ifndef FACE_ANTISPOOF_H
#define FACE_ANTISPOOF_H

#include <iostream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "ai_base.h"

/**
 * 人脸活体（静默）模型最小封装：继承 AIBase，只做与训练对齐的预处理 + 推理。
 */
class FaceAntiSpoof : public AIBase
{
public:
    /**
     * @param real_class_is_output_index0 仅当 ONNX/kmodel 约定 out[0]=REAL、out[1]=SPOOF 时置 true；
     *        默认 false：与工程约定一致，out[0]=SPOOF、out[1]=REAL。
     */
    FaceAntiSpoof(const char *kmodel_file, int debug_mode = 2, bool real_class_is_output_index0 = false);

    /**
     * 按训练脚本：BGR→RGB、最长边等比缩放、反射 padding、/255、HWC→CHW，写入输入 tensor。
     * uint8 输入时写入 0~255 未除 255（与 float 校准链路不同，仅兼容部分量化 kmodel）。
     */
    bool feed_image(const std::string &image_path);

    /** 与 feed_image 相同预处理，输入为 BGR 图（如从视频帧 ROI 截取）。 */
    bool feed_bgr_mat(const cv::Mat &bgr);

    /** try_run + try_get_output */
    bool forward();

    /**
     * forward() 且输出为 2 类后有效。默认：out[0]=SPOOF、out[1]=REAL（与 header 约定一致）；
     * 构造时 real_class_is_output_index0=true 时视为 out[0]=REAL。
     */
    bool decode_liveness_scores(float *real_prob, float *spoof_prob) const;

    /** 打印缓存的输入/输出 shape（与 AIBase 构造时一致） */
    void print_cached_shapes(std::ostream &os = std::cout) const;

    /** forward() / get_output 之后有效：各输出 tensor 映射后的 float 指针（与 AIBase 一致） */
    const std::vector<float *> &last_output_ptrs() const { return p_outputs_; }

    const std::vector<std::vector<int>> &cached_output_shapes() const { return output_shapes_; }

private:
    bool real_class_is_output_index0_;
};

#endif
