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
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include "perf_stats.h"
#include "video_pipeline.h"
#include "ai_utils.h"
#include "face_detection.h"
#include "face_recognition.h"

using std::cerr;
using std::cout;
using std::endl;

static constexpr int k_register_preview_hold_ms = 2000;

static void convert_preview_to_osd_argb8888(const cv::Mat &src_preview, cv::Mat &dst_argb)
{
    dst_argb.create(src_preview.rows, src_preview.cols, CV_8UC4);
    for (int y = 0; y < src_preview.rows; ++y) {
        const cv::Vec3b *src_row = src_preview.ptr<cv::Vec3b>(y);
        cv::Vec4b *dst_row = dst_argb.ptr<cv::Vec4b>(y);
        for (int x = 0; x < src_preview.cols; ++x) {
            // OSD 消费 ARGB8888；当前预览图通道语义按板端验证保持为 RGB。
            dst_row[x][0] = 255;
            dst_row[x][1] = src_row[x][0];
            dst_row[x][2] = src_row[x][1];
            dst_row[x][3] = src_row[x][2];
        }
    }
}

static void build_register_preview(const cv::Mat &dump_img, cv::Mat &preview_argb)
{
    cv::Mat oriented_preview;
    if (DISPLAY_ROTATE) {
        cv::rotate(dump_img, oriented_preview, cv::ROTATE_90_CLOCKWISE);
    } else {
        oriented_preview = dump_img;
    }

    const int preview_max_width = std::max(1, OSD_WIDTH / 2);
    const int preview_max_height = std::max(1, OSD_HEIGHT / 2);
    const double scale_x = static_cast<double>(preview_max_width) / oriented_preview.cols;
    const double scale_y = static_cast<double>(preview_max_height) / oriented_preview.rows;
    const double scale = std::min(scale_x, scale_y);
    const int preview_width = std::max(1, static_cast<int>(oriented_preview.cols * scale));
    const int preview_height = std::max(1, static_cast<int>(oriented_preview.rows * scale));

    cv::Mat resized_preview;
    cv::resize(oriented_preview, resized_preview, cv::Size(preview_width, preview_height));
    convert_preview_to_osd_argb8888(resized_preview, preview_argb);
}

static void render_register_preview(cv::Mat &draw_frame, const cv::Mat &preview_argb)
{
    if (preview_argb.empty())
        return;

    const int preview_padding = 16;
    cv::Rect roi(preview_padding, preview_padding, preview_argb.cols, preview_argb.rows);
    preview_argb.copyTo(draw_frame(roi));
}

std::atomic<bool> isp_stop(false);
// 注册人名称
static std::string register_name;
int cur_state = 0;

namespace {

PerfStageStats g_perf_total_time("main.total_time");

volatile sig_atomic_t g_signal_exit_requested = 0;

void handle_exit_signal(int signo)
{
    (void)signo;
    g_signal_exit_requested = 1;
}

bool exit_requested()
{
    return isp_stop.load() || g_signal_exit_requested != 0;
}

bool metrics_log_enabled(int debug_mode)
{
    const char *e = std::getenv("FACE_METRICS");
    return (debug_mode > 0) || (e && e[0] == '1' && e[1] == '\0');
}

bool install_exit_signal_handlers()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_exit_signal;
    sigfillset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, nullptr) != 0) {
        std::cerr << "sigaction(SIGINT) failed: " << strerror(errno) << std::endl;
        return false;
    }

    if (sigaction(SIGTERM, &sa, nullptr) != 0) {
        std::cerr << "sigaction(SIGTERM) failed: " << strerror(errno) << std::endl;
        return false;
    }

    return true;
}

}  // namespace

void print_usage(const char *name)
{
    cout << "Usage: " << name << "<kmodel_det> <det_thres> <nms_thres> <kmodel_recg> <recg_thres> <db_dir> <debug_mode>" << endl
         << "Options:" << endl
         << "  kmodel_det               人脸检测kmodel路径\n"
         << "  det_thres                人脸检测阈值\n"
         << "  nms_thres                人脸检测nms阈值\n"
         << "  kmodel_recg              人脸识别kmodel路径\n"
         << "  recg_thres               人脸识别阈值\n"
         << "  db_dir                   数据库目录\n"
         << "  debug_mode               是否需要调试，0、1、2分别表示不调试、简单调试、详细调试\n"
         << "\n"
         << endl;
}

/**
 * @brief 打印终端操作帮助说明。
 * 
 * 本函数用于向控制台输出当前程序支持的交互命令，便于用户了解操作流程。
 * 提供注册、识别、查询等功能入口。
 */
void print_help() {
    std::cout << "======== 操作说明 ========\n";
    std::cout << "请输入下列命令并按回车以执行对应操作：\n\n";

    std::cout << "  h/help    : 显示帮助说明（即本页内容）\n";
    std::cout << "  i         : 进入注册模式\n";
    std::cout << "              - 系统将自动截图用于人脸注册\n";
    std::cout << "              - 注册后继续输入该用户的姓名并回车完成绑定\n";
    std::cout << "  d         : 清空人脸数据库\n";
    std::cout << "  n         : 显示当前已注册的人脸数量\n";
    std::cout << "  q         : 退出程序并清理资源\n\n";

    std::cout << "注意事项：\n";
    std::cout << "  - 注册截图时请确保画面中仅有一张清晰可见的人脸。\n";
    std::cout << "  - 姓名应使用可识别英文字符，避免特殊符号。\n";

    std::cout << "==========================\n" << std::endl;
}


void video_proc(char *argv[])
{
    int debug_mode = atoi(argv[7]);
    int consecutive_failures = 0;
    bool first_frame_logged = false;
    FrameCHWSize image_size={AI_FRAME_CHANNEL,AI_FRAME_HEIGHT, AI_FRAME_WIDTH};
    // 创建一个空的Mat对象，用于存储绘制的帧
    cv::Mat draw_frame(OSD_HEIGHT, OSD_WIDTH, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    // 创建一个空的runtime_tensor对象，用于存储输入数据
    runtime_tensor input_tensor;
    dims_t in_shape { 1, AI_FRAME_CHANNEL, AI_FRAME_HEIGHT, AI_FRAME_WIDTH };

    // 创建一个PipeLine对象，用于处理视频流
    PipeLine pl(debug_mode);
try {
    // 初始化PipeLine对象
    int ret = pl.Create();
    if (ret != 0) {
        std::cout << "PipeLine create failed: " << ret << std::endl;
        isp_stop = true;
        return;
    }
    std::cerr << "[stage] pl.Create OK" << std::endl;
    std::cerr.flush();
    // 创建一个DumpRes对象，用于存储帧数据
    DumpRes dump_res;
    FaceDetection face_det(argv[1], atof(argv[2]),atof(argv[3]),image_size, debug_mode);
    std::cerr << "[stage] FaceDetection ctor OK" << std::endl;
    std::cerr.flush();

    char* db_dir=argv[6];
    FaceRecognition face_recg(argv[4], atoi(argv[5]), image_size, debug_mode);
    std::cerr << "[stage] FaceRecognition ctor OK" << std::endl;
    std::cerr.flush();
    std::cerr << "[stage] calling database_init(" << db_dir << ") ..." << std::endl;
    std::cerr.flush();
    face_recg.database_init(db_dir);
    std::cerr << "[stage] database_init OK" << std::endl;
    std::cerr.flush();

    vector<FaceDetectionInfo> det_results;
    vector<FaceRecognitionInfo> rec_results;

    int display_state=0;
    std::vector<cv::Mat> sensor_bgr(3);
    cv::Mat dump_img(AI_FRAME_HEIGHT,AI_FRAME_WIDTH , CV_8UC3);
    cv::Mat register_preview_bgra;
    auto register_preview_deadline = std::chrono::steady_clock::time_point::min();

    while(!exit_requested()){
        ScopedPerfStage perf_stage(g_perf_total_time, metrics_log_enabled(debug_mode));
        // 从PipeLine中获取一帧数据，并创建tensor
        ret = pl.GetFrame(dump_res);
        if (ret != 0) {
            if (exit_requested()) {
                break;
            }
            std::cout << "GetFrame failed: " << ret << std::endl;
            if (++consecutive_failures >= 5) {
                std::cout << "Too many consecutive frame failures, stopping pipeline." << std::endl;
                isp_stop = true;
                break;
            }
            usleep(10000);
            continue;
        }
        consecutive_failures = 0;
        if (!first_frame_logged) {
            std::cerr << "[stage] first GetFrame OK: virt=0x" << std::hex << dump_res.virt_addr
                      << " phy=0x" << dump_res.phy_addr << std::dec
                      << " " << dump_res.width << "x" << dump_res.height
                      << " pix=" << dump_res.pixel_format
                      << " size=" << dump_res.mmap_size << std::endl;
        }

        auto input_tensor_r =
            host_runtime_tensor::create(typecode_t::dt_uint8, in_shape,
                                        {(gsl::byte *)dump_res.virt_addr, compute_size(in_shape)},
                                        false, hrt::pool_shared, dump_res.phy_addr);
        if (!input_tensor_r.is_ok()) {
            std::cout << "cannot create input tensor from dumped frame, skip this frame" << std::endl;
            pl.ReleaseFrame(dump_res);
            usleep(10000);
            continue;
        }
        input_tensor = std::move(input_tensor_r.unwrap());
        if (!first_frame_logged) {
            std::cerr << "[stage] first tensor create OK" << std::endl;
        }

        auto sync_r = hrt::sync(input_tensor, sync_op_t::sync_write_back, true);
        if (!sync_r.is_ok()) {
            std::cout << "sync write_back failed on dumped frame, skip this frame" << std::endl;
            pl.ReleaseFrame(dump_res);
            usleep(10000);
            continue;
        }
        if (!first_frame_logged) {
            std::cerr << "[stage] first sync_write_back OK" << std::endl;
        }
        //前处理，推理，后处理
        det_results.clear();
        rec_results.clear();

        const auto now = std::chrono::steady_clock::now();
        const bool preview_active = !register_preview_bgra.empty() && now < register_preview_deadline;

        if(cur_state==-1){
            //不执行任何操作
        }
        else if(cur_state==0){
            // 正常执行人脸识别
            if (!first_frame_logged) {
                std::cerr << "[stage] entering first face_det.pre_process" << std::endl;
            }
            if (!face_det.pre_process(input_tensor)) {
                std::cout << "FaceDetection pre_process failed, skip this frame" << std::endl;
                draw_frame.setTo(cv::Scalar(0, 0, 0, 0));
                pl.InsertFrame(draw_frame.data);
                pl.ReleaseFrame(dump_res);
                usleep(10000);
                continue;
            }
            if (!first_frame_logged) {
                std::cerr << "[stage] first face_det.pre_process OK, calling inference" << std::endl;
            }
            if (!face_det.inference()) {
                std::cout << "FaceDetection inference failed, skip this frame" << std::endl;
                draw_frame.setTo(cv::Scalar(0, 0, 0, 0));
                pl.InsertFrame(draw_frame.data);
                pl.ReleaseFrame(dump_res);
                usleep(10000);
                continue;
            }
            if (!first_frame_logged) {
                std::cerr << "[stage] first face_det full inference OK" << std::endl;
                first_frame_logged = true;
            }
            face_det.post_process(image_size,det_results);
            if (debug_mode > 0 && !det_results.empty()) {
                std::cerr << "[stage] face_det.post_process produced " << det_results.size()
                          << " face(s)" << std::endl;
                std::cerr.flush();
            }
            draw_frame.setTo(cv::Scalar(0, 0, 0, 0));
            for (int i = 0; i < det_results.size(); ++i)
            {
                if (debug_mode > 0) {
                    const auto &bbox = det_results[i].bbox;
                    std::cerr << "[stage] face[" << i << "] enter face_recg.pre_process bbox=("
                              << bbox.x << "," << bbox.y << "," << bbox.w << "," << bbox.h << ")"
                              << std::endl;
                    std::cerr.flush();
                }
                if (!face_recg.pre_process(input_tensor, det_results[i].sparse_kps.points)) {
                    std::cout << "FaceRecognition pre_process failed, skip current face" << std::endl;
                    continue;
                }
                if (debug_mode > 0) {
                    std::cerr << "[stage] face[" << i << "] face_recg.pre_process OK, entering inference"
                              << std::endl;
                    std::cerr.flush();
                }
                if (!face_recg.inference()) {
                    std::cout << "FaceRecognition inference failed, skip current face" << std::endl;
                    continue;
                }
                if (debug_mode > 0) {
                    std::cerr << "[stage] face[" << i << "] face_recg.inference OK, entering database_search"
                              << std::endl;
                    std::cerr.flush();
                }
                FaceRecognitionInfo recg_result;
                face_recg.database_search(recg_result);
                if (debug_mode > 0) {
                    std::cerr << "[stage] face[" << i << "] database_search OK, name="
                              << recg_result.name << " score=" << recg_result.score << std::endl;
                    std::cerr.flush();
                }
                rec_results.push_back(recg_result);
            }
        }
        else if (cur_state==1){
            // 查询当前注册人数
            int reg_num = face_recg.database_count(db_dir);
            std::cout<<"当前注册人数：" + std::to_string(reg_num) + "人。"<<std::endl;
            cur_state=0;
            display_state=0;
        }
        else if (cur_state==2){
            // dump一帧图片，作为待注册图片供用户查看
            draw_frame.setTo(cv::Scalar(0, 0, 0, 0));
            dump_img.setTo(cv::Scalar(0, 0, 0));
            sensor_bgr.clear();
            uint8_t* data = reinterpret_cast<uint8_t*>(dump_res.virt_addr);
            cv::Mat ori_img_R(AI_FRAME_HEIGHT, AI_FRAME_WIDTH, CV_8UC1, data);
            cv::Mat ori_img_G(AI_FRAME_HEIGHT, AI_FRAME_WIDTH, CV_8UC1, data + AI_FRAME_HEIGHT * AI_FRAME_WIDTH);
            cv::Mat ori_img_B(AI_FRAME_HEIGHT, AI_FRAME_WIDTH, CV_8UC1, data + 2 * AI_FRAME_HEIGHT * AI_FRAME_WIDTH);
            if (ori_img_B.empty() || ori_img_G.empty() || ori_img_R.empty()) {
                std::cout << "One or more of the channel images is empty." << std::endl;
                continue;
            }
            sensor_bgr.push_back(ori_img_B);
            sensor_bgr.push_back(ori_img_G);
            sensor_bgr.push_back(ori_img_R);
            cv::merge(sensor_bgr, dump_img);

            cur_state=0;
            display_state=2;
        }
        else if (cur_state==3){
            // 将dump的图片进行人脸注册，当只有一张人脸时注册成功，否则注册失败
            FrameCHWSize reg_size={dump_img.channels(),dump_img.rows,dump_img.cols};
            // 创建一个空的向量，用于存储chw图像数据,将读入的hwc数据转换成chw数据
            std::vector<uint8_t> chw_vec;
            std::vector<cv::Mat> bgrChannels(3);
            cv::split(dump_img, bgrChannels);
            for (auto i = 2; i > -1; i--)
            {
                std::vector<uint8_t> data = std::vector<uint8_t>(bgrChannels[i].reshape(1, 1));
                chw_vec.insert(chw_vec.end(), data.begin(), data.end());
            }
            // 创建tensor
            dims_t in_shape { 1, 3, dump_img.rows, dump_img.cols };
            auto reg_tensor_r = host_runtime_tensor::create(typecode_t::dt_uint8, in_shape, hrt::pool_shared);
            if (!reg_tensor_r.is_ok()) {
                std::cout << "cannot create register tensor, skip register request" << std::endl;
                cur_state=0;
                display_state=0;
                pl.ReleaseFrame(dump_res);
                usleep(10000);
                continue;
            }
            runtime_tensor reg_tensor = std::move(reg_tensor_r.unwrap());
            auto to_host_r = reg_tensor.impl()->to_host();
            if (!to_host_r.is_ok()) {
                std::cout << "register tensor to_host failed, skip register request" << std::endl;
                cur_state=0;
                display_state=0;
                pl.ReleaseFrame(dump_res);
                usleep(10000);
                continue;
            }
            nncase::tensor host_tensor = std::move(to_host_r.unwrap());
            auto as_host_r = host_tensor->buffer().as_host();
            if (!as_host_r.is_ok()) {
                std::cout << "register tensor as_host failed, skip register request" << std::endl;
                cur_state=0;
                display_state=0;
                pl.ReleaseFrame(dump_res);
                usleep(10000);
                continue;
            }
            auto as_host = std::move(as_host_r.unwrap());
            auto map_r = as_host.map(map_access_::map_write);
            if (!map_r.is_ok()) {
                std::cout << "register tensor map failed, skip register request" << std::endl;
                cur_state=0;
                display_state=0;
                pl.ReleaseFrame(dump_res);
                usleep(10000);
                continue;
            }
            auto mapped = std::move(map_r.unwrap());
            auto ref_buf = mapped.buffer();
            memcpy(reinterpret_cast<char *>(ref_buf.data()), chw_vec.data(), chw_vec.size());
            auto reg_sync_r = hrt::sync(reg_tensor, sync_op_t::sync_write_back, true);
            if (!reg_sync_r.is_ok()) {
                std::cout << "register tensor sync failed, skip register request" << std::endl;
                cur_state=0;
                display_state=0;
                pl.ReleaseFrame(dump_res);
                usleep(10000);
                continue;
            }
            if (!face_det.pre_process(reg_tensor) || !face_det.inference()) {
                std::cout << "FaceDetection register path failed, skip register request" << std::endl;
                cur_state=0;
                display_state=0;
                pl.ReleaseFrame(dump_res);
                usleep(10000);
                continue;
            }
            face_det.post_process(reg_size,det_results);
            if(det_results.size()==1){
                if (!face_recg.pre_process(reg_tensor, det_results[0].sparse_kps.points) ||
                    !face_recg.inference()) {
                    std::cout << "FaceRecognition register path failed, skip register request" << std::endl;
                    cur_state=0;
                    display_state=0;
                    pl.ReleaseFrame(dump_res);
                    usleep(10000);
                    continue;
                }
                face_recg.database_add(register_name, db_dir, dump_img);
                std::cout<<"注册成功！"<<std::endl;
            }
            else{
                std::cout<<"注册图片中需要满足仅有一张人脸，请重新注册！"<<std::endl;
            }
            cur_state=0;
            display_state=0;
        }
        else if (cur_state==4){
            // 清空人脸数据库
            face_recg.database_reset(db_dir);
            std::cout<<"人脸数据库已清空！"<<std::endl;
            cur_state=0;
            display_state=0;
        }
        
        if(display_state==2){
            build_register_preview(dump_img, register_preview_bgra);
            register_preview_deadline = now + std::chrono::milliseconds(k_register_preview_hold_ms);
            render_register_preview(draw_frame, register_preview_bgra);
        }
        else if(preview_active){
            render_register_preview(draw_frame, register_preview_bgra);
        }
        else if(!register_preview_bgra.empty()){
            register_preview_bgra.release();
        }
        else{
            for (size_t i = 0; i < rec_results.size(); i++) {
                face_recg.draw_result(draw_frame, det_results[i].bbox, rec_results[i]);
            }
        }
        
        // 将绘制的帧插入到PipeLine中
        if (pl.InsertFrame(draw_frame.data) != 0) {
            std::cout << "InsertFrame failed, skip current OSD update" << std::endl;
        }
        // 释放帧数据
        pl.ReleaseFrame(dump_res);
    }
    pl.Destroy();
} catch (const std::exception &e) {
    std::cerr << "video_proc std::exception: " << e.what() << std::endl;
    std::cerr.flush();
    isp_stop = true;
    pl.Destroy();
} catch (...) {
    std::cerr << "video_proc unknown exception (not std::exception)" << std::endl;
    std::cerr.flush();
    isp_stop = true;
    pl.Destroy();
}
}

int main(int argc, char *argv[])
{
    std::cout << "case " << argv[0] << " built at " << __DATE__ << " " << __TIME__ << std::endl;
    std::cout << "Press 'q+Enter' or Ctrl+C to exit." << std::endl;
    if (argc != 8)
    {
        print_usage(argv[0]);
        return -1;
    }

    if (!install_exit_signal_handlers())
        return -1;

    std::thread thread_isp(video_proc, argv);
    // 命令行输入处理主循环
    bool awaiting_register_name = false;
    sleep(2);
    // 输入提示信息
    std::cout << "输入 'h' 或 'help' 并回车 查看命令说明" << std::endl;
    while(!exit_requested()){
        struct pollfd stdin_pollfd;
        memset(&stdin_pollfd, 0, sizeof(stdin_pollfd));
        stdin_pollfd.fd = STDIN_FILENO;
        stdin_pollfd.events = POLLIN | POLLHUP | POLLERR;

        int poll_ret = poll(&stdin_pollfd, 1, 200);
        if (poll_ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "poll(stdin) failed: " << strerror(errno) << std::endl;
            break;
        }
        if (poll_ret == 0) {
            continue;
        }

        std::string input;
        if (!std::getline(std::cin, input)) {
            std::cout << "stdin closed, exiting." << std::endl;
            break;
        }

        if (input == "h" || input == "help") {
            print_help();  // 打印帮助信息
        }
        else if (input == "i") {
            // 进入注册模式
            cur_state = 2; 
            awaiting_register_name = true;
        }
        else if (input == "d") {
            // 清空人脸数据库
            cur_state = 4;      
            awaiting_register_name = false;
        }
        else if (input == "n") {
            // 查询已注册人数
            cur_state = 1;
            awaiting_register_name = false;
        }
        else if (input == "q") {
            usleep(100000);     
            awaiting_register_name = false;
            isp_stop.store(true);      
            break;
        }
        else if (awaiting_register_name) {
            // 进入注册模式后，下一行非空输入作为姓名提交。
            if (input.empty()) {
                std::cout << "请输入姓名后再回车，或输入其他命令取消当前注册。" << std::endl;
                continue;
            }
            register_name = input;
            cur_state = 3;   // 注册确认阶段
            awaiting_register_name = false;
        }
        else if (!input.empty()) {
            // 未进入注册模式却输入姓名
            std::cout << "请先输入 'i' 并回车以进入注册模式！" << std::endl;
        }
    }

    if (g_signal_exit_requested != 0) {
        std::cout << "Signal received, shutting down gracefully..." << std::endl;
    }

    isp_stop.store(true);
    thread_isp.join();
    return 0;
}
