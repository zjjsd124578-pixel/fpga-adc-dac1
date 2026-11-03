# 可重构信号发生与采集分析系统

基于 Zynq-7020 的双通道采集与单通道激励系统，将 ADC 采集、DDS 波形生成、幅值反馈控制、双通道幅相测量和自动扫频集成到同一套以太网控制流程中。

本目录为 2026-11-02 整理的源码快照，PL 来源为当前 Vivado 主工程，PS 来源为该工程的 `adc_dac_ps/src`。本次整理保留采集和控制算法，只调整发布目录、ROM/测试引用路径及构建入口。尚未选择统一开源许可证；正式公开发布前应完成 [发布准备](docs/release_readiness.md)。

## 硬件与开发环境

| 项目 | 配置 |
|---|---|
| FPGA/SoC | Zynq-7020，工程器件 xc7z020clg400-2 |
| ADC | AD9238，双通道 12 位，采样时钟 65 MHz |
| DAC | AD9708，单通道 8 位，DDS/数据更新时钟 65 MHz |
| 片上控制总线 | PS FCLK，约 142.857 MHz |
| 工具 | Vivado 2022.2、Vitis 2022.2；Python 3（命令行工具无额外依赖） |
| 网络 | 板端 192.168.1.10:6001，TCP，ASCII 行命令与 SCP1 二进制帧 |

## 功能及本快照范围

| 功能 | 源码与说明 |
|---|---|
| 双通道采集、触发、DDR 环形缓冲与预览 | `fpga/adc9238_if.v`、`adc_axis_packer.v`、`firmware/src/dma_capture.c` |
| DDS 波形、频率、数字幅值与开环偏置 | `fpga/dac_dds.v`、`dds_axi_ctrl.v`、`firmware/src/dds_ctrl.c` |
| ADC 反馈的幅值 PI | `fpga/amplitude_pi.v`、`sweep_measure_axi.v`、`firmware/src/sweep_ctrl.c` |
| 双通道幅相测量与直通校正 | `fpga/lockin_core.v`、`firmware/src/sweep_math.c`、`sweep_ctrl.c` |
| 一次自动执行粗扫定位与局部精扫 | `firmware/src/sweep_ctrl.c`；自动测量配置范围 1 kHz～1 MHz |
| 数字 PLL | ADC A 作为外部参考，驱动 DAC 跟踪；ADC B 用于外部验证 |
| 完整图形上位机、FFT 与 AI 辅助分析 | 作者另有实现，源码尚未提供，本快照不包含这些页面或功能的可运行实现 |

AI 在完整系统中用于解释本地测量结果、理论对照和辅助报告；不替代 FPGA/上位机计算测量值。本目录仅提供已取得的板端代码和 Python 命令行工具，不包含 API Key。

## 目录

```text
fpga/          RTL、ROM、BD 与 XDC 约束
firmware/      PS 应用源码及 BSP 配置参考
host/          命令行控制工具；完整 GUI 待补
scripts/       可迁移构建入口与离线测试入口
hardware/      已确认的接线与接口说明；测试板设计文件待补
docs/          架构、协议、构建、标定、发布准备
tests/         RTL/PS 回归测试与历史实现报告
```

## 从这里开始

1. 阅读 [接线与电平](hardware/README.md)，确认板卡、ADC/DAC 模块和负载条件。
2. 按 [构建与下载说明](docs/build.md) 重建 Vivado 工程、导出 XSA、生成 BSP 并编译 PS。
3. 电脑有线网卡与开发板处于同一子网，使用 [命令协议](docs/protocol.md) 对接。
4. 按 [验证说明](tests/README.md) 运行离线测试；实测结果另行记录。

## 已收录实测结果

已加入作者提供的实测记录及规范化数据，见 [实测结果说明](tests/reports/measured_results_20261102.md)。不同观测批次分别呈现，未核实的极值、PLL相位定义与测试条件明确标注。资源利用率与RC参考频率按记录重新计算，不改写板端标定。

## 测量与实现边界

- ADC 默认换算在当前代码中为 10000 mV/4096 码、零点 2048；这是现有前端显示约定，不是 AD9238 芯片本身允许 ±5 V 输入，也不代表独立精度标定。
- `A1000` 表示数字比例 100%，不表示 1 Vpp。输出幅值取决于模拟增益、频率、负载和偏置余量。
- 直流偏置为开环；稳幅、扫频与 PLL 需遵循模式互斥与退出确认规则。
- 历史报告显示已约束内部路径满足时序，但仍缺少完整外部 IO 延迟约束，并存在待审查的 CDC/DRC 提示。
- 约 100 kHz PLL 输出曾出现断续，原因和实测状态仍待验证；已收录PLL恢复时间填写值，仍缺起止判据与过程轨迹，幅值闭环动态恢复证据尚待补充。不得将功能入口存在当作全部指标已验收。

## 许可证与第三方内容

自有代码的统一许可证待作者确认。现有源文件中的 Xilinx 等版权与许可声明原样保留，见 [第三方说明](THIRD_PARTY_NOTICES.md)。无统一许可证时，本目录不宣称已完成开源授权。
