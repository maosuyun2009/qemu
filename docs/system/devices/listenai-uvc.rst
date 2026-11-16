聆思虚拟 UVC 摄像头
===================

``CONFIG_LISTENAI_MUSB`` 构建包含 ``usb-uvc`` 设备和 VenusA MUSB 主机模型。
设备使用一张不可变的 baseline JPEG，提供 USB 2.0 high-speed、UVC 1.0、
MJPEG 640×480、20 fps 的单一模式。它不使用电脑摄像头，也不需要图形界面。

启动
----

在 ``venusa-rd-evb`` 上设置环境变量即可连接摄像头：

.. code-block:: sh

   LISEM_QEMU_UVC_IMAGE=/absolute/path/mock-640x480.jpg \
   LISEM_QEMU_SOC_CLOCK=10000 \
   LISEM_QEMU_BUDGET_NS=5000000000 \
   LISEM_QEMU_REPORT=/absolute/path/report.json \
   qemu-system-riscv32 \
     -machine venusa-rd-evb -smp 2 \
     -icount shift=0,align=off,sleep=off \
     -display none -audio none -monitor none \
     -drive if=pflash,format=raw,file=/absolute/path/flash.bin \
     -serial stdio

``flash.bin`` 是按板型 Flash 容量准备的完整镜像，不是裸应用 BIN。
应用与资源须按固件自己的发布布局写入。镜像和运行输出使用独立路径，
不要与另一个活动实例共享可写存储。

不设置 ``LISEM_QEMU_UVC_IMAGE`` 时，主机端口为空。图像必须恰为 640×480，
不超过 32768 字节，具有 SOI/EOI，且能被完整解码。渐进式、损坏、截断或
不存在的图像会导致启动失败。设备在启动时读取文件，此后重复发送相同字节。
也可以不使用环境变量，显式添加 ``-device usb-uvc,image=/absolute/path/mock.jpg``；
不要同时使用这两种方式。

传输与时间
----------

固件通过 EP0 完成描述符枚举及 26 字节 PROBE/COMMIT 协商，再启用 VS interface 1
的 alternate setting 1。端点 ``0x81`` 使用 ISO IN，最大单包 1024 字节。
每包带两字节 UVC header，包含 FID、EOF 和 EOH；其余字节来自原始 JPEG。

第一帧在启流后 50 ms 虚拟时间到达，之后每 50 ms 产生一帧。收帧间隙的 ISO
请求返回零长度包，不伪造图像或 NAK。设备仅保留最新一帧；主机过慢导致未完成
帧被替换时，增加丢帧计数。帧率由 QEMU 虚拟时钟驱动，不保证宿主墙钟时间
达到 20 fps，也不代表实际硬件吞吐量。

通过 QMP ``qom-get`` 读取自动连接设备的状态：

.. code-block:: json

   {"execute":"qom-get","arguments":{
     "path":"/machine/soc/usb/camera","property":"x-lisem-uvc-state"}}

返回 JSON 字符串，包含 ``streaming``、``frames``、``completed_frames``、
``bytes``、``dropped_frames``、``frame_ns`` 和 ``next_frame_ns``。
``bytes`` 只统计 JPEG 数据，不包含 UVC header；计数用于该设备生命周期的
观测，不等同于固件完成解码或算法推理的帧数。

边界
----

MUSB 实现 EP0 PIO 和单事务 high-speed ISO IN PIO（``bInterval=1``）。
USB DMA、Bulk、Hub/TT、低速/全速、挂起恢复及其他未实现模式明确拒绝。
这不是通用的完整 MUSB 控制器模型。

UVC 只负责送入压缩图像。固件后续使用的 JPEG 硬件、DMA2D 像素重排和算法
加速器必须分别有可用模型。仅有枚举成功、流开启或摄像头侧完成帧计数，不能
判定固件解码或算法成功。VenusA JPEG 模型现支持 baseline 4:2:2、4:2:0、
4:4:4 的 MCU 对齐图像，最大 640×480；它接收固件写入的熵编码数据与量化/
Huffman 表，而不是读取摄像头宿主图片。JPEG 输入最多 32768 字节。
非 MCU 对齐尺寸、编码模式、缩放及未支持的配置明确拒绝，不返回伪造像素。

DMA2D 支持 JPEG 输入握手 4 和输出握手 5。通道 2 支持完整 4:2:2 图像的
MCU 块到 YUYV 重排；4:2:0/4:4:4 使用正常 P2M 输出原始 MCU 块，由固件完成
重排。FIFO 就绪、停钟、取消、复位和中断屏蔽/W1C 均保持独立语义。
JPEG 解码完成事件具有有界虚拟延迟，该延迟不是硅片逐周期时序校准。

VenusA 运行报告及 ``x-lisa-snapshot`` 的 ``jpeg`` 字段包含
``decoded_frames`` 和 ``output_bytes``：分别统计有效解码次数和真实读取的
原始像素字节。JPEG 外设复位保留统计，SoC 复位清零。它们不代表算法推理
成功。DMA2D 通道 0 支持下述功能缩放模型；算法的 LUNA 微程序支持仍须独立验证。

DMA2D 通道 0 缩放
~~~~~~~~~~~~~~~~~~

图像描述符锁存输入/输出尺寸、stride、格式、合并系数、Q12 步长和起始相位。
输入支持 RGB/BGR888、Y8 和四种 packed YUV422 顺序；输出支持 RGB/BGR888
或 Y8。YUV422 输入宽度须为偶数。原始缓冲区不能重叠，PSRAM 镜像地址也按
实际 RAM backing 检查。图像跨度须位于单一连续 RAM/ROM 映射，不跨越映射边界。

处理顺序为原始分量的 1/2/4/8/16 倍 box 合并、Q12 双线性插值、输出颜色转换。
合并和插值使用最近整数舍入。Q12 坐标为 ``start + output_index * step``；
仅在最后一块/带允许边缘复制，其他边界要求有效邻居样本。ROI 和分块偏移来自
固件设置的源/目标地址，stride 是整行字节跨度。重叠列/skip-first-sample 标志
尚未校准，启用时明确拒绝；RGB565、旋转、镜像、scatter/gather 等也不在此路径内。

颜色转换使用与 ARCS 功能模型一致的 full-range BT.601 定点系数及截断饱和规则。
这些舍入、插值和颜色转换规则定义的是可重复的功能模型，尚未证明与 VenusA
实物逐位相同，不用于画质精度或逐周期性能验收。

写出按 DMA burst 的字节数推进，RGB 像素跨 burst 时保留未写分量。完成 IRQ 只在
最后一个有效输出字节写入后产生；停钟保留进度，取消/复位不继续写入。shadow
描述符包含完整缩放配置，后续寄存器写入不会改变已经排队的图像操作。

验证
----

构建并运行 ``tests/qtest/listenai-musb-test``，通过 ``QTEST_QEMU_BINARY`` 指定
待测 ``qemu-system-riscv32``。测试会生成 baseline JPEG，不依赖外部业务固件。
覆盖枚举、PROBE/COMMIT、虚拟帧期限、FID/EOF、完整 JPEG 字节、停止流、复位
中止控制传输、RX FIFO flush、速度限制及非法图像拒绝。

``tests/qtest/venusa-jpeg-test`` 通过实际 JPEG 表项和熵编码数据验证 MCU
像素、4:2:2 DMA 重排、输入损坏、尺寸边界以及停钟/复位/停止取消。
``tests/qtest/venusa-scaler-test`` 检查合并、分数相位、颜色顺序、分块 stride、
精确 burst 进度、跨 burst RGB 像素、时钟/取消/复位、shadow 配置及非法地址。
``make qemu-venusa`` 同时执行 UVC、JPEG、缩放和已有 VenusA 外设回归。
