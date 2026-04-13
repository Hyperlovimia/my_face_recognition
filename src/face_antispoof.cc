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

#include "face_antispoof.h"

#include <cmath>
#include <cstring>
#include <iostream>

#include <nncase/tensor.h>

#include "ai_utils.h"

using std::cerr;
using std::endl;
using namespace nncase;
using namespace nncase::runtime;
using namespace nncase::runtime::detail;

namespace
{

/**
 * 与 Python 校准脚本对齐：
 * BGR→RGB（调用方完成）、最长边等比缩放到 max(th,tw)、反射 padding 到 th×tw。
 * BORDER_REFLECT_101 对应 cv::BORDER_REFLECT101（与 cv2.BORDER_REFLECT_101 一致）。
 */
cv::Mat preprocess_reflect_pad_rgb(const cv::Mat &rgb, int target_h, int target_w)
{
    const int w = rgb.cols;
    const int h = rgb.rows;
    const int long_side = (std::max)(w, h);
    const int scale_denom = (std::max)(long_side, 1);
    const float scale = static_cast<float>((std::max)(target_h, target_w)) / static_cast<float>(scale_denom);

    int new_w = static_cast<int>(std::lround(static_cast<float>(w) * scale));
    int new_h = static_cast<int>(std::lround(static_cast<float>(h) * scale));
    new_w = (std::max)(new_w, 1);
    new_h = (std::max)(new_h, 1);

    cv::Mat resized;
    cv::resize(rgb, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);

    const int pad_top = (target_h - new_h) / 2;
    const int pad_bottom = target_h - new_h - pad_top;
    const int pad_left = (target_w - new_w) / 2;
    const int pad_right = target_w - new_w - pad_left;

    cv::Mat padded;
    cv::copyMakeBorder(resized, padded, pad_top, pad_bottom, pad_left, pad_right, cv::BORDER_REFLECT101);
    return padded;
}

} // namespace

FaceAntiSpoof::FaceAntiSpoof(const char *kmodel_file, int debug_mode)
    : AIBase(kmodel_file, "FaceAntiSpoof", debug_mode)
{
    model_name_ = "FaceAntiSpoof";
}

void FaceAntiSpoof::print_cached_shapes(std::ostream &os) const
{
    if (!input_shapes_.empty())
    {
        const auto &s = input_shapes_[0];
        os << "Input shape: [";
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (i)
                os << ", ";
            os << s[i];
        }
        os << "]" << std::endl;
    }
    if (!output_shapes_.empty())
    {
        const auto &s = output_shapes_[0];
        os << "Output shape: [";
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (i)
                os << ", ";
            os << s[i];
        }
        os << "]" << std::endl;
    }
}

bool FaceAntiSpoof::feed_image(const std::string &image_path)
{
    cv::Mat bgr = cv::imread(image_path, cv::IMREAD_COLOR);
    if (bgr.empty())
    {
        cerr << "feed_image: failed to read image: " << image_path << endl;
        return false;
    }

    if (input_shapes_.empty() || input_shapes_[0].size() < 4)
    {
        cerr << "feed_image: unexpected model input rank" << endl;
        return false;
    }

    const int Nexp = input_shapes_[0][0];
    const int Cexp = input_shapes_[0][1];
    const int Hexp = input_shapes_[0][2];
    const int Wexp = input_shapes_[0][3];
    const size_t expected_elems = static_cast<size_t>(Nexp * Cexp * Hexp * Wexp);

    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);

    cv::Mat padded = preprocess_reflect_pad_rgb(rgb, Hexp, Wexp);
    if (padded.rows != Hexp || padded.cols != Wexp)
    {
        cerr << "feed_image: internal error, padded size mismatch" << endl;
        return false;
    }

    runtime_tensor in_tensor = get_input_tensor(0);

    if (model_input_is_float32(0))
    {
        cv::Mat f32;
        padded.convertTo(f32, CV_32FC3, 1.0 / 255.0);

        std::vector<cv::Mat> ch;
        cv::split(f32, ch);
        if (static_cast<int>(ch.size()) != Cexp)
        {
            cerr << "feed_image: channel count mismatch" << endl;
            return false;
        }

        std::vector<float> nchw(expected_elems);
        const size_t plane = static_cast<size_t>(Hexp * Wexp);
        for (int c = 0; c < Cexp; ++c)
        {
            const float *src = reinterpret_cast<const float *>(ch[static_cast<size_t>(c)].data);
            std::memcpy(nchw.data() + static_cast<size_t>(c) * plane, src, plane * sizeof(float));
        }

        auto th_r = in_tensor.impl()->to_host();
        if (!th_r.is_ok())
        {
            cerr << "feed_image: to_host failed" << endl;
            return false;
        }
        nncase::tensor host_tensor = std::move(th_r.unwrap());
        auto bh_r = host_tensor->buffer().as_host();
        if (!bh_r.is_ok())
            return false;
        auto as_host = std::move(bh_r.unwrap());
        auto map_r = as_host.map(map_access_::map_write);
        if (!map_r.is_ok())
            return false;
        auto mapped = std::move(map_r.unwrap());
        auto buf = mapped.buffer();
        if (buf.size_bytes() < expected_elems * sizeof(float))
        {
            cerr << "feed_image: input buffer too small for float32" << endl;
            return false;
        }
        std::memcpy(reinterpret_cast<char *>(buf.data()), nchw.data(), expected_elems * sizeof(float));

        auto sync_r = hrt::sync(in_tensor, sync_op_t::sync_write_back, true);
        if (!sync_r.is_ok())
        {
            cerr << "feed_image: sync_write_back failed" << endl;
            return false;
        }
        return true;
    }

    if (model_input_is_uint8(0))
    {
        std::vector<cv::Mat> ch;
        cv::split(padded, ch);
        if (static_cast<int>(ch.size()) != Cexp)
            return false;

        std::vector<uint8_t> nchw(expected_elems);
        const size_t plane = static_cast<size_t>(Hexp * Wexp);
        for (int c = 0; c < Cexp; ++c)
        {
            const uint8_t *src = ch[static_cast<size_t>(c)].data;
            std::memcpy(nchw.data() + static_cast<size_t>(c) * plane, src, plane);
        }

        auto th_r = in_tensor.impl()->to_host();
        if (!th_r.is_ok())
            return false;
        nncase::tensor host_tensor = std::move(th_r.unwrap());
        auto bh_r = host_tensor->buffer().as_host();
        if (!bh_r.is_ok())
            return false;
        auto as_host = std::move(bh_r.unwrap());
        auto map_r = as_host.map(map_access_::map_write);
        if (!map_r.is_ok())
            return false;
        auto mapped = std::move(map_r.unwrap());
        auto buf = mapped.buffer();
        if (buf.size_bytes() < expected_elems)
        {
            cerr << "feed_image: input buffer too small for uint8" << endl;
            return false;
        }
        std::memcpy(reinterpret_cast<char *>(buf.data()), nchw.data(), expected_elems);

        auto sync_r = hrt::sync(in_tensor, sync_op_t::sync_write_back, true);
        return sync_r.is_ok();
    }

    cerr << "feed_image: unsupported input dtype (see dump_model_io); only float32 / uint8 handled here"
         << endl;
    return false;
}

bool FaceAntiSpoof::forward()
{
    return try_run() && try_get_output();
}
