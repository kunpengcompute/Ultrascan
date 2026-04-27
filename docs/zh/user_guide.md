# 用户指南<a name="ZH-CN_TOPIC_0000002518413720"></a>

## 特性使能<a name="ZH-CN_TOPIC_0000002518253766"></a>

在Hyperscan根目录下配置config.txt文件，用于控制短字节优化和假阳性阻断特性开关。短字节优化和假阳性阻断特性默认开启。

>![](public_sys-resources/icon-note.gif) **说明：** 
>若不包含config.txt文件或config.txt文件格式有误，短字节优化和假阳性阻断特性默认都关闭。

- allowLily：用于控制短字节优化特性开关，1表示开启特性，0表示关闭特性。
- allowNeoFdr：用于控制假阳性阻断特性开关，1表示开启特性，0表示关闭特性。

config.txt文件示例内容如下：

```bash
allowLily:1;allowNeoFdr:1;
```
