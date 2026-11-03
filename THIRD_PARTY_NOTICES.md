# 第三方来源与许可

本项目的统一自有代码许可证尚待作者确认，不覆盖或替换已有第三方文件的许可。

| 内容 | 来源与处理 |
|---|---|
| `firmware/src/platform.h`、`platform_zynq.c` | Xilinx 平台示例衍生文件；原有版权、再分发条件和免责声明逐字保留 |
| `firmware/src/lscript.ld`、`Xilinx.spec`、BSP配置参考 | Xilinx 工具生成/配套配置；保留原声明，不声明为完全自主编写 |
| BD 内 DMA、PS7、SmartConnect、FIFO、Clock Wizard 等 | AMD/Xilinx Vivado IP；仓库保留配置，生成实现需使用工具及适用许可 |
| lwIP/BSP/驱动库 | 由 Vitis 2022.2 从工具安装生成，不打包二进制库；使用时保留生成代码对应声明 |
| DDS波形、FPGA/PS应用及测试 | 从当前开发工程复制；作者仍需确认任何早期引入代码的来源和授权，不以本清单替代权属确认 |

没有收录整个 Vivado/Vitis 安装目录、已编译 IP 库、BSP 二进制、第三方软件安装包、专利交底书或 AI API Key。

本快照包含的工具生成配置、报告和源码不能据此声称全部适用某一新的统一许可证。
