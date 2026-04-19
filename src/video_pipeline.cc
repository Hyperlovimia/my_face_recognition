#include "video_pipeline.h"

#include <opencv2/imgproc.hpp>

namespace {

constexpr k_u32 kOsdInsertChnOffset = 3;

bool FindCompatibleVbPool(const k_vb_config &vb_config, k_u64 required_size, int *matched_index,
                          k_u64 *matched_size, k_u32 *matched_count)
{
    int best_index = -1;
    k_u64 best_size = 0;
    k_u32 best_count = 0;

    for (int i = 0; i < VB_MAX_COMM_POOLS; ++i) {
        const k_vb_pool_config &pool = vb_config.comm_pool[i];
        if (pool.blk_cnt == 0 || pool.blk_size < required_size) {
            continue;
        }
        if (best_index < 0 || pool.blk_size < best_size) {
            best_index = i;
            best_size = pool.blk_size;
            best_count = pool.blk_cnt;
        }
    }

    if (best_index < 0) {
        return false;
    }

    if (matched_index != nullptr) {
        *matched_index = best_index;
    }
    if (matched_size != nullptr) {
        *matched_size = best_size;
    }
    if (matched_count != nullptr) {
        *matched_count = best_count;
    }
    return true;
}

void PrintVbConfigSummary(const k_vb_config &vb_config)
{
    printf("Current VB config summary:\n");
    for (int i = 0; i < VB_MAX_COMM_POOLS; ++i) {
        const k_vb_pool_config &pool = vb_config.comm_pool[i];
        if (pool.blk_cnt == 0) {
            continue;
        }
        printf("  comm_pool[%d]: blk_cnt=%u blk_size=%lu mode=%u\n",
               i, pool.blk_cnt, (unsigned long)pool.blk_size, pool.mode);
    }
}

}  // namespace

PipeLine::PipeLine(int debug_mode)
{
    //配置屏幕类型
    if(DISPLAY_MODE == DISPLAY_MODE_LT9611){
        connector_type = LT9611_MIPI_4LAN_1920X1080_30FPS;
    }
    else if(DISPLAY_MODE == DISPLAY_MODE_ST7701){
        connector_type = ST7701_V1_MIPI_2LAN_480X800_30FPS;
    }
    else if(DISPLAY_MODE == DISPLAY_MODE_HX8377){
        connector_type = HX8377_V2_MIPI_4LAN_1080X1920_30FPS;
    }
    else if(DISPLAY_MODE == DISPLAY_MODE_NT35516){
        connector_type = NT35516_MIPI_2LAN_540X960_30FPS;
    }
    else{
        // 默认回退为 1080P HDMI 输出
        connector_type = LT9611_MIPI_4LAN_1920X1080_30FPS;
    }


    // ------------------------ VO（视频输出）相关 ID ------------------------
    vo_dev_id = K_VO_DISPLAY_DEV_ID;        // VO 设备 ID
    vi_vo_id  = K_VO_LAYER1;                // 用于显示摄像头视频的 VO layer
    osd_vo_id = K_VO_OSD3;                  // 用于叠加 OSD 的 VO layer


    // ------------------------ Sensor / VICAP 默认配置 ------------------------
    // 默认使用 GC2093，start() 中会根据探测结果自动适配
    sensor_type = GC2093_MIPI_CSI2_1920X1080_30FPS_10BIT_LINEAR;
    // VICAP 设备 ID
    vicap_dev = VICAP_DEV_ID_0;
    // VICAP → VO 通道（视频直通显示）
    vicap_chn_to_vo = VICAP_CHN_ID_0;
    // VICAP → AI 通道（用于算法推理）
    vicap_chn_to_ai = VICAP_CHN_ID_1;

    // 调试模式开关
    debug_mode_ = debug_mode;

    // OSD 所使用的 VB 内存池，初始化为无效
    osd_pool_id = VB_INVALID_POOLID;
    handle = VB_INVALID_HANDLE;
    insert_osd_vaddr = nullptr;
}

PipeLine::~PipeLine()
{
}

int PipeLine::Create()
{
    ScopedTiming st("PipeLine::Create", debug_mode_);
    k_s32 ret = 0;
    bool vb_preinitialized = false;
    const k_u64 required_yuv_blk_size =
        VICAP_ALIGN_UP((DISPLAY_WIDTH * DISPLAY_HEIGHT * 3 / 2), VICAP_ALIGN_1K);
    const k_u64 required_ai_blk_size =
        VICAP_ALIGN_UP((AI_FRAME_WIDTH * AI_FRAME_HEIGHT * AI_FRAME_CHANNEL), VICAP_ALIGN_1K);
    const k_u32 vo_layer_width = DISPLAY_ROTATE ? DISPLAY_HEIGHT : DISPLAY_WIDTH;
    const k_u32 vo_layer_height = DISPLAY_ROTATE ? DISPLAY_WIDTH : DISPLAY_HEIGHT;

    // =============================================================================================
    // 1. 配置 Video Buffer（VB）系统
    // =============================================================================================
    memset(&config, 0, sizeof(k_vb_config));
    config.max_pool_cnt = 64;  // 最多支持 64 个内存池
    config.comm_pool[0].blk_cnt = 5;
    config.comm_pool[0].mode = VB_REMAP_MODE_NOCACHE;
    config.comm_pool[0].blk_size = required_yuv_blk_size;
    config.comm_pool[1].blk_cnt = 5;
    config.comm_pool[1].mode = VB_REMAP_MODE_NOCACHE;
    config.comm_pool[1].blk_size = required_ai_blk_size;
    yuv_buffer_size_ = required_yuv_blk_size;
    ai_buffer_size_ = required_ai_blk_size;

    ret = kd_mpi_vicap_set_mclk(VICAP_MCLK0, VICAP_PLL0_CLK_DIV4, 16, 1);
    if (ret) {
        printf("kd_mpi_vicap_set_mclk failed ret:%d\n", ret);
        return ret;
    }

    // 设置 VB 全局配置
    ret = kd_mpi_vb_set_config(&config);
    if (ret == K_ERR_VB_BUSY) {
        printf("VB is already initialized by system/another app, reuse existing VB configuration.\n");
        k_vb_config current_vb_config;
        memset(&current_vb_config, 0, sizeof(current_vb_config));
        ret = kd_mpi_vb_get_config(&current_vb_config);
        if (ret) {
            printf("kd_mpi_vb_get_config failed ret:%d\n", ret);
            return ret;
        }

        int yuv_pool_index = -1;
        int ai_pool_index = -1;
        k_u32 yuv_pool_cnt = 0;
        k_u32 ai_pool_cnt = 0;
        if (!FindCompatibleVbPool(current_vb_config, required_yuv_blk_size, &yuv_pool_index,
                                  &yuv_buffer_size_, &yuv_pool_cnt)) {
            printf("Existing VB has no common pool large enough for VO YUV output. required=%lu\n",
                   (unsigned long)required_yuv_blk_size);
            PrintVbConfigSummary(current_vb_config);
            return K_ERR_VB_BUSY;
        }
        if (!FindCompatibleVbPool(current_vb_config, required_ai_blk_size, &ai_pool_index,
                                  &ai_buffer_size_, &ai_pool_cnt)) {
            printf("Existing VB has no common pool large enough for AI output. required=%lu\n",
                   (unsigned long)required_ai_blk_size);
            PrintVbConfigSummary(current_vb_config);
            return K_ERR_VB_BUSY;
        }

        printf("Reuse VB comm_pool[%d] for VO YUV: blk_size=%lu blk_cnt=%u\n",
               yuv_pool_index, (unsigned long)yuv_buffer_size_, yuv_pool_cnt);
        printf("Reuse VB comm_pool[%d] for AI BGR: blk_size=%lu blk_cnt=%u\n",
               ai_pool_index, (unsigned long)ai_buffer_size_, ai_pool_cnt);
        vb_preinitialized = true;
        ret = K_SUCCESS;
    }
    if (ret) {
        printf("vb_set_config failed ret:%d\n", ret);
        return ret;
    }

    if (!vb_preinitialized) {
        // 设置 VB 附加配置（JPEG、ISP 统计等）
        k_vb_supplement_config supplement_config;
        memset(&supplement_config, 0, sizeof(supplement_config));
        supplement_config.supplement_config |= VB_SUPPLEMENT_JPEG_MASK;
        ret = kd_mpi_vb_set_supplement_config(&supplement_config);
        if (ret) {
            printf("vb_set_supplement_config failed ret:%d\n", ret);
            return ret;
        }

        // 初始化 VB 子系统
        ret = kd_mpi_vb_init();
        if (ret) {
            printf("vb_init failed ret:%d\n", ret);
            return ret;
        }

        vb_inited_by_pipeline_ = true;
    }

    // ---------------------------------------------------------------------------------------------
    // 预分配 AI 输入私有缓存（CPU-cached + 物理连续）
    // 参考 ai_poc/face_detection/main.cc:66-69。每帧 dump 到这块常驻缓存，
    // AI tensor 始终消费它，与 VICAP dump 的生命周期解耦。
    // ---------------------------------------------------------------------------------------------
    ai_buf_size_ = (size_t)AI_FRAME_CHANNEL * AI_FRAME_HEIGHT * AI_FRAME_WIDTH;
    ret = kd_mpi_sys_mmz_alloc_cached(&ai_buf_paddr_, &ai_buf_vaddr_,
                                      "ai_input", "anonymous", (k_u32)ai_buf_size_);
    if (ret) {
        printf("kd_mpi_sys_mmz_alloc_cached failed for AI input buffer: ret=%d size=%zu\n",
               ret, ai_buf_size_);
        ai_buf_vaddr_ = nullptr;
        ai_buf_paddr_ = 0;
        ai_buf_size_ = 0;
        return ret;
    }
    printf("AI input private buffer: paddr=%lx vaddr=%p size=%zu\n",
           (unsigned long)ai_buf_paddr_, ai_buf_vaddr_, ai_buf_size_);

    // YUV420SP → RGB HWC 中间缓冲。当前板端实测更接近 NV21/VU 排布，GetFrame 会在 CPU 侧
    // 先 cvtColor 到这块 H×W×3 的 HWC 图像，再按 R/G/B plane 写入 ai_buf。
    bgr_hwc_scratch_.create(AI_FRAME_HEIGHT, AI_FRAME_WIDTH, CV_8UC3);
    if (bgr_hwc_scratch_.empty()) {
        printf("bgr_hwc_scratch_ create failed\n");
        return -1;
    }

    // =============================================================================================
    // 2. 屏幕（Connector）配置
    // =============================================================================================
    k_connector_info connector_info;
    memset(&connector_info, 0, sizeof(k_connector_info));

    // 根据 connector 类型获取硬件参数
    ret = kd_mpi_get_connector_info(connector_type, &connector_info);
    if (ret) {
        printf("the connector type not supported!\n");
        return ret;
    }

    // 打开 connector 设备
    k_s32 connector_fd = kd_mpi_connector_open(connector_info.connector_name);
    if (connector_fd < 0) {
        printf("%s, connector open failed. connector_type=%d, name=%s.\n",
               __func__, connector_type, connector_info.connector_name);
        printf("Check whether the panel driver for this connector is enabled in firmware.\n");
        return K_ERR_VO_NOTREADY;
    }

    // 打开电源
    ret = kd_mpi_connector_power_set(connector_fd, K_TRUE);
    if (ret) {
        printf("ERROR: kd_mpi_connector_power_set failed, ret=%d\n", ret);
        close(connector_fd);
        return ret;
    }

    // 初始化 connector（配置时序、分辨率等）
    ret = kd_mpi_connector_init(connector_fd, connector_info);
    if (ret) {
        printf("ERROR: kd_mpi_connector_init failed, ret=%d\n", ret);
        close(connector_fd);
        return ret;
    }

    // SDK 当前 kd_mpi_connector_close() 缺少 return，直接读取其返回值会产生未定义行为。
    // 这里沿用 test_vi_vo 的做法：初始化完成后继续持有该 fd，直到进程退出/销毁。
    connector_fd_ = connector_fd;

    // =============================================================================================
    // 3. 配置 VO（视频输出层：用于显示摄像头画面）
    // =============================================================================================
    kd_mpi_vo_disable_video_layer(vi_vo_id);  // 先关闭 layer，避免旧配置干扰

    memset(&vi_vo_attr, 0, sizeof(vi_vo_attr));
    vi_vo_attr.display_rect.x  = 0;
    vi_vo_attr.display_rect.y  = 0;
    vi_vo_attr.img_size.width  = vo_layer_width;
    vi_vo_attr.img_size.height = vo_layer_height;
    // 你的板上已实测 test_demo/test_vi_vo 可以跑通，因此这里以它为第一参考。
    vi_vo_attr.pixel_format    = PIXEL_FORMAT_YVU_PLANAR_420;
    vi_vo_attr.stride          = (vo_layer_width / 8 - 1) + ((vo_layer_height - 1) << 16);
    vi_vo_attr.func            = DISPLAY_ROTATE ? K_ROTATION_90 : K_ROTATION_0;

    ret = kd_mpi_vo_set_video_layer_attr(vi_vo_id, &vi_vo_attr);
    if (ret != K_SUCCESS) {
        printf("ERROR: kd_mpi_vo_set_video_layer_attr failed, ret=%d\n", ret);
        return ret;
    }

    ret = kd_mpi_vo_enable_video_layer(vi_vo_id);
    if (ret != K_SUCCESS) {
        printf("ERROR: kd_mpi_vo_enable_video_layer failed, ret=%d\n", ret);
        return ret;
    }
    vo_video_enabled_ = true;

    printf("VICAP to VO: layer=%d configured for %ux%u YVU420P, rotate90=%d\n",
           vi_vo_id, vo_layer_width, vo_layer_height, DISPLAY_ROTATE ? 1 : 0);

    // =============================================================================================
    // 4. 配置 OSD 层（ARGB8888 叠加图层）
    // =============================================================================================
    if(USE_OSD == 1){
        kd_mpi_vo_osd_disable(osd_vo_id);

        memset(&osd_vo_attr, 0, sizeof(osd_vo_attr));
        osd_vo_attr.display_rect.x  = 0;
        osd_vo_attr.display_rect.y  = 0;
        osd_vo_attr.img_size.width  = OSD_WIDTH;
        osd_vo_attr.img_size.height = OSD_HEIGHT;
        osd_vo_attr.pixel_format    = PIXEL_FORMAT_ARGB_8888;
        osd_vo_attr.stride          = OSD_WIDTH * 4 / 8;
        osd_vo_attr.global_alptha   = 0xFF;

        ret = kd_mpi_vo_set_video_osd_attr(osd_vo_id, &osd_vo_attr);
        if (ret != K_SUCCESS) {
            printf("ERROR: kd_mpi_vo_set_video_osd_attr failed, ret=%d\n", ret);
            return ret;
        }

        ret = kd_mpi_vo_osd_enable(osd_vo_id);
        if (ret != K_SUCCESS) {
            printf("ERROR: kd_mpi_vo_osd_enable failed, ret=%d\n", ret);
            return ret;
        }
        vo_osd_enabled_ = true;

        printf("OSD to VO: layer=%d configured for %ux%u ARGB8888\n",
               osd_vo_id, OSD_WIDTH, OSD_HEIGHT);

        k_vb_pool_config pool_config;
        memset(&pool_config, 0, sizeof(pool_config));
        pool_config.blk_cnt = 4;
        pool_config.blk_size =
            VICAP_ALIGN_UP((OSD_WIDTH * OSD_HEIGHT * OSD_CHANNEL * 2), VICAP_ALIGN_1K);
        pool_config.mode = VB_REMAP_MODE_NOCACHE;
        osd_pool_id = kd_mpi_vb_create_pool(&pool_config);
        if (osd_pool_id == VB_INVALID_POOLID) {
            printf("kd_mpi_vb_create_pool for OSD failed.\n");
            return -1;
        }

        // --------------------- 从 OSD VB 池获取一块缓存，用于写入叠加数据 ---------------------
        osd_frame_size_ = OSD_WIDTH * OSD_HEIGHT * OSD_CHANNEL;
        osd_mmap_size_ = osd_frame_size_ + 4096;

        // 从指定内存池中申请一块缓存
        handle = kd_mpi_vb_get_block(osd_pool_id, osd_mmap_size_, NULL);
        if (handle == VB_INVALID_HANDLE)
        {
            printf("%s get vb block error\n", __func__);
            return -1;
        }
        osd_block_acquired_ = true;

        // 获取该缓存块的物理地址
        k_u64 phys_addr = kd_mpi_vb_handle_to_phyaddr(handle);
        if (phys_addr == 0)
        {
            printf("%s get phys addr error\n", __func__);
            return -1;
        }

        // 映射为用户态虚拟地址（非 cache）
        k_u32* virt_addr = (k_u32 *)kd_mpi_sys_mmap(phys_addr, osd_mmap_size_);
        if (virt_addr == NULL)
        {
            printf("%s mmap error\n", __func__);
            return -1;
        }

        // 初始化 OSD 帧描述结构
        memset(&osd_frame_info, 0, sizeof(osd_frame_info));
        osd_frame_info.v_frame.width        = OSD_WIDTH;
        osd_frame_info.v_frame.height       = OSD_HEIGHT;
        osd_frame_info.v_frame.stride[0]    = OSD_WIDTH;
        osd_frame_info.v_frame.pixel_format = PIXEL_FORMAT_ARGB_8888;
        osd_frame_info.mod_id               = K_ID_VO;
        osd_frame_info.pool_id              = osd_pool_id;
        osd_frame_info.v_frame.phys_addr[0] = phys_addr;

        // 保存虚拟地址，用于后续 memcpy 写入 OSD 数据
        insert_osd_vaddr = virt_addr;
        printf("phys_addr is %lx g_pool_id is %d \n", phys_addr, osd_pool_id);
    }


    // =============================================================================================
    // 5. 传感器探测 & VICAP 设备配置
    // =============================================================================================
    k_vicap_sensor_info sensor_info;
    memset(&sensor_info, 0, sizeof(k_vicap_sensor_info));
    ret = kd_mpi_vicap_get_sensor_info(sensor_type, &sensor_info);
    if (ret) {
        printf("vicap, the sensor type not supported!\n");
        return ret;
    }

    // 配置 VICAP 设备属性（采集窗口、工作模式、ISP 功能等）
    k_vicap_dev_attr dev_attr;
    memset(&dev_attr, 0, sizeof(k_vicap_dev_attr));
    dev_attr.acq_win.h_start = 0;
    dev_attr.acq_win.v_start = 0;
    dev_attr.acq_win.width   = ISP_WIDTH;
    dev_attr.acq_win.height  = ISP_HEIGHT;
    dev_attr.mode            = VICAP_WORK_ONLINE_MODE;  // 在线模式
    dev_attr.pipe_ctrl.data  = 0xFFFFFFFF;
    dev_attr.pipe_ctrl.bits.af_enable   = 0;
    dev_attr.pipe_ctrl.bits.ahdr_enable = 0;
    dev_attr.pipe_ctrl.bits.dnr3_enable = 0;
    dev_attr.cpature_frame   = 0;
    dev_attr.sensor_info     = sensor_info;

    ret = kd_mpi_vicap_set_dev_attr(vicap_dev, dev_attr);
    if (ret) {
        printf("vicap, kd_mpi_vicap_set_dev_attr failed.\n");
        return ret;
    }

    // =============================================================================================
    // 6. VICAP 通道 0：输出到 VO 显示
    // =============================================================================================
    k_vicap_chn_attr chn0_attr;
    memset(&chn0_attr, 0, sizeof(k_vicap_chn_attr));
    chn0_attr.out_win.h_start = 0;
    chn0_attr.out_win.v_start = 0;
    chn0_attr.out_win.width  = DISPLAY_WIDTH;
    chn0_attr.out_win.height = DISPLAY_HEIGHT;
    chn0_attr.crop_win       = dev_attr.acq_win;
    chn0_attr.scale_win      = chn0_attr.out_win;
    chn0_attr.crop_enable    = K_FALSE;
    chn0_attr.scale_enable   = K_FALSE;
    chn0_attr.chn_enable     = K_TRUE;
    chn0_attr.pix_format     = PIXEL_FORMAT_YVU_PLANAR_420;
    chn0_attr.buffer_num     = VICAP_MAX_FRAME_COUNT;
    chn0_attr.buffer_size    = yuv_buffer_size_;
    chn0_attr.alignment      = 0;
    chn0_attr.fps            = 0;

    printf("vicap ...kd_mpi_vicap_set_chn_attr, buffer_size[%d]\n", chn0_attr.buffer_size);
    ret = kd_mpi_vicap_set_chn_attr(vicap_dev, vicap_chn_to_vo, chn0_attr);
    if (ret) {
        printf("vicap, kd_mpi_vicap_set_chn_attr failed.\n");
        return ret;
    }

    // 绑定 VICAP → VO（视频直通显示）
    vicap_mpp_chn.mod_id = K_ID_VI;
    vicap_mpp_chn.dev_id = vicap_dev;
    vicap_mpp_chn.chn_id = vicap_chn_to_vo;
    vo_mpp_chn.mod_id    = K_ID_VO;
    vo_mpp_chn.dev_id    = vo_dev_id;
    vo_mpp_chn.chn_id    = K_VO_DISPLAY_CHN_ID1;

    // 参考工程默认假设系统处于干净状态；实际反复运行时，若上次异常退出，
    // 这里可能残留同一条 VI->VO 绑定。先做一次幂等解绑，避免 bind 命中 NOT_PERM。
    ret = kd_mpi_sys_unbind(&vicap_mpp_chn, &vo_mpp_chn);
    if (ret == K_SUCCESS) {
        printf("Cleared stale VICAP->VO bind before rebinding.\n");
    } else if (ret != K_ERR_SYS_UNEXIST) {
        printf("kd_mpi_sys_unbind before bind returned:0x%x\n", ret);
    }

    ret = kd_mpi_sys_bind(&vicap_mpp_chn, &vo_mpp_chn);
    if (ret == K_ERR_SYS_NOT_PERM) {
        // 某些固件版本在目标端已有绑定时返回 NOT_PERM，并打印 "dest have bind src"。
        // 再执行一次解绑后重绑，兼容板端遗留状态。
        printf("kd_mpi_sys_bind reported existing destination bind, retrying after unbind.\n");
        ret = kd_mpi_sys_unbind(&vicap_mpp_chn, &vo_mpp_chn);
        if (ret != K_SUCCESS && ret != K_ERR_SYS_UNEXIST) {
            printf("kd_mpi_sys_unbind retry failed:0x%x\n", ret);
            return ret;
        }
        ret = kd_mpi_sys_bind(&vicap_mpp_chn, &vo_mpp_chn);
    }

    if (ret) {
        printf("kd_mpi_sys_bind failed:0x%x\n", ret);
        return ret;
    } else {
        vicap_vo_bound_ = true;
    }

    // =============================================================================================
    // 7. VICAP 通道 1：输出给 AI 使用（RGB Planar）
    // =============================================================================================
    k_vicap_chn_attr chn1_attr;
    memset(&chn1_attr, 0, sizeof(k_vicap_chn_attr));
    chn1_attr.out_win.h_start = 0;
    chn1_attr.out_win.v_start = 0;
    chn1_attr.out_win.width  = AI_FRAME_WIDTH;
    chn1_attr.out_win.height = AI_FRAME_HEIGHT;
    chn1_attr.crop_win       = dev_attr.acq_win;
    chn1_attr.scale_win      = chn1_attr.out_win;
    chn1_attr.crop_enable    = K_FALSE;
    chn1_attr.scale_enable   = K_FALSE;
    chn1_attr.chn_enable     = K_TRUE;
    chn1_attr.pix_format     = PIXEL_FORMAT_BGR_888_PLANAR;
    chn1_attr.buffer_num     = VICAP_MAX_FRAME_COUNT;
    chn1_attr.buffer_size    = ai_buffer_size_;
    chn1_attr.alignment      = 0;
    chn1_attr.fps            = 0;

    printf("kd_mpi_vicap_set_chn_attr, buffer_size[%d]\n", chn1_attr.buffer_size);
    ret = kd_mpi_vicap_set_chn_attr(vicap_dev, vicap_chn_to_ai, chn1_attr);
    if (ret) {
        printf("kd_mpi_vicap_set_chn_attr failed.\n");
        return ret;
    }

    // 设置数据库解析模式（XML/JSON）
    ret = kd_mpi_vicap_set_database_parse_mode(vicap_dev, VICAP_DATABASE_PARSE_XML_JSON);
    if (ret) {
        printf("kd_mpi_vicap_set_database_parse_mode failed.\n");
        return ret;
    }

    // 初始化 VICAP
    printf("kd_mpi_vicap_init\n");
    ret = kd_mpi_vicap_init(vicap_dev);
    if (ret) {
        printf("kd_mpi_vicap_init failed.\n");
        return ret;
    }
    vicap_inited_ = true;

    // 启动数据流
    printf("kd_mpi_vicap_start_stream\n");
    ret = kd_mpi_vicap_start_stream(vicap_dev);
    if (ret) {
        printf("kd_mpi_vicap_start_stream failed.\n");
        return ret;
    }
    vicap_stream_started_ = true;

    return ret;
}

int PipeLine::GetFrame(DumpRes &dump_res){
    ScopedTiming st("PipeLine::GetFrame", debug_mode_);
    int ret = 0;
    memset(&dump_res, 0, sizeof(dump_res));
    memset(&dump_info, 0, sizeof(k_video_frame_info));

    // 从 VICAP dump 一帧（阻塞最多 1000ms）
    ret = kd_mpi_vicap_dump_frame(vicap_dev, VICAP_CHN_ID_1, VICAP_DUMP_YUV, &dump_info, 1000);
    if (ret)
    {
        printf("kd_mpi_vicap_dump_frame failed.\n");
        return ret;
    }

    if (dump_info.v_frame.phys_addr[0] == 0) {
        printf("kd_mpi_vicap_dump_frame returned invalid phys addr.\n");
        kd_mpi_vicap_dump_release(vicap_dev, VICAP_CHN_ID_1, &dump_info);
        memset(&dump_info, 0, sizeof(k_video_frame_info));
        return -1;
    }

    const k_u32 pix_format = dump_info.v_frame.pixel_format;
    const k_u32 f_width = dump_info.v_frame.width;
    const k_u32 f_height = dump_info.v_frame.height;
    const k_u32 f_stride0 = dump_info.v_frame.stride[0];
    const k_u32 f_stride1 = dump_info.v_frame.stride[1];

    if (!dump_frame_info_logged_) {
        printf("First VICAP dump: width=%u height=%u pixel_format=%d stride0=%u stride1=%u phys0=%lx phys1=%lx\n",
               f_width, f_height, pix_format, f_stride0, f_stride1,
               (unsigned long)dump_info.v_frame.phys_addr[0],
               (unsigned long)dump_info.v_frame.phys_addr[1]);
        dump_frame_info_logged_ = true;

        // 首帧格式 sanity check：目前仅接受两种 chn1 输出 —— BGR planar（参考 demo 理想路径），
        // 或 NV12/YUV420SP（当前板子实际交付的格式，会在 CPU 侧做一次转换）。
        if (pix_format != PIXEL_FORMAT_BGR_888_PLANAR &&
            pix_format != PIXEL_FORMAT_YUV_SEMIPLANAR_420) {
            printf("ERROR: unsupported AI channel pixel_format=%d (expected BGR_888_PLANAR=%d or YUV_SEMIPLANAR_420=%d)\n",
                   pix_format,
                   (int)PIXEL_FORMAT_BGR_888_PLANAR,
                   (int)PIXEL_FORMAT_YUV_SEMIPLANAR_420);
            kd_mpi_vicap_dump_release(vicap_dev, VICAP_CHN_ID_1, &dump_info);
            memset(&dump_info, 0, sizeof(k_video_frame_info));
            return -1;
        }
        if (pix_format == PIXEL_FORMAT_BGR_888_PLANAR) {
            printf("AI chn1 delivering BGR planar directly, using memcpy fast path.\n");
        } else {
            printf("AI chn1 delivering NV12, converting to RGB CHW on CPU each frame.\n");
        }
    }

    // 不同格式对应不同的 dump 帧内存大小
    size_t dump_bytes = 0;
    if (pix_format == PIXEL_FORMAT_BGR_888_PLANAR) {
        dump_bytes = (size_t)AI_FRAME_CHANNEL * AI_FRAME_HEIGHT * AI_FRAME_WIDTH;
    } else {
        // NV12: Y 平面 H*W + UV 交错平面 H*W/2
        dump_bytes = (size_t)AI_FRAME_HEIGHT * AI_FRAME_WIDTH * 3 / 2;
    }

    // 把 dump 帧映射成 CPU-cached 虚拟地址（参考 ai_poc/face_detection/main.cc:93）
    void *dump_vaddr = kd_mpi_sys_mmap_cached(dump_info.v_frame.phys_addr[0], (k_u32)dump_bytes);
    if (dump_vaddr == nullptr) {
        printf("kd_mpi_sys_mmap_cached failed for dumped frame.\n");
        kd_mpi_vicap_dump_release(vicap_dev, VICAP_CHN_ID_1, &dump_info);
        memset(&dump_info, 0, sizeof(k_video_frame_info));
        return -1;
    }

    if (pix_format == PIXEL_FORMAT_BGR_888_PLANAR) {
        // ISP 直出 BGR planar（实际为 RGB CHW 布局）—— 直接搬运到私有缓存
        memcpy(ai_buf_vaddr_, dump_vaddr, ai_buf_size_);
    } else {
        // YUV420SP(NV21/VU) → RGB HWC（cv::cvtColor） → R/G/B plane 解交错写入 ai_buf
        //
        // 1. 把 YUV420SP 帧当作 (H*3/2) 行 × W 列的单通道 Mat
        // 2. cv::COLOR_YUV2RGB_NV21 产出 H×W×3 的 RGB HWC
        // 3. 手动把 HWC 拆成 R/G/B plane 写入 ai_buf（CHW 布局，uint8）
        //
        // ai_buf 的 plane 次序按 ISP "BGR_888_PLANAR（实际RGB）" 约定：
        //   plane0 = R，plane1 = G，plane2 = B
        // main.cc 的注册路径和 AI tensor shape {3, H, W} 都基于这个约定。
        cv::Mat yuv420sp_view(AI_FRAME_HEIGHT * 3 / 2, AI_FRAME_WIDTH, CV_8UC1, dump_vaddr);
        cv::cvtColor(yuv420sp_view, bgr_hwc_scratch_, cv::COLOR_YUV2RGB_NV21);

        uint8_t *const ai_data = static_cast<uint8_t *>(ai_buf_vaddr_);
        uint8_t *const dst_r = ai_data + 0 * AI_FRAME_HEIGHT * AI_FRAME_WIDTH;
        uint8_t *const dst_g = ai_data + 1 * AI_FRAME_HEIGHT * AI_FRAME_WIDTH;
        uint8_t *const dst_b = ai_data + 2 * AI_FRAME_HEIGHT * AI_FRAME_WIDTH;
        const uint8_t *src = bgr_hwc_scratch_.data;
        const int total_pixels = AI_FRAME_HEIGHT * AI_FRAME_WIDTH;
        for (int i = 0; i < total_pixels; ++i) {
            dst_r[i] = src[i * 3 + 0];
            dst_g[i] = src[i * 3 + 1];
            dst_b[i] = src[i * 3 + 2];
        }
    }

    kd_mpi_sys_munmap(dump_vaddr, (k_u32)dump_bytes);

    // 注意：dump_release 现在挪到 ReleaseFrame 里调用，整体生命周期对齐
    // ai_poc/face_detection/main.cc:77-147 —— dump → mmap+memcpy → munmap
    // → [AI 推理] → [OSD insert] → dump_release 是在循环末尾统一执行。
    // 如果这里过早 release，VICAP 内部的帧队列状态和 mmap_cached 的残留
    // 映射之间会出现 timing 竞争，下一次 dump_frame 会连续失败。

    // DumpRes 指向私有缓存；下游 host_runtime_tensor 直接消费这块地址，
    // 无论 chn1 实际交付的是 BGR planar 还是 NV12，调用方看到的永远是 RGB CHW。
    dump_res.virt_addr = reinterpret_cast<uintptr_t>(ai_buf_vaddr_);
    dump_res.phy_addr = (uintptr_t)ai_buf_paddr_;
    dump_res.width = f_width;
    dump_res.height = f_height;
    dump_res.stride0 = f_stride0;
    dump_res.stride1 = f_stride1;
    dump_res.pixel_format = pix_format;
    dump_res.mmap_size = ai_buf_size_;

    return 0;
}

int PipeLine::ReleaseFrame(DumpRes &dump_res){
    ScopedTiming st("PipeLine::ReleaseFrame", debug_mode_);
    int ret = 0;

    // 对齐 ai_poc/face_detection 的循环末尾 release 模式：
    // 只要 GetFrame 成功 dump 过，dump_info 里就有一帧待释放。
    if (dump_info.v_frame.phys_addr[0] != 0) {
        ret = kd_mpi_vicap_dump_release(vicap_dev, VICAP_CHN_ID_1, &dump_info);
        if (ret) {
            printf("kd_mpi_vicap_dump_release failed: ret=0x%x\n", ret);
        }
        memset(&dump_info, 0, sizeof(k_video_frame_info));
    }

    // 清空 DumpRes 语义字段
    dump_res.virt_addr = 0;
    dump_res.phy_addr = 0;
    dump_res.width = 0;
    dump_res.height = 0;
    dump_res.stride0 = 0;
    dump_res.stride1 = 0;
    dump_res.pixel_format = 0;
    dump_res.mmap_size = 0;
    return ret;
}

int PipeLine::InsertFrame(void* osd_data){
    ScopedTiming st("PipeLine::InsertFrame", debug_mode_);
    int ret=0;

    // 将外部生成的 OSD 数据拷贝到 VB 映射的内存中
    memcpy(insert_osd_vaddr, osd_data, OSD_WIDTH * OSD_HEIGHT * OSD_CHANNEL);

    // 插入到 VO 的 OSD layer
    if (kd_mpi_vo_chn_insert_frame(osd_vo_id + kOsdInsertChnOffset, &osd_frame_info) != K_SUCCESS) {
        printf("ERROR: kd_mpi_vo_chn_insert_frame failed for OSD\n");
        return -1;
    } 
    return ret;
}

int PipeLine::Destroy()
{
    ScopedTiming st("PipeLine::Destroy", debug_mode_);
    int ret = 0;
    int first_error = 0;
    auto record_error = [&](const char *step, int err) {
        if (err == 0) {
            return;
        }
        printf("PipeLine::Destroy step failed: %s ret=0x%x\n", step, err);
        if (first_error == 0) {
            first_error = err;
        }
    };

    if (dump_info.v_frame.phys_addr[0] != 0) {
        ret = kd_mpi_vicap_dump_release(vicap_dev, VICAP_CHN_ID_1, &dump_info);
        record_error("kd_mpi_vicap_dump_release", ret);
        memset(&dump_info, 0, sizeof(k_video_frame_info));
    }

    // ------------------ 停止 VICAP ------------------
    if (vicap_stream_started_) {
        ret = kd_mpi_vicap_stop_stream(vicap_dev);
        record_error("kd_mpi_vicap_stop_stream", ret);
        vicap_stream_started_ = false;
    }

    if (vicap_vo_bound_) {
        vicap_mpp_chn.mod_id = K_ID_VI;
        vicap_mpp_chn.dev_id = vicap_dev;
        vicap_mpp_chn.chn_id = vicap_chn_to_vo;
        vo_mpp_chn.mod_id    = K_ID_VO;
        vo_mpp_chn.dev_id    = vo_dev_id;
        vo_mpp_chn.chn_id    = K_VO_DISPLAY_CHN_ID1;
        ret = kd_mpi_sys_unbind(&vicap_mpp_chn, &vo_mpp_chn);
        record_error("kd_mpi_sys_unbind", ret);
        vicap_vo_bound_ = false;
    }

    // 反初始化 VICAP
    if (vicap_inited_) {
        ret = kd_mpi_vicap_deinit(vicap_dev);
        record_error("kd_mpi_vicap_deinit", ret);
        vicap_inited_ = false;
    }

    // ------------------ 关闭 OSD / VO ------------------
    if(USE_OSD == 1 && vo_osd_enabled_)
    {
        ret = kd_mpi_vo_osd_disable(osd_vo_id);
        record_error("kd_mpi_vo_osd_disable", ret);
        vo_osd_enabled_ = false;
    }

    if (vo_video_enabled_) {
        ret = kd_mpi_vo_disable_video_layer(vi_vo_id);
        record_error("kd_mpi_vo_disable_video_layer", ret);
        vo_video_enabled_ = false;
    }

    /* 等待一帧时间，确保 VO 释放 VB */
    k_u32 display_ms = 1000 / 33;
    usleep(1000 * display_ms);

    // ------------------ 清理 OSD 内存 ------------------
    if(USE_OSD == 1)
    {
        if (insert_osd_vaddr != nullptr) {
            ret = kd_mpi_sys_munmap(reinterpret_cast<void*>(insert_osd_vaddr),
                                    osd_mmap_size_);
            record_error("kd_mpi_sys_munmap(osd)", ret);
            insert_osd_vaddr = nullptr;
            osd_mmap_size_ = 0;
            osd_frame_size_ = 0;
        }

        if (osd_block_acquired_) {
            ret = kd_mpi_vb_release_block(handle);
            record_error("kd_mpi_vb_release_block", ret);
            osd_block_acquired_ = false;
            handle = VB_INVALID_HANDLE;
        }

        if (osd_pool_id != VB_INVALID_POOLID){
            ret = kd_mpi_vb_destory_pool(osd_pool_id);
            record_error("kd_mpi_vb_destory_pool", ret);
            osd_pool_id = VB_INVALID_POOLID;
        }
    }

    if (connector_fd_ >= 0) {
        if (close(connector_fd_) != 0) {
            record_error("close(connector_fd_)", errno);
        }
        connector_fd_ = -1;
    }

    // 释放 AI 输入私有缓存（在 VB exit 之前，保持与 mmz 配对）
    if (ai_buf_vaddr_ != nullptr) {
        ret = kd_mpi_sys_mmz_free(ai_buf_paddr_, ai_buf_vaddr_);
        record_error("kd_mpi_sys_mmz_free", ret);
        ai_buf_vaddr_ = nullptr;
        ai_buf_paddr_ = 0;
        ai_buf_size_ = 0;
    }

    // ------------------ 反初始化 VB ------------------
    if (vb_inited_by_pipeline_) {
        ret = kd_mpi_vb_exit();
        record_error("kd_mpi_vb_exit", ret);
        vb_inited_by_pipeline_ = false;
    } else {
        printf("Keep pre-initialized VB untouched.\n");
    }

    return first_error;
}
