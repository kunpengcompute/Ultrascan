# 用户指南

## 前提条件

请参考[安装指南](./installation_guide.md)完成Ultrascan的安装和编译指导。

## 特性使能

在Ultrascan根目录下配置config.txt文件，用于控制短字节优化、HAO算法引擎和假阳性阻断特性开关。短字节优化、HAO算法引擎和假阳性阻断特性默认开启。

>![](public_sys-resources/icon-note.gif) **说明：** 
>若不包含config.txt文件或config.txt文件格式有误，短字节优化、HAO算法引擎和假阳性阻断特性默认都关闭。

- allowLily：用于控制短字节优化特性开关，1表示开启特性，0表示关闭特性。
- allowHao：用于控制HAO算法引擎特性开关，1表示开启特性，0表示关闭特性。
- allowNeoFdr：用于控制假阳性阻断特性开关，1表示开启特性，0表示关闭特性。

config.txt文件示例内容如下：

```text
allowLily:1;allowHao:1;allowNeoFdr:1;
```

## HAO算法引擎

HAO算法是基于并行位提取的多模匹配加速算法。其基本原理是在编译期选择规则字节的关键bit位组成哈希key，并构建哈希表；运行期从输入窗口中并行提取相关bit位生成哈希key，快速过滤候选并进行后端校验，从而降低无效匹配开销。开启`allowHao`后，Ultrascan在编译规则集时会根据规则形态、目标平台能力和编译配置判断是否构建HAO匹配器；不满足条件时自动回退到原有匹配引擎，不需要用户修改业务代码。

HAO算法引擎在AArch64平台上结合SVE/SVE2能力进行运行期批量过滤。其中BEXT路径依赖SVE2 BitPerm能力；仅支持SVE的平台不会使用BEXT路径。
