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

/**
 * 最小活体 kmodel 验证程序：加载模型、打印 I/O、单张图推理。
 *
 * 用法:
 *   fas_test.elf <kmodel_path> <image_path>
 *
 * 类别顺序（须与你的 ONNX 导出时 class 顺序一致，常见为按字母序 [real, spoof] 或训练时 Dataset 顺序）：
 *   下面默认假定 output[0] = SPOOF 分数/ logits，output[1] = REAL。
 *   若实机结果反了，请交换 kIdxSpoof / kIdxReal 或改训练标签映射。
 */

#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>

#include "face_antispoof.h"

using std::cerr;
using std::cout;
using std::endl;

namespace
{

constexpr int kIdxSpoof = 0;
constexpr int kIdxReal = 1;

void softmax2(float a, float b, float *o0, float *o1)
{
    const float m = (std::max)(a, b);
    const float e0 = std::exp(a - m);
    const float e1 = std::exp(b - m);
    const float s = e0 + e1;
    *o0 = e0 / s;
    *o1 = e1 / s;
}

static void print_usage(const char *prog)
{
    cerr << "Usage: " << prog << " <kmodel_path> <image_path>\n"
         << "Example: " << prog << " utils/face_antispoof.kmodel test.jpg\n";
}

} // namespace

int main(int argc, char **argv)
{
    cout << "fas_test built at " << __DATE__ << " " << __TIME__ << endl;

    if (argc != 3)
    {
        print_usage(argv[0]);
        return 1;
    }

    try
    {
        FaceAntiSpoof fas(argv[1], 2);

        cout << "Model loaded: ok" << endl;
        fas.print_cached_shapes();
        cout << "--- dump_model_io ---" << endl;
        fas.dump_model_io();
        cout << "---------------------" << endl;

        if (!fas.feed_image(argv[2]))
        {
            cerr << "feed_image failed" << endl;
            return 2;
        }

        if (!fas.forward())
        {
            cerr << "inference failed (try_run / try_get_output)" << endl;
            return 3;
        }

        if (fas.last_output_ptrs().empty())
        {
            cerr << "no output mapped" << endl;
            return 4;
        }

        const float *out = fas.last_output_ptrs()[0];
        size_t n = 1;
        if (!fas.cached_output_shapes().empty())
        {
            const auto &sh = fas.cached_output_shapes()[0];
            n = 1;
            for (size_t d : sh)
                n *= static_cast<size_t>(d);
        }

        cout << "Output elements: " << n << endl;
        cout << "Raw output: [";
        cout << std::fixed << std::setprecision(6);
        for (size_t i = 0; i < n; ++i)
        {
            if (i)
                cout << ", ";
            cout << out[i];
        }
        cout << "]" << endl;

        if (n >= 2)
        {
            const float v0 = out[0];
            const float v1 = out[1];
            float p0 = v0, p1 = v1;
            const float sumab = std::fabs(v0 + v1 - 1.0f);
            if (sumab > 0.08f)
            {
                softmax2(v0, v1, &p0, &p1);
                cout << "(Applied softmax; raw values did not look like probabilities.)" << endl;
            }

            const float spoof_score = p0; // 对应 kIdxSpoof
            const float real_score = p1;  // 对应 kIdxReal

            cout << std::setprecision(6);
            cout << "Real score:  " << real_score << "  (index " << kIdxReal << ")" << endl;
            cout << "Spoof score: " << spoof_score << "  (index " << kIdxSpoof << ")" << endl;

            const char *label = (real_score >= spoof_score) ? "REAL" : "SPOOF";
            cout << "Result: " << label << endl;
        }
        else
        {
            cout << "Output dim < 2; skip binary score decode. Check model head." << endl;
        }

        return 0;
    }
    catch (const std::exception &e)
    {
        cerr << "Model loaded: FAIL (" << e.what() << ")" << endl;
        return 10;
    }
}
