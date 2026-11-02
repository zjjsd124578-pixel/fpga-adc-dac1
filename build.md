# 构建、下载与启动

推荐工具版本为 Vivado 2022.2、Vitis 2022.2。以下命令从仓库根目录执行；工具可执行文件不在 PATH 时使用本机安装目录的完整路径。所有生成文件进入被忽略的 `build/` 目录。建议将仓库放在短的纯英文目录，例如 `D:/projects/fpga-adc-dac`。当前受限运行环境下，工具的部分路径归一化/目录复制操作遇到权限问题；本次用显式脚本路径与逐文件暂存验证了 Vivado 工程重建，Vitis全新平台生成尚未验证完成。Windows PowerShell 的批处理程序用 `&` 调用。

## 1. 重建 PL 工程

在已配置 Vivado 环境的终端运行：

```text
vivado -mode batch -source scripts/create_vivado_project.tcl
```

输出 `build/vivado/ADC.xpr`。脚本导入提交的 BD，重新绑定本机 ROM 路径、校验 BD、生成 IP 输出与顶层 wrapper。若工程已存在，脚本拒绝覆盖；使用 `-tclargs <新的构建目录>` 指定另一目录。

生成位流和含位流的硬件平台：

```text
vivado -mode batch -source scripts/build_bitstream.tcl
```

输出 `build/hardware/S2MM_wrapper.xsa` 与实现报告。重新实现后重新检查时序、外部 IO 约束、CDC 和 DRC，不能拿历史报告当作本次新构建结果。

## 2. 建立 Vitis 平台与应用

```text
xsct scripts/build_vitis.tcl
```

脚本使用新 XSA 建立独立的 `build/vitis/` 工作区，启用 standalone/lwip211，并将标准输入输出设置为 PS CoreSight。不会更改原开发工作区。

也可以在 Vitis GUI 新建 Platform Project，选新 XSA；建立 `ps7_cortexa9_0` standalone 域，添加 lwip211 1.8，设置 stdin/stdout 为 `ps7_coresight_comp_0`。新建空 C 应用，导入 `firmware/src` 全部文件，保留 `lscript.ld`，加入数学库 `m` 后编译。`firmware/config/system.mss.reference` 是原工作平台的配置参考，不是可直接复用的 BSP 库。

若已生成兼容 BSP，可单独运行严格编译入口：

```powershell
& ./scripts/build_firmware.ps1 -BspRoot '你的BSP根目录' -CompilerBin '你的Vitis安装目录/gnu/aarch32/nt/gcc-arm-none-eabi/bin'
```

BSP 根目录应包含 `bspinclude/include`、`bsplib/lib`，或标准 `include`、`lib`。必须使用和本次 XSA 匹配的 `xparameters.h`、libxil、liblwip4。生成的 ELF 在 `build/firmware/`，不会覆盖现有 Vitis 工程。

## 3. 下载并检查启动

1. 开发板上电，JTAG 与网线连接正确。
2. 使用新生成的位流配置 PL。
3. 在 Vitis 中选择新平台对应的 PS7 初始化，下载对应的 `adc_dac_ps.elf` 并运行。
4. 若使用 JTAG 日志，在 XSCT 连接并选中 Cortex-A9 #0 后启用 `readjtaguart -start`；本项目 BSP 的 stdout 指向 CoreSight，不应默认去看某个 USB 串口。
5. 检查 DDS 就绪、DMA 初始化成功、TCP 6001 监听与开发板 IP。程序会自动开始采集。

## 4. 网络与客户端

电脑有线网卡可设 192.168.1.2/24，板端固定 192.168.1.10。一次只允许一个 TCP 客户端；关闭占用连接的 GUI 后再运行命令行工具。发送命令后需要持续接收二进制帧，不能只写不读。

```text
python host/tools/feedback_cli.py --host 192.168.1.10
python host/tools/sweep_client.py --help
```

第一条默认只查询 LOOPSTATUS。工具不代表完整图形上位机。完整命令、模式切换和回包限制见 `docs/protocol.md`。
