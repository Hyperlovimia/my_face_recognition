# 三进程同步单进程 OSD 修复与异步推理结果应用修复记录

日期：2026-04-19

## 1. 背景

上午的 `260419_SINGLE_PROCESS_EXIT_AND_OSD_FIXES_CHANGELOG.md` 只把 OSD 相关修复落到了
单进程入口 `face_recognition.elf`（即 `src/main.cc`）。同一轮改动里，`ipc_osd_draw.cc`
的 `bbox_to_osd_rect` 已经同步给了三进程，但**截图预览的 ARGB8888 写入与尺寸策略**
并未落到 `src/face_video_main.cc`。

在三进程模式下实际上板后暴露出三个现象：

1. 程序启动后，屏幕上不显示检测框。
2. 按 `i` 抓拍后，检测框才出现一次，然后一直卡在抓拍那一帧对应的人脸位置。
3. 抓拍图只闪一瞬就消失，并且仍然带有“半透明、被错误压缩、R/B 反色”问题，
   与修复前的单进程现象完全一致。

本轮把缺失的修复补齐到三进程，并修掉一个三进程独有的异步推理 bug。

## 2. 根因

### 2.1 注册预览仍走旧的 `BGR2BGRA` 路径

`src/face_video_main.cc` 中 `video_ipc_loop` 仍然使用：

```cpp
cv::cvtColor(dump_img, dump_img, cv::COLOR_BGR2BGRA);
cv::Mat resized_dump;
cv::resize(dump_img, resized_dump, cv::Size(OSD_WIDTH / 2, OSD_HEIGHT / 2));
cv::Rect roi(0, 0, resized_dump.cols, resized_dump.rows);
resized_dump.copyTo(draw_frame(roi));
```

而单进程已经切到：

- 手工按 `ARGB8888` 字节序重排，`A=255` 固定；
- 通道语义按 `R=src[0], G=src[1], B=src[2]` 处理；
- 等比缩放到 `OSD_WIDTH/2 × OSD_HEIGHT/2` 的上限；
- 旋转 `90°` 与视频层方向一致；
- 留出 16px 内边距。

未同步导致三进程的注册预览半透明、颜色错、尺寸错。

### 2.2 `display_state` 作用域问题

单进程 `src/main.cc` 的 `display_state` 声明在 `while` 循环**之外**，这样按下 `i`
置为 `2` 后可以一直保留到 `state==3` 注册提交把它复位回 `0`。

三进程 `src/face_video_main.cc` 中写成：

```cpp
while (!isp_stop)
{
    ...
    int state = cur_state.load();
    int display_state = 0;  // 每轮都被重置
    ...
}
```

结果注册预览只能存在 1 帧，这就是“抓拍后图片只闪一瞬就消失”的原因。

### 2.3 异步推理 worker 的 stale 判定过严

三进程的 `ai_infer_worker_thread` 里：

```cpp
if (snap != g_infer_capture_seq.load())
{
    g_metric_infer_stale.fetch_add(1);
    continue;   // 丢弃推理结果
}
```

意图是：如果推理期间采集侧又推进了 `capture_seq`，就丢弃当前推理结果，等下一轮。

问题是采集帧率通常高于推理完成速率，于是 `snap == capture_seq` 几乎永远不成立，
`g_last_infer_reply` 就永远写不进去，`g_infer_has_reply` 永远保持 `false`，
`ipc_draw_faces_osd` 拿到一个零值 reply 后直接 `return`，屏幕上就看不到检测框。

按 `i` 后，`state==2` 这一帧**不调用** `state==0` 的 `capture_seq.fetch_add`，
于是 worker 正好抓到一个不 stale 的窗口、一次性把 reply 写进去。此后
`g_last_infer_reply` 长期不变，屏幕上就表现为检测框“卡在抓拍那一帧的位置”。

## 3. 修改内容

### 3.1 `src/face_video_main.cc`：补齐预览渲染

新增匿名命名空间中的两个辅助函数（与 `src/main.cc` 保持一致）：

- `convert_preview_to_osd_argb8888()`：按 `ARGB8888` 手工打包，`R=src[0]`。
- `render_register_preview()`：先按 `DISPLAY_ROTATE` 旋转 90°，再等比缩放到
  `OSD_WIDTH/2 × OSD_HEIGHT/2` 上限，左上角固定 16px 内边距。

### 3.2 `src/face_video_main.cc`：`display_state` 跨迭代保留

- 将 `int display_state = 0;` 移出 `while` 循环，紧随 `dump_img` 声明之后。
- 保持原有：`state==2` 置为 `2`、`state==3` 置为 `0`、`state==1/4` 也置为 `0`。

### 3.3 `src/face_video_main.cc`：预览与检测框绘制互斥

`state` 分支结束后统一用：

```cpp
if (display_state == 2) {
    render_register_preview(draw_frame, dump_img);
} else {
    ipc_draw_faces_osd(draw_frame, &ai_reply);
}
```

把 `state==0` 内部的 `ipc_draw_faces_osd` 调用去掉，避免检测框画到预览之下，
行为与单进程 `main.cc` 的 `if/else` 一致。

### 3.4 `src/face_video_main.cc`：异步推理结果始终应用

把 worker 里的 `continue` 去掉，保留 stale 计数但不丢弃结果：

```cpp
if (snap != g_infer_capture_seq.load()) {
    g_metric_infer_stale.fetch_add(1);
}
// 无论是否 stale，都把 reply 写入 g_last_infer_reply
memcpy(&g_last_infer_reply, &reply, sizeof(reply));
g_infer_has_reply.store(true);
g_infer_last_displayed_seq.store(snap);
```

这样：

- 启动后第一帧推理一完成，`g_infer_has_reply` 立刻变 `true`，检测框就能出现；
- 即使推理永远比采集慢，`g_last_infer_reply` 也会被逐次刷新，不会出现“只在抓拍那一帧有检测框”的假象。

## 4. 修改文件

- `src/face_video_main.cc`

## 5. 预期效果

- 启动三进程后，只要 `face_ai` 正常推理，屏幕立刻能看到白色人脸检测框。
- 按 `i` 后，注册预览稳定显示在左上角（方向、颜色、尺寸均正确，不透明），
  直到输入姓名完成注册后才消失。
- 抓拍不再让检测框“卡住”；检测框持续随人脸移动刷新。
- `n` / `d` 不再留下预览残影。

## 6. 未迁移 / 已知局限

- `ipc_osd_draw.cc` 的框/文字颜色仍用 OpenCV BGRA 的 `cv::Scalar`，而 OSD 按
  `ARGB8888` 字节序消费，文字颜色会与注释不一致（白框无影响，因三通道同值）。
  当前用户未反馈此问题，暂不改动；如后续需要，可改为与预览相同的逐字节写入。
- `convert_preview_to_osd_argb8888` / `render_register_preview` 目前在
  `src/main.cc` 与 `src/face_video_main.cc` 各存一份。未来可合进
  `src/ipc_osd_draw.{h,cc}` 以消除重复。

## 7. 相关文档

- `archive/260408_RTSMART_SINGLE_UART_CHANGELOG.md`
- `archive/260419_SINGLE_PROCESS_EXIT_AND_OSD_FIXES_CHANGELOG.md`
