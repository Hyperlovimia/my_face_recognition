#include "video_pipeline.h"

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

static k_u32 osd_vb_create_pool()
{
    k_u32 private_pool_id;
    k_vb_pool_config pool_config;
    memset(&pool_config, 0, sizeof(pool_config));
    pool_config.blk_cnt = 3;
    pool_config.blk_size = VICAP_ALIGN_UP((OSD_WIDTH * OSD_HEIGHT * OSD_CHANNEL), VICAP_ALIGN_1K);
    pool_config.mode = VB_REMAP_MODE_NOCACHE;
    private_pool_id = kd_mpi_vb_create_pool(&pool_config);

    return private_pool_id;
}

int PipeLine::Create()
{
    ScopedTiming st("PipeLine::Create", debug_mode_);
    k_s32 ret = 0;
    bool vb_preinitialized = false;

    // =============================================================================================
    // 1. 配置 Video Buffer（VB）系统
    // =============================================================================================
    memset(&config, 0, sizeof(k_vb_config));
    config.max_pool_cnt = 64;  // 最多支持 64 个内存池

    // 设置 VB 全局配置
    ret = kd_mpi_vb_set_config(&config);
    if (ret == K_ERR_VB_BUSY) {
        printf("VB is already initialized by system/another app, reuse existing VB configuration.\n");
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

    // =============================================================================================
    // 2. 创建 OSD 专用 VB 内存池（用于 ARGB8888 叠加图层）
    // =============================================================================================
    // 用于存放一帧 OSD 数据（如 AI 结果绘制）
    if(USE_OSD == 1){
        k_vb_pool_config pool_config;
        memset(&pool_config, 0, sizeof(pool_config));
        pool_config.blk_cnt = 3; // 3 个缓冲块，避免帧冲突
        pool_config.blk_size = VICAP_ALIGN_UP((OSD_WIDTH * OSD_HEIGHT * OSD_CHANNEL), VICAP_ALIGN_1K);
        pool_config.mode = VB_REMAP_MODE_NOCACHE; // 非 cache 映射，避免缓存一致性问题
        osd_pool_id = kd_mpi_vb_create_pool(&pool_config);
    }


    // =============================================================================================
    // 3. 屏幕（Connector）配置
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
        return ret;
    }

    // 初始化 connector（配置时序、分辨率等）
    ret = kd_mpi_connector_init(connector_fd, connector_info);
    if (ret) {
        printf("ERROR: kd_mpi_connector_init failed, ret=%d\n", ret);
        return ret;
    }

    // 关闭设备句柄（配置完成即可关闭）
    ret = kd_mpi_connector_close(connector_fd);
    if (ret) {
        printf("ERROR: kd_mpi_connector_close failed, ret=%d\n", ret);
        return ret;
    }

    // =============================================================================================
    // 4. 配置 VO（视频输出层：用于显示摄像头画面）
    // =============================================================================================
    kd_mpi_vo_disable_video_layer(vi_vo_id);  // 先关闭 layer，避免旧配置干扰

    memset(&vi_vo_attr, 0, sizeof(vi_vo_attr));
    vi_vo_attr.display_rect.x  = 0;
    vi_vo_attr.display_rect.y  = 0;
    vi_vo_attr.img_size.width  = DISPLAY_WIDTH;
    vi_vo_attr.img_size.height = DISPLAY_HEIGHT;
    vi_vo_attr.pixel_format    = PIXEL_FORMAT_YUV_SEMIPLANAR_420; // NV12
    vi_vo_attr.stride          = (DISPLAY_WIDTH / 8 - 1) + ((DISPLAY_HEIGHT - 1) << 16);
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

    printf("VICAP to VO: layer=%d configured for %ux%u NV12, rotate90=%d\n",
           vi_vo_id, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ROTATE ? 1 : 0);

    // =============================================================================================
    // 5. 配置 OSD 层（ARGB8888 叠加图层）
    // =============================================================================================
    if(USE_OSD == 1){
        kd_mpi_vo_osd_disable(osd_vo_id);

        memset(&osd_vo_attr, 0, sizeof(osd_vo_attr));
        osd_vo_attr.display_rect.x  = 0;
        osd_vo_attr.display_rect.y  = 0;
        osd_vo_attr.img_size.width  = OSD_WIDTH;
        osd_vo_attr.img_size.height = OSD_HEIGHT;
        osd_vo_attr.pixel_format    = PIXEL_FORMAT_BGRA_8888;
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

        printf("OSD to VO: layer=%d configured for %ux%u BGRA8888, rotate90=%d\n",
               osd_vo_id, OSD_WIDTH, OSD_HEIGHT, DISPLAY_ROTATE ? 1 : 0);

        // --------------------- 从 OSD VB 池获取一块缓存，用于写入叠加数据 ---------------------
        k_s32 size = VICAP_ALIGN_UP(OSD_HEIGHT * OSD_WIDTH * OSD_CHANNEL, VICAP_ALIGN_1K);

        // 从指定内存池中申请一块缓存
        handle = kd_mpi_vb_get_block(osd_pool_id, size, NULL);
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
        k_u32* virt_addr = (k_u32 *)kd_mpi_sys_mmap(phys_addr, size);
        if (virt_addr == NULL)
        {
            printf("%s mmap error\n", __func__);
            return -1;
        }

        // 初始化 OSD 帧描述结构
        memset(&osd_frame_info, 0, sizeof(osd_frame_info));
        osd_frame_info.v_frame.width        = OSD_WIDTH;
        osd_frame_info.v_frame.height       = OSD_HEIGHT;
        osd_frame_info.v_frame.stride[0]    = OSD_WIDTH*4;
        osd_frame_info.v_frame.pixel_format = PIXEL_FORMAT_BGRA_8888;
        osd_frame_info.mod_id               = K_ID_VO;
        osd_frame_info.pool_id              = osd_pool_id;
        osd_frame_info.v_frame.phys_addr[0] = phys_addr;

        // 保存虚拟地址，用于后续 memcpy 写入 OSD 数据
        insert_osd_vaddr = virt_addr;
        printf("phys_addr is %lx g_pool_id is %d \n", phys_addr, osd_pool_id);
    }


    // =============================================================================================
    // 6. 传感器探测 & VICAP 设备配置
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
    // 7. VICAP 通道 0：输出到 VO 显示
    // =============================================================================================
    k_vicap_chn_attr chn0_attr;
    memset(&chn0_attr, 0, sizeof(k_vicap_chn_attr));
    chn0_attr.out_win.width  = DISPLAY_WIDTH;
    chn0_attr.out_win.height = DISPLAY_HEIGHT;
    chn0_attr.crop_win       = dev_attr.acq_win;
    chn0_attr.scale_win      = chn0_attr.out_win;
    chn0_attr.crop_enable    = K_FALSE;
    chn0_attr.scale_enable   = K_FALSE;
    chn0_attr.chn_enable     = K_TRUE;
    chn0_attr.pix_format     = PIXEL_FORMAT_YUV_SEMIPLANAR_420; // NV12
    chn0_attr.buffer_num     = VICAP_MAX_FRAME_COUNT;
    chn0_attr.buffer_size    = VICAP_ALIGN_UP((DISPLAY_WIDTH * DISPLAY_HEIGHT * 3 / 2), VICAP_ALIGN_1K);
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
    ret = kd_mpi_sys_bind(&vicap_mpp_chn, &vo_mpp_chn);
    if (ret) {
        printf("kd_mpi_sys_bind failed:0x%x\n", ret);
    } else {
        vicap_vo_bound_ = true;
    }

    // =============================================================================================
    // 8. VICAP 通道 1：输出给 AI 使用（RGB Planar）
    // =============================================================================================
    k_vicap_chn_attr chn1_attr;
    memset(&chn1_attr, 0, sizeof(k_vicap_chn_attr));
    chn1_attr.out_win.width  = AI_FRAME_WIDTH;
    chn1_attr.out_win.height = AI_FRAME_HEIGHT;
    chn1_attr.crop_win       = dev_attr.acq_win;
    chn1_attr.scale_win      = chn1_attr.out_win;
    chn1_attr.crop_enable    = K_FALSE;
    chn1_attr.scale_enable   = K_FALSE;
    chn1_attr.chn_enable     = K_TRUE;
    chn1_attr.pix_format     = PIXEL_FORMAT_RGB_888_PLANAR; // AI 常用输入格式
    chn1_attr.buffer_num     = VICAP_MAX_FRAME_COUNT;
    chn1_attr.buffer_size    = VICAP_ALIGN_UP((AI_FRAME_WIDTH * AI_FRAME_HEIGHT * 3 ), VICAP_ALIGN_1K);
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
    int ret=0;
    dump_res.virt_addr = 0;
    dump_res.phy_addr = 0;
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

    // 将物理地址映射为虚拟地址，供 CPU 访问
    dump_res.virt_addr = reinterpret_cast<uintptr_t>(
        kd_mpi_sys_mmap(dump_info.v_frame.phys_addr[0],
                        AI_FRAME_CHANNEL*AI_FRAME_HEIGHT*AI_FRAME_WIDTH));
    if (dump_res.virt_addr == 0) {
        printf("kd_mpi_sys_mmap failed for dumped frame.\n");
        kd_mpi_vicap_dump_release(vicap_dev, VICAP_CHN_ID_1, &dump_info);
        memset(&dump_info, 0, sizeof(k_video_frame_info));
        return -1;
    }

    dump_res.phy_addr = reinterpret_cast<uintptr_t>(dump_info.v_frame.phys_addr[0]);
    return 0;
}

int PipeLine::ReleaseFrame(DumpRes &dump_res){
    ScopedTiming st("PipeLine::ReleaseFrame", debug_mode_);
    int ret=0;

    // 解除虚拟地址映射
    if (dump_res.virt_addr != 0) {
        kd_mpi_sys_munmap(reinterpret_cast<void*>(dump_res.virt_addr),
                          AI_FRAME_CHANNEL*AI_FRAME_HEIGHT*AI_FRAME_WIDTH);
    }

    // 释放 VICAP dump 帧
    if (dump_info.v_frame.phys_addr[0] != 0) {
        ret = kd_mpi_vicap_dump_release(vicap_dev, VICAP_CHN_ID_1, &dump_info);
        if (ret)
        {
            printf("kd_mpi_vicap_dump_release failed.\n");
        }
    }
    dump_res.virt_addr = 0;
    dump_res.phy_addr = 0;
    memset(&dump_info, 0, sizeof(k_video_frame_info));
    return ret;
}

int PipeLine::InsertFrame(void* osd_data){
    ScopedTiming st("PipeLine::InsertFrame", debug_mode_);
    int ret=0;

    // 将外部生成的 OSD 数据拷贝到 VB 映射的内存中
    memcpy(insert_osd_vaddr, osd_data, OSD_WIDTH * OSD_HEIGHT * OSD_CHANNEL);

    // 插入到 VO 的 OSD layer
    if (kd_mpi_vo_chn_insert_frame(osd_vo_id + 3, &osd_frame_info) != K_SUCCESS) {
        printf("ERROR: kd_mpi_vo_chn_insert_frame failed for OSD\n");
        return -1;
    } 
    return ret;
}

int PipeLine::Destroy()
{
    ScopedTiming st("PipeLine::Destroy", debug_mode_);
    int ret=0;

    // ------------------ 关闭 OSD ------------------
    if(USE_OSD == 1)
    {
        if (vo_osd_enabled_) {
            ret = kd_mpi_vo_osd_disable(osd_vo_id);
            if (ret) {
                printf("kd_mpi_vo_osd_disable failed.\n");
                return ret;
            }
            vo_osd_enabled_ = false;
        }
        if (osd_block_acquired_) {
            ret = kd_mpi_vb_release_block(handle);
            if (ret) {
                printf("kd_mpi_vb_release_block failed.\n");
                return ret;
            }
            osd_block_acquired_ = false;
        }
    }
    printf("kd_mpi_vb_release_block\n");

    // ------------------ 停止 VICAP ------------------
    if (vicap_stream_started_) {
        ret = kd_mpi_vicap_stop_stream(vicap_dev);
        if (ret) {
            printf("kd_mpi_vicap_stop_stream failed.\n");
            return ret;
        }
        vicap_stream_started_ = false;
    }

    // 反初始化 VICAP
    if (vicap_inited_) {
        ret = kd_mpi_vicap_deinit(vicap_dev);
        if (ret) {
            printf("kd_mpi_vicap_deinit failed.\n");
            return ret;
        }
        vicap_inited_ = false;
    }

    // ------------------ 解除 VI → VO 绑定 ------------------
    if (vo_video_enabled_) {
        ret = kd_mpi_vo_disable_video_layer(vi_vo_id);
        if (ret) {
            printf("kd_mpi_vo_disable_video_layer failed.\n");
            return ret;
        }
        vo_video_enabled_ = false;
    }

    if (vicap_vo_bound_) {
        vicap_mpp_chn.mod_id = K_ID_VI;
        vicap_mpp_chn.dev_id = vicap_dev;
        vicap_mpp_chn.chn_id = vicap_chn_to_vo;
        vo_mpp_chn.mod_id    = K_ID_VO;
        vo_mpp_chn.dev_id    = vo_dev_id;
        vo_mpp_chn.chn_id    = K_VO_DISPLAY_CHN_ID1;
        ret = kd_mpi_sys_unbind(&vicap_mpp_chn, &vo_mpp_chn);
        if (ret) {
            printf("kd_mpi_sys_unbind failed:0x%x\n", ret);
        }
        vicap_vo_bound_ = false;
    }

    /* 等待一帧时间，确保 VO 释放 VB */
    k_u32 display_ms = 1000 / 33;
    usleep(1000 * display_ms);

    // ------------------ 销毁 OSD 内存池 ------------------
    if (osd_pool_id != VB_INVALID_POOLID){
        if (insert_osd_vaddr != nullptr) {
            ret = kd_mpi_sys_munmap(reinterpret_cast<void*>(insert_osd_vaddr),
                                    OSD_WIDTH * OSD_HEIGHT * OSD_CHANNEL);
            if (ret) {
                printf("kd_mpi_sys_munmap failed.\n");
                return ret;
            }
            insert_osd_vaddr = nullptr;
        }
        ret = kd_mpi_vb_destory_pool(osd_pool_id);
        if (ret) {
            printf("kd_mpi_vb_destory_pool failed.\n");
            return ret;
        }
        osd_pool_id = VB_INVALID_POOLID;
    }

    // ------------------ 反初始化 VB ------------------
    if (vb_inited_by_pipeline_) {
        ret = kd_mpi_vb_exit();
        if (ret) {
            printf("kd_mpi_vb_exit failed.\n");
            return ret;
        }
        vb_inited_by_pipeline_ = false;
    } else {
        printf("Keep pre-initialized VB untouched.\n");
    }

    return 0;
}
