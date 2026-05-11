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
#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <vector>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <nncase/tensor.h>
#include "face_recognition.h"
#include "setting.h"

namespace {
// K230 musl 下 std::filesystem::create_directories 可能静默抛异常，
// 这里用 POSIX stat/mkdir 做一个可移植的实现。
/** db_dir + '/' + tail；去掉多余 '/'，避免出现 `/sharefs//face_db/…`。 */
std::string face_db_join_path(const char *dir, const std::string &tail)
{
    if (!dir || dir[0] == '\0')
        return tail;
    std::string d(dir);
    while (!d.empty() && (d.back() == '/' || d.back() == '\\'))
        d.pop_back();
    std::string t = tail;
    while (!t.empty() && (t.front() == '/' || t.front() == '\\'))
        t.erase(0, 1);
    return d.empty() ? t : d + "/" + t;
}

bool face_db_slot_has_pair(const char *dir, int slot)
{
    struct stat st_db{};
    struct stat st_nm{};
    const std::string dbf = face_db_join_path(dir, std::to_string(slot) + ".db");
    const std::string nf = face_db_join_path(dir, std::to_string(slot) + ".name");
    return stat(dbf.c_str(), &st_db) == 0 && S_ISREG(st_db.st_mode) && stat(nf.c_str(), &st_nm) == 0 &&
           S_ISREG(st_nm.st_mode);
}

/** 从 1 起连续存在的「index.db + index.name」数量（忽略 .jpg 等附加文件）。 */
int count_contiguous_face_slots(const char *dir)
{
    int n = 0;
    for (int i = 1; i <= 4096; ++i)
    {
        if (!face_db_slot_has_pair(dir, i))
            break;
        n = i;
    }
    return n;
}

bool ensure_dir_exists_posix(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return true;
        }
        std::cerr << "path exists but is not a directory: " << path << std::endl;
        return false;
    }
    if (errno != ENOENT) {
        std::cerr << "stat(" << path << ") failed: errno=" << errno << std::endl;
        return false;
    }
    if (mkdir(path, 0755) == 0) {
        std::cout << "人脸数据库不存在，已成功创建该目录: " << path << std::endl;
        return true;
    }
    std::cerr << "mkdir(" << path << ") failed: errno=" << errno << std::endl;
    return false;
}

cv::Rect bbox_to_osd_rect(const Bbox &bbox, int ref_w, int ref_h, int dst_w, int dst_h)
{
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    if (DISPLAY_ROTATE) {
        x = (ref_h - (bbox.y + bbox.h)) / ref_h * dst_w;
        y = bbox.x / ref_w * dst_h;
        w = bbox.h / ref_h * dst_w;
        h = bbox.w / ref_w * dst_h;
    } else {
        x = bbox.x / ref_w * dst_w;
        y = bbox.y / ref_h * dst_h;
        w = bbox.w / ref_w * dst_w;
        h = bbox.h / ref_h * dst_h;
    }

    int ix = std::max(0, static_cast<int>(x));
    int iy = std::max(0, static_cast<int>(y));
    int iw = std::max(1, static_cast<int>(w));
    int ih = std::max(1, static_cast<int>(h));

    if (ix >= dst_w || iy >= dst_h) {
        return cv::Rect(0, 0, 0, 0);
    }

    iw = std::min(iw, dst_w - ix);
    ih = std::min(ih, dst_h - iy);
    return cv::Rect(ix, iy, iw, ih);
}

std::string shape_to_string(const std::vector<int> &shape)
{
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < shape.size(); ++i)
    {
        if (i)
            oss << ",";
        oss << shape[i];
    }
    oss << "]";
    return oss.str();
}

std::string sparse_points_to_string(const float *sparse_points)
{
    std::ostringstream oss;
    oss << "[";
    for (int i = 0; i < 10; ++i)
    {
        if (i)
            oss << ",";
        oss << sparse_points[i];
    }
    oss << "]";
    return oss.str();
}
}  // namespace

using namespace nncase::runtime;
using namespace nncase::runtime::detail;

FaceRecognition::FaceRecognition(char *kmodel_file, float thresh, FrameCHWSize image_size,
                                 int debug_mode, float db_top2_margin)
    : AIBase(kmodel_file, "FaceRecognition", debug_mode)
{
	model_name_ = "FaceRecognition";
	obj_thresh_ = thresh;
    db_top2_margin_ = db_top2_margin;
	valid_register_face_ = 0;
	image_size_ = image_size;
    const std::vector<int> &raw_input_shape = input_shapes_[0];
    input_is_nhwc_ = raw_input_shape.size() == 4 && raw_input_shape[1] != 3 && raw_input_shape[3] == 3;
    if (input_is_nhwc_)
    {
        input_size_ = {static_cast<size_t>(raw_input_shape[3]), static_cast<size_t>(raw_input_shape[1]),
                       static_cast<size_t>(raw_input_shape[2])};
    }
    else
    {
        input_size_ = {static_cast<size_t>(input_shapes_[0][1]), static_cast<size_t>(input_shapes_[0][2]),
                       static_cast<size_t>(input_shapes_[0][3])};
    }
	feature_num_ = output_shapes_[0][1];
	ai2d_out_tensor_ = get_input_tensor(0);

    if (debug_mode_ > 0)
    {
        std::cerr << "[recg_ctor] raw input shape=" << shape_to_string(raw_input_shape)
                  << ", code assumes NCHW and interprets input_size as C="
                  << input_size_.channel << " H=" << input_size_.height
                  << " W=" << input_size_.width << std::endl;
        if (raw_input_shape.size() == 4 && raw_input_shape[1] != 3 && raw_input_shape[3] == 3)
        {
            std::cerr << "[recg_ctor] WARNING: model input looks NHWC; if so, expected logical CHW would be C=3 H="
                      << raw_input_shape[1] << " W=" << raw_input_shape[2]
                      << ", runtime will switch to RGB_packed/NHWC affine output for this model." << std::endl;
        }
        if (!output_shapes_.empty())
        {
            std::cerr << "[recg_ctor] output[0] shape=" << shape_to_string(output_shapes_[0]) << std::endl;
        }
        std::cerr.flush();
    }
}

FaceRecognition::~FaceRecognition()
{
}

string FaceRecognition::registered_name_at(int idx) const
{
    if (idx < 0 || idx >= valid_register_face_)
        return string();
    return names_[idx];
}

bool FaceRecognition::pre_process(runtime_tensor& input_tensor, float *sparse_points)
{
	ScopedTiming st(model_name_ + " pre_process", debug_mode_);
    if (debug_mode_ > 0)
    {
        std::cerr << "[recg_pre] preparing affine for model input C=" << input_size_.channel
                  << " H=" << input_size_.height << " W=" << input_size_.width
                  << ", sparse_points=" << sparse_points_to_string(sparse_points) << std::endl;
        std::cerr.flush();
    }
	get_affine_matrix(sparse_points);
    if (debug_mode_ > 0)
    {
        std::cerr << "[recg_pre] affine matrix=["
                  << matrix_dst_[0] << "," << matrix_dst_[1] << "," << matrix_dst_[2] << ","
                  << matrix_dst_[3] << "," << matrix_dst_[4] << "," << matrix_dst_[5] << "]"
                  << std::endl;
        std::cerr.flush();
    }
	Utils::affine_set(image_size_, input_size_, ai2d_builder_, matrix_dst_, input_is_nhwc_);
    if (debug_mode_ > 0)
    {
        std::cerr << "[recg_pre] ai2d builder ready, invoking affine warp now"
                  << " (output_layout=" << (input_is_nhwc_ ? "NHWC/RGB_packed" : "NCHW/RGB_planar") << ")"
                  << std::endl;
        std::cerr.flush();
    }
	auto r = ai2d_builder_->invoke(input_tensor, ai2d_out_tensor_);
    if (debug_mode_ > 0)
    {
        std::cerr << "[recg_pre] ai2d invoke result=" << (r.is_ok() ? "ok" : "fail") << std::endl;
        std::cerr.flush();
    }
	return r.is_ok();
}

bool FaceRecognition::aligned_face_to_bgr(cv::Mat &bgr) const
{
    const int C = (int)input_size_.channel;
    const int H = (int)input_size_.height;
    const int W = (int)input_size_.width;
    if (C != 3 || H <= 0 || W <= 0)
        return false;

    /* ai2d 绑定输入 tensor 上常为「设备侧带 padding / 非直观 strides」。手写 strides 索引易与
     * 运行时布局不一致 → JPEG 快照对角剪切；注册预览用的是整帧 BGR，看起来「正常」。
     * 先 copy_to 到新建的默认紧凑 tensor，再按 NHWC/NCHW 线性读取，由运行时保证语义正确。 */
    gsl::span<const size_t> sh = ai2d_out_tensor_.shape();
    if (sh.size() != 4 || ai2d_out_tensor_.datatype() != typecode_t::dt_uint8)
        return false;

    dims_t shape_vec;
    for (size_t i = 0; i < sh.size(); ++i)
        shape_vec.push_back(sh[i]);

    auto dst_res = host_runtime_tensor::create(typecode_t::dt_uint8, shape_vec, hrt::pool_shared);
    if (!dst_res.is_ok())
        return false;
    runtime_tensor dst = std::move(dst_res.unwrap());

    /* copy_to 是非常量成员函数；aligned_face_to_bgr 为 const，用句柄副本调用即可（共享同一缓冲）。 */
    runtime_tensor src_rt = ai2d_out_tensor_;
    if (!src_rt.copy_to(dst).is_ok())
        return false;

    auto th_r = dst.impl()->to_host();
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
    const uint8_t *src = reinterpret_cast<const uint8_t *>(buf.data());
    const size_t nbytes = buf.size_bytes();
    const size_t plane = static_cast<size_t>(H) * static_cast<size_t>(W);
    const size_t need = plane * static_cast<size_t>(C);
    if (nbytes < need)
        return false;

    bgr.create(H, W, CV_8UC3);

    if (input_is_nhwc_)
    {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const size_t off =
                    (static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)) * static_cast<size_t>(C);
                bgr.at<cv::Vec3b>(y, x) = cv::Vec3b(src[off + 0], src[off + 1], src[off + 2]);
            }
        return true;
    }

    const std::vector<cv::Mat> ch{
        cv::Mat(H, W, CV_8UC1, const_cast<uint8_t *>(src + 0 * plane)),
        cv::Mat(H, W, CV_8UC1, const_cast<uint8_t *>(src + 1 * plane)),
        cv::Mat(H, W, CV_8UC1, const_cast<uint8_t *>(src + 2 * plane)),
    };
    cv::merge(ch, bgr);
    return true;
}

bool FaceRecognition::inference()
{
	return try_run() && try_get_output();
}

void FaceRecognition::database_init(char *db_pth)
{
	ScopedTiming st(model_name_ + " database_init", debug_mode_);
	std::cerr << "[db_init] path=" << (db_pth ? db_pth : "(null)") << std::endl;
	std::cerr.flush();

	if (db_pth == nullptr) {
		std::cerr << "[db_init] db_pth is null, skip." << std::endl;
		return;
	}

	if (!ensure_dir_exists_posix(db_pth)) {
		std::cerr << "[db_init] ensure_dir_exists_posix failed, continue with empty db" << std::endl;
		valid_register_face_ = 0;
		std::cerr.flush();
		return;
	}
	std::cerr << "[db_init] directory ready" << std::endl;
	std::cerr.flush();

	const int file_num = count_contiguous_face_slots(db_pth);
	if (debug_mode_ > 0)
		std::cout << "found " << file_num << " contiguous db+.name pairs (jpg ignored)" << std::endl;
	std::cerr << "[db_init] slots=" << file_num << std::endl;
	std::cerr.flush();

	valid_register_face_ = 0;
	for (int i = 1; i <= file_num; ++i)
	{
		std::string fname = face_db_join_path(db_pth, std::to_string(i) + ".db");
		vector<float> db_vec = Utils::read_binary_file<float>(fname.c_str());
		if (db_vec.size() != static_cast<size_t>(feature_num_))
		{
			std::cerr << "[db_init] skip slot " << i << ": bad .db size=" << db_vec.size() << " expect=" << feature_num_
			          << std::endl;
			break;
		}
        l2_normalize(db_vec.data(), db_vec.data(), feature_num_);
		features_.push_back(std::move(db_vec));
		fname = face_db_join_path(db_pth, std::to_string(i) + ".name");
		vector<char> name_vec = Utils::read_binary_file<char>(fname.c_str());
		string current_name(name_vec.begin(), name_vec.end());
		names_.push_back(current_name);
		valid_register_face_ += 1;
	}
	std::cout << "init database Done!" << std::endl;
	std::cerr.flush();
	std::cout.flush();
}

namespace {

/** 与 face_video / main 注册预览一致：DISPLAY_ROTATE 时顺时针 90°。 */
static void orient_isp_snapshot_like_register_preview(const cv::Mat &landscape_bgr, cv::Mat &dst)
{
    if (landscape_bgr.empty() || landscape_bgr.type() != CV_8UC3)
    {
        dst.release();
        return;
    }
#if DISPLAY_ROTATE
    cv::rotate(landscape_bgr, dst, cv::ROTATE_90_CLOCKWISE);
#else
    landscape_bgr.copyTo(dst);
#endif
}

}  // namespace

void FaceRecognition::database_reset(char *db_pth){
	ScopedTiming st(model_name_ + " database_reset", debug_mode_);
    const int file_num = count_contiguous_face_slots(db_pth);
    if (debug_mode_ > 0)
        std::cout << "removing " << file_num << " pieces of data in db" << std::endl;
    // 遍历所有文件并删除
    for (int i = 1; i <= file_num; ++i)
    {
        std::string db_file = face_db_join_path(db_pth, std::to_string(i) + ".db");
        std::string name_file = face_db_join_path(db_pth, std::to_string(i) + ".name");
        std::string jpg_file = face_db_join_path(db_pth, std::to_string(i) + ".jpg");

        unlink(db_file.c_str());
        unlink(name_file.c_str());
        unlink(jpg_file.c_str());
    }

    // 清空内存中的缓存
    features_.clear();
    names_.clear();
    valid_register_face_ = 0;
    std::cout << "database reset Done!" << std::endl;
}

void FaceRecognition::database_add(std::string& name, char* db_path, const cv::Mat &full_isp_bgr_landscape)
{
	ScopedTiming st(model_name_ + " database_add", debug_mode_);
    // 计算保存的索引
    float* feature=p_outputs_[0];
    int save_index = valid_register_face_ + 1; // 文件名从1开始编号
    
    // 生成文件路径
    std::string feature_file = face_db_join_path(db_path, std::to_string(save_index) + ".db");
    std::string name_file = face_db_join_path(db_path, std::to_string(save_index) + ".name");

    // 写入 feature 到 .db 文件
    FILE* f_db = fopen(feature_file.c_str(), "wb");
    if (!f_db) {
        std::cerr << "Failed to open " << feature_file << " for writing." << std::endl;
        return;
    }
    fwrite(feature, sizeof(float), feature_num_, f_db);
    fclose(f_db);

    // 写入名字到 .name 文件
    FILE* f_name = fopen(name_file.c_str(), "wb");
    if (!f_name) {
        std::cerr << "Failed to open " << name_file << " for writing." << std::endl;
        return;
    }
    fwrite(name.c_str(), sizeof(char), name.length(), f_name);
    fclose(f_name);

    std::string jpg_file = face_db_join_path(db_path, std::to_string(save_index) + ".jpg");
    cv::Mat snapshot_oriented;
    orient_isp_snapshot_like_register_preview(full_isp_bgr_landscape, snapshot_oriented);

    if (!snapshot_oriented.empty())
    {
        /* ISP→dump_img 与 OSD 预览一致：Mat 三字节按 RGB 语义送给 VO（face_video convert_preview_to_osd_argb8888）。
         * cv::imwrite 则按 BGR 编码 JPEG，若不交换则在浏览器里会偏蓝/偏青。 */
        cv::Mat jpeg_bgr;
        cv::cvtColor(snapshot_oriented, jpeg_bgr, cv::COLOR_RGB2BGR);
        static const std::vector<int> k_jpeg_params{cv::IMWRITE_JPEG_QUALITY, 88};
        if (!cv::imwrite(jpg_file, jpeg_bgr, k_jpeg_params))
            std::cerr << "database_add: imwrite failed " << jpg_file << std::endl;
    }
    else
    {
        cv::Mat aligned_bgr;
        if (aligned_face_to_bgr(aligned_bgr) && !aligned_bgr.empty())
        {
            if (!cv::imwrite(jpg_file, aligned_bgr, std::vector<int>{cv::IMWRITE_JPEG_QUALITY, 88}))
                std::cerr << "database_add: imwrite failed " << jpg_file << std::endl;
        }
        else
            std::cerr << "database_add: skip gallery snapshot (no full frame + aligned_face_to_bgr failed)"
                      << std::endl;
    }

    std::vector<float> feature_vec(feature, feature + feature_num_);
    l2_normalize(feature_vec.data(), feature_vec.data(), feature_num_);
    features_.push_back(std::move(feature_vec));  // 避免拷贝
    names_.push_back(name);
    valid_register_face_ += 1;

    std::cout << "Saved feature and name to database successfully!" << std::endl;
}

int FaceRecognition::database_count(char* db_pth)
{
	ScopedTiming st(model_name_ + " database_count", debug_mode_);
    // int count = 0;
    // while (true)
    // {
    //     std::string fname = std::string(db_pth) + "/" + std::to_string(count + 1) + ".db";
    //     std::ifstream file_check(fname, std::ios::binary);
    //     if (!file_check.good()) {
    //         break;
    //     }
    //     ++count;
    // }
    // if (debug_mode_ > 0)
    //     std::cout << "Found " << count << " registered face(s) in " << db_pth << std::endl;
    // return count;
    return valid_register_face_;
}


void FaceRecognition::export_query_l2_normalized(float *dst) const
{
    if (!dst || feature_num_ <= 0 || p_outputs_.empty() || !p_outputs_[0])
        return;
    l2_normalize(p_outputs_[0], dst, feature_num_);
}

void FaceRecognition::database_search(FaceRecognitionInfo &result, const float *query_l2norm, int frame_face_count)
{
	ScopedTiming st(model_name_ + " database_search", debug_mode_);
	int i;
	int v_id = -1;
	float v_score;
	float v_score_top1 = -1.f;
	float v_score_top2 = -1.f;
	float testf[feature_num_];
	if (query_l2norm)
		std::memcpy(testf, query_l2norm, sizeof(float) * (size_t)feature_num_);
	else
		l2_normalize(p_outputs_[0], testf, feature_num_);
	for (i = 0; i < valid_register_face_; i++)
	{
        const float* cur_feature = features_[i].data();
		v_score = cal_cosine_distance(testf, cur_feature, feature_num_);
        if (v_score > v_score_top1)
        {
            v_score_top2 = v_score_top1;
            v_score_top1 = v_score;
            v_id = i;
        }
        else if (v_score > v_score_top2)
        {
            v_score_top2 = v_score;
        }
	}
	const bool above_thresh = (v_score_top1 > obj_thresh_);
	/* 第二名若未贴近识别阈，视为「仅能匹配一人」，不判两可 */
	const bool second_plausible =
	    (valid_register_face_ >= 2) && (v_score_top2 > obj_thresh_ - 0.5f);
	float margin_need = db_top2_margin_;
	if (valid_register_face_ == 2)
		margin_need = (std::min)(margin_need, (std::max)(3.2f, db_top2_margin_ * 0.58f));
	if (frame_face_count >= 2)
		margin_need = (std::min)(margin_need, (std::max)(2.9f, db_top2_margin_ * 0.52f));
	const float gap = v_score_top1 - v_score_top2;
	/* Top1 显著高于阈值时允许在略小分差下仍采信第一名，减轻同框两人时的 unknown */
	const bool strong_top1 = (v_score_top1 >= obj_thresh_ + 4.5f);
	const bool ambiguous =
	    (valid_register_face_ >= 2) && second_plausible && (gap < margin_need) && !strong_top1;
	result.top1_id = v_id;
	result.ambiguous_match = ambiguous;
	if (v_id == -1 || !above_thresh || ambiguous)
	{
		result.id = -1;
		result.name = "unknown";
		/* 保留与库里的最高相似度，便于区分「真陌生人」与「过阈但歧义/抖动」；纯无库时 top1 仍为 -1 */
		result.score = (v_score_top1 > 0.f) ? v_score_top1 : 0.f;
        if (debug_mode_ > 1 && valid_register_face_ >= 2 && above_thresh && ambiguous)
        {
            std::cout << "face_rec: ambiguous match (top1=" << v_score_top1 << " top2=" << v_score_top2
                      << " margin_need>=" << margin_need << ") -> unknown\n";
        }
	}
	else
	{
		result.id = v_id;
		result.name = names_[v_id];
		result.score = v_score_top1;
	}
}

void FaceRecognition::draw_result(cv::Mat &src_img, Bbox &bbox, FaceRecognitionInfo &result)
{
	int src_w = src_img.cols;
	int src_h = src_img.rows;
	char text[30];

    cv::Rect rect = bbox_to_osd_rect(bbox, image_size_.width, image_size_.height, src_w, src_h);
    if (rect.width <= 0 || rect.height <= 0) {
        return;
    }

    cv::rectangle(src_img, rect, cv::Scalar(255, 255, 255, 255), 2, 2, 0);
    if (result.id==-1)
	{
        cv::putText(src_img, "unknown", {rect.x, std::max(rect.y - 10, 0)}, cv::FONT_HERSHEY_COMPLEX, 1.0, cv::Scalar(255, 0, 255, 255), 1, 8, 0);
	}
	else
	{
		sprintf(text, "%s:%.2f", result.name.c_str(), result.score);
        cv::putText(src_img, text, {rect.x, std::max(rect.y - 10, 0)}, cv::FONT_HERSHEY_COMPLEX, 1.0, cv::Scalar(255, 255, 0, 255), 1, 8, 0);
	}
}

void FaceRecognition::svd22(const float a[4], float u[4], float s[2], float v[4])
{
	s[0] = (sqrtf(powf(a[0] - a[3], 2) + powf(a[1] + a[2], 2)) + sqrtf(powf(a[0] + a[3], 2) + powf(a[1] - a[2], 2))) / 2;
	s[1] = fabsf(s[0] - sqrtf(powf(a[0] - a[3], 2) + powf(a[1] + a[2], 2)));
	v[2] = (s[0] > s[1]) ? sinf((atan2f(2 * (a[0] * a[1] + a[2] * a[3]), a[0] * a[0] - a[1] * a[1] + a[2] * a[2] - a[3] * a[3])) / 2) : 0;
	v[0] = sqrtf(1 - v[2] * v[2]);
	v[1] = -v[2];
	v[3] = v[0];
	u[0] = (s[0] != 0) ? -(a[0] * v[0] + a[1] * v[2]) / s[0] : 1;
	u[2] = (s[0] != 0) ? -(a[2] * v[0] + a[3] * v[2]) / s[0] : 0;
	u[1] = (s[1] != 0) ? (a[0] * v[1] + a[1] * v[3]) / s[1] : -u[2];
	u[3] = (s[1] != 0) ? (a[2] * v[1] + a[3] * v[3]) / s[1] : u[0];
	v[0] = -v[0];
	v[2] = -v[2];
}

static float umeyama_args_112[] =
	{
#define PIC_SIZE 112
		38.2946 * PIC_SIZE / 112, 51.6963 * PIC_SIZE / 112,
		73.5318 * PIC_SIZE / 112, 51.5014 * PIC_SIZE / 112,
		56.0252 * PIC_SIZE / 112, 71.7366 * PIC_SIZE / 112,
		41.5493 * PIC_SIZE / 112, 92.3655 * PIC_SIZE / 112,
		70.7299 * PIC_SIZE / 112, 92.2041 * PIC_SIZE / 112};

void FaceRecognition::image_umeyama_112(float *src, float *dst)
{
#define SRC_NUM 5
#define SRC_DIM 2
	int i, j, k;
	float src_mean[SRC_DIM] = {0.0};
	float dst_mean[SRC_DIM] = {0.0};
	for (i = 0; i < SRC_NUM * 2; i += 2)
	{
		src_mean[0] += src[i];
		src_mean[1] += src[i + 1];
		dst_mean[0] += umeyama_args_112[i];
		dst_mean[1] += umeyama_args_112[i + 1];
	}
	src_mean[0] /= SRC_NUM;
	src_mean[1] /= SRC_NUM;
	dst_mean[0] /= SRC_NUM;
	dst_mean[1] /= SRC_NUM;

	float src_demean[SRC_NUM][2] = {0.0};
	float dst_demean[SRC_NUM][2] = {0.0};

	for (i = 0; i < SRC_NUM; i++)
	{
		src_demean[i][0] = src[2 * i] - src_mean[0];
		src_demean[i][1] = src[2 * i + 1] - src_mean[1];
		dst_demean[i][0] = umeyama_args_112[2 * i] - dst_mean[0];
		dst_demean[i][1] = umeyama_args_112[2 * i + 1] - dst_mean[1];
	}

	float A[SRC_DIM][SRC_DIM] = {0.0};
	for (i = 0; i < SRC_DIM; i++)
	{
		for (k = 0; k < SRC_DIM; k++)
		{
			for (j = 0; j < SRC_NUM; j++)
			{
				A[i][k] += dst_demean[j][i] * src_demean[j][k];
			}
			A[i][k] /= SRC_NUM;
		}
	}

	float(*T)[SRC_DIM + 1] = (float(*)[SRC_DIM + 1]) dst;
	T[0][0] = 1;
	T[0][1] = 0;
	T[0][2] = 0;
	T[1][0] = 0;
	T[1][1] = 1;
	T[1][2] = 0;
	T[2][0] = 0;
	T[2][1] = 0;
	T[2][2] = 1;

	float U[SRC_DIM][SRC_DIM] = {0};
	float S[SRC_DIM] = {0};
	float V[SRC_DIM][SRC_DIM] = {0};
	svd22(&A[0][0], &U[0][0], S, &V[0][0]);

	T[0][0] = U[0][0] * V[0][0] + U[0][1] * V[1][0];
	T[0][1] = U[0][0] * V[0][1] + U[0][1] * V[1][1];
	T[1][0] = U[1][0] * V[0][0] + U[1][1] * V[1][0];
	T[1][1] = U[1][0] * V[0][1] + U[1][1] * V[1][1];

	float scale = 1.0;
	float src_demean_mean[SRC_DIM] = {0.0};
	float src_demean_var[SRC_DIM] = {0.0};
	for (i = 0; i < SRC_NUM; i++)
	{
		src_demean_mean[0] += src_demean[i][0];
		src_demean_mean[1] += src_demean[i][1];
	}
	src_demean_mean[0] /= SRC_NUM;
	src_demean_mean[1] /= SRC_NUM;

	for (i = 0; i < SRC_NUM; i++)
	{
		src_demean_var[0] += (src_demean_mean[0] - src_demean[i][0]) * (src_demean_mean[0] - src_demean[i][0]);
		src_demean_var[1] += (src_demean_mean[1] - src_demean[i][1]) * (src_demean_mean[1] - src_demean[i][1]);
	}
	src_demean_var[0] /= (SRC_NUM);
	src_demean_var[1] /= (SRC_NUM);
	scale = 1.0 / (src_demean_var[0] + src_demean_var[1]) * (S[0] + S[1]);
	T[0][2] = dst_mean[0] - scale * (T[0][0] * src_mean[0] + T[0][1] * src_mean[1]);
	T[1][2] = dst_mean[1] - scale * (T[1][0] * src_mean[0] + T[1][1] * src_mean[1]);
	T[0][0] *= scale;
	T[0][1] *= scale;
	T[1][0] *= scale;
	T[1][1] *= scale;
	float(*TT)[3] = (float(*)[3])T;
}

void FaceRecognition::get_affine_matrix(float *sparse_points)
{
	float matrix_src[5][2];
	for (uint32_t i = 0; i < 5; ++i)
	{
		matrix_src[i][0] = sparse_points[2 * i + 0];
		matrix_src[i][1] = sparse_points[2 * i + 1];
	}
	image_umeyama_112(&matrix_src[0][0], &matrix_dst_[0]);
}

void FaceRecognition::l2_normalize(const float *src, float *dst, int len) const
{
	float sum = 0;
	for (int i = 0; i < len; ++i)
	{
		sum += src[i] * src[i];
	}
	sum = sqrtf(sum);
    if (sum < 1e-6f)
        sum = 1.0f;
	for (int i = 0; i < len; ++i)
	{
		dst[i] = src[i] / sum;
	}
}

float FaceRecognition::cal_cosine_distance(const float *feature_0, const float *feature_1, int feature_len)
{
	float cosine_distance = 0;
	// calculate the sum square
	for (int i = 0; i < feature_len; ++i)
	{
		float p0 = *(feature_0 + i);
		float p1 = *(feature_1 + i);
		cosine_distance += p0 * p1;
	}
	// cosine distance
	return (0.5 + 0.5 * cosine_distance) * 100;
}
