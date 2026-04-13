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
#include "ai_base.h"
#include <iostream>
#include <cassert>
#include <fstream>
#include <string>
#include <nncase/runtime/debug.h>
#include <nncase/tensor.h>
#include "ai_utils.h"

using std::cout;
using std::endl;
using namespace nncase;
using namespace nncase::runtime;
using namespace nncase::runtime::k230;
using namespace nncase::F::k230;
using namespace nncase::runtime::detail;

/**
 * @brief AI 推理基类
 * 作用：
 *  - 加载 kmodel
 *  - 初始化输入输出 tensor
 *  - 执行推理
 *  - 获取输出数据
 *
 * 这是所有检测 / 分类 / 人脸识别模型的公共父类
 */
AIBase::AIBase(const char *kmodel_file,const string model_name, const int debug_mode)
    : debug_mode_(debug_mode),model_name_(model_name)
{
    // 打印模型路径（调试等级 >1）
    if (debug_mode > 1)
        cout << "kmodel_file:" << kmodel_file << endl;

    // 以二进制方式打开 kmodel 文件
    std::ifstream ifs(kmodel_file, std::ios::binary);

    // 加载 kmodel 到解释器（nncase runtime）
    kmodel_interp_.load_model(ifs).expect("Invalid kmodel");

    // 初始化输入 tensor
    set_input_init();

    // 初始化输出信息
    set_output_init();
}

AIBase::~AIBase()
{
    // 当前没有资源需要手动释放
}

////////////////////////////////////////////////////////////
/// 输入初始化
////////////////////////////////////////////////////////////
    void AIBase::set_input_init()
{
    // 性能计时（可选）
    ScopedTiming st(model_name_ + " set_input init", debug_mode_);

    int input_total_size = 0;

    // 遍历模型所有输入
    for (int i = 0; i < kmodel_interp_.inputs_size(); ++i)
    {
        // 获取输入描述信息（数据类型）
        auto desc = kmodel_interp_.input_desc(i);

        // 获取输入 shape
        auto shape = kmodel_interp_.input_shape(i);

        // 在 host 侧创建 tensor（共享内存池）
        auto tensor = host_runtime_tensor::create(
            desc.datatype, shape, hrt::pool_shared
        ).expect("cannot create input tensor");

        // 将 tensor 绑定到解释器输入
        kmodel_interp_.input_tensor(i, tensor).expect("cannot set input tensor");

        vector<int> in_shape;

        if (debug_mode_ > 1)
            cout<<"input "<< std::to_string(i) <<" : "<<to_string(desc.datatype)<<",";

        int dsize = 1;

        // 记录 shape 维度
        for (int j = 0; j < shape.size(); ++j)
        {
            in_shape.push_back(shape[j]);
            dsize *= shape[j];   // 计算元素数量

            if (debug_mode_ > 1)
                cout << shape[j] << ",";
        }

        if (debug_mode_ > 1)
            cout << endl;

        // 保存输入 shape
        input_shapes_.push_back(in_shape);
    }
}

////////////////////////////////////////////////////////////
/// 获取输入 tensor（给外部写数据用）
////////////////////////////////////////////////////////////
runtime_tensor AIBase::get_input_tensor(size_t idx)
{
    return kmodel_interp_.input_tensor(idx).expect("cannot get input tensor");
}

////////////////////////////////////////////////////////////
/// 手动设置输入 tensor（外部创建好后替换）
////////////////////////////////////////////////////////////
void AIBase::set_input_tensor(size_t idx,runtime_tensor &input_tensor){
    kmodel_interp_.input_tensor(idx, input_tensor).expect("cannot set input tensor");
}

////////////////////////////////////////////////////////////
/// 输出初始化（只记录 shape，不分配内存）
////////////////////////////////////////////////////////////
void AIBase::set_output_init()
{
    ScopedTiming st(model_name_ + " set_output_init", debug_mode_);
    int output_total_size = 0;

    // 遍历所有输出
    for (size_t i = 0; i < kmodel_interp_.outputs_size(); i++)
    {
        auto desc = kmodel_interp_.output_desc(i);
        auto shape = kmodel_interp_.output_shape(i);

        vector<int> out_shape;

        if (debug_mode_ > 1)
            cout<<"output "<<std::to_string(i)<<" : "<<to_string(desc.datatype)<<",";

        int dsize = 1;

        for (int j = 0; j < shape.size(); ++j)
        {
            out_shape.push_back(shape[j]);
            dsize *= shape[j];

            if (debug_mode_ > 1)
                cout << shape[j] << ",";
        }

        if (debug_mode_ > 1)
            cout << endl;

        // 保存输出 shape
        output_shapes_.push_back(out_shape);
    }
}

////////////////////////////////////////////////////////////
/// 执行推理
////////////////////////////////////////////////////////////
void AIBase::run()
{
    ScopedTiming st(model_name_ + " run", debug_mode_);

    // 调用 nncase 执行模型
    kmodel_interp_.run().expect("error occurred in running model");
}

bool AIBase::try_run()
{
    ScopedTiming st(model_name_ + " run", debug_mode_);
    auto r = kmodel_interp_.run();
    return r.is_ok();
}

////////////////////////////////////////////////////////////
/// 读取输出数据（映射到 host 内存）
////////////////////////////////////////////////////////////
void AIBase::get_output()
{
    ScopedTiming st(model_name_ + " get_output", debug_mode_);

    // 清空旧指针
    p_outputs_.clear();

    for (int i = 0; i < kmodel_interp_.outputs_size(); i++)
    {
        // 获取输出 tensor
        auto out = kmodel_interp_.output_tensor(i).expect("cannot get output tensor");

        /**
         * 这里是关键：
         * device tensor → host tensor → map → 得到 CPU 可访问指针
         */
        auto buf = out.impl()
                        ->to_host().unwrap()
                        ->buffer().as_host().unwrap()
                        .map(map_access_::map_read).unwrap()
                        .buffer();

        // 默认按 float 解析
        float *p_out = reinterpret_cast<float *>(buf.data());

        // 保存输出指针（生命周期由 tensor 控制）
        p_outputs_.push_back(p_out);
    }
}

bool AIBase::try_get_output()
{
    ScopedTiming st(model_name_ + " get_output", debug_mode_);
    p_outputs_.clear();

    for (int i = 0; i < kmodel_interp_.outputs_size(); i++)
    {
        auto out_r = kmodel_interp_.output_tensor(i);
        if (!out_r.is_ok())
            return false;
        runtime_tensor out = std::move(out_r.unwrap());
        auto th_r = out.impl()->to_host();
        if (!th_r.is_ok())
            return false;
        nncase::tensor host_tensor = std::move(th_r.unwrap());
        auto bh_r = host_tensor->buffer().as_host();
        if (!bh_r.is_ok())
            return false;
        auto as_host = std::move(bh_r.unwrap());
        auto map_r = as_host.map(map_access_::map_read);
        if (!map_r.is_ok())
            return false;
        auto mapped = std::move(map_r.unwrap());
        auto buf = mapped.buffer();
        float *p_out = reinterpret_cast<float *>(buf.data());
        p_outputs_.push_back(p_out);
    }
    return true;
}

////////////////////////////////////////////////////////////
/// 获取某个输出 tensor（高级接口）
////////////////////////////////////////////////////////////
runtime_tensor AIBase::get_output_tensor(int idx){
    return kmodel_interp_.output_tensor(idx).expect("cannot get current output tensor");
}

void AIBase::dump_model_io()
{
    cout << "Model tensor count: inputs=" << kmodel_interp_.inputs_size()
         << ", outputs=" << kmodel_interp_.outputs_size() << endl;

    for (int i = 0; i < kmodel_interp_.inputs_size(); ++i)
    {
        auto desc = kmodel_interp_.input_desc(i);
        auto shape = kmodel_interp_.input_shape(i);
        cout << "  input[" << i << "] dtype=" << to_string(desc.datatype) << " shape=[";
        for (size_t j = 0; j < shape.size(); ++j)
        {
            if (j)
                cout << ",";
            cout << shape[j];
        }
        cout << "]" << endl;
    }

    for (size_t i = 0; i < kmodel_interp_.outputs_size(); ++i)
    {
        auto desc = kmodel_interp_.output_desc(i);
        auto shape = kmodel_interp_.output_shape(i);
        cout << "  output[" << i << "] dtype=" << to_string(desc.datatype) << " shape=[";
        for (size_t j = 0; j < shape.size(); ++j)
        {
            if (j)
                cout << ",";
            cout << shape[j];
        }
        cout << "]" << endl;
    }
}

bool AIBase::model_input_is_float32(size_t i) const
{
    return kmodel_interp_.input_desc(i).datatype == typecode_t::dt_float32;
}

bool AIBase::model_input_is_uint8(size_t i) const
{
    return kmodel_interp_.input_desc(i).datatype == typecode_t::dt_uint8;
}
