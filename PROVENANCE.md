# 来源与实现边界

研究输入为设备所有者自行备份的 M1 ATSAMD20G18A 主 Flash，共 262144 字节。

```text
SHA-256 6a8a9f72741e01d5ebc73467819079146553ec73339029eaf4cc49e8cbd1a7f9
```

读取阶段曾将两份完整读取逐字节比较，结果一致。备份范围是主 Flash，不包含 NVM User Row / 熔丝 / 校准辅助区；这不能称为芯片全部非易失内容的备份。

本仓库公开：地址与数据格式分析、原创 C 实现、复现工具、测试和构建配置。原厂镜像、整段反汇编/反编译程序、调试器序列号、私有主机信息和操作日志均不随仓库分发。研究使用了 Ghidra 11.4.2 辅助分析、Capstone Thumb 反汇编和 Unicorn 指定函数仿真；反编译结果中的常量和控制流需回到指令验证。

实现不是经过人员隔离的 clean-room 项目，也不宣称已有全部硬件兼容性。`src/` 与 `platform/` 是依据观察到的协议与行为重新编写的实现。新增输入校验等差异在协议文档中单独说明。

基础参考资料：

- [Microchip SAM D20 Family Data Sheet](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU32/ProductDocuments/DataSheets/SAM-D20-Family-Data-Sheet-DS60001504.pdf)：内存、PORT、SERCOM、TC、SWD 与复用功能。
- [Microchip SAM D20 引脚复用表](https://onlinedocs.microchip.com/oxy/GUID-2BBF0A73-A043-4A40-A6CA-0FF154E4BFFE-en-US-11/GUID-D350FD6F-9A6F-4025-903F-10E9CE79A524.html)：核对固件中的 pinmux 编码与 MCU 信号。
- [Ghidra](https://github.com/NationalSecurityAgency/ghidra)、[Capstone](https://github.com/capstone-engine/capstone)、[Unicorn](https://github.com/unicorn-engine/unicorn)：外部分析工具，不作为本项目固件的运行依赖。

分析结论只对应上述哈希；其他版本需要重新比对。硬件连通、传感器具体型号及新固件全部实物行为仍需单独验证。灯组颜色依据用户实机反馈：组 2 绿、组 3 黄、组 1 红；没有据此声称已上板验证新驱动。

## 运行时复用的库

- `third_party/samd20`：Microchip SAMD20 DFP 3.7.231 设备头文件的未修改子集，Apache-2.0；原包 URL 与 SHA-256 见目录内 `SOURCE.md`。不使用其启动实现。
- `third_party/cmsis`：ARM CMSIS 5.9.0 / commit `2b7495b8535bdcb306dac29b9ded4cfb679d7e5c` Cortex-M0+ 和 GCC 头文件，Apache-2.0；来源和许可保留在目录内。仅支持当前 GCC 构建。

这些官方开放头文件是运行构建依赖；原厂产品私有 SDK 和原厂二进制没有包含在仓库中。
