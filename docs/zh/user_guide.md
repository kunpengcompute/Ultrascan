# 用户指南

## 前提条件

请参考[安装指南](./installation_guide.md)完成Ultrascan安装和编译。需要使用正则匹配反馈优化技术时，必须在AArch64平台显式开启对应构建选项。

## Grey编译参数设置

Ultrascan不再从仓库根目录、可执行文件目录或其父目录读取`config.txt`。需要调整Grey编译参数时，应在编译数据库之前调用`hs_set_grey_overrides()`；编译结束后可调用`hs_reset_grey_overrides()`恢复默认值。

常用配置项如下：

- `allowLily`：用于控制短字节优化特性开关，`1`表示开启，`0`表示关闭。
- `allowNeoFdr`：用于控制假阳性阻断特性开关，`1`表示开启，`0`表示关闭。该配置项不用于开启V5.8.0新增的正则匹配反馈优化技术。

使用示例：

```c
hs_error_t err = hs_set_grey_overrides(
    "allowLily:1;allowNeoFdr:1;");
if (err != HS_SUCCESS) {
    /* 覆盖字符串无效，原有配置保持不变。 */
}

/* 调用hs_compile_*()编译数据库。 */

hs_reset_grey_overrides();
```

Grey覆盖字符串格式为`key:value;key:value;...`。传入`NULL`或空字符串也会恢复默认值。配置是进程级共享状态，建议在编译开始前完成设置，并避免在编译过程中并发修改。

完整的配置格式、错误处理、示例程序参数和API定义见[Grey配置API](./api_reference.md#3-grey配置api)。

## 正则匹配反馈优化技术

正则匹配反馈优化技术是V5.8.0新增特性，适用于“少量前端fragment频繁触发、但很少产生最终规则上报”的业务语料。其使用流程如下：

1. 使用普通编译接口生成baseline数据库。
2. 使用带collector的扫描接口采集业务语料。
3. 按阈值筛选高浪费fragment并生成feedback。
4. 使用feedback重新编译相同规则集。
5. 校验匹配结果后切换到新数据库，并继续观测性能。

使用时应注意：

- 当前仅支持AArch64，且必须在构建时显式设置`-DHS_ENABLE_FP_FEEDBACK=ON`。
- collector绑定创建它的具体数据库对象，并借用该对象的生命周期。
- collector不支持多线程并发写；每个扫描线程应使用独立collector，停止写入后再合并。
- baseline编译与反馈编译必须使用相同的表达式、ID、flags、扩展参数、模式及关键Grey配置。
- feedback编译前后应使用业务语料验证最终匹配结果，不能用性能采样代替正确性验证。
- 公开stream close/reset接口不携带collector，因此对应EOD阶段不在采样范围内。
- 关闭该能力的构建仍导出相关公共符号，但功能调用会返回`HS_ARCH_ERROR`；普通业务路径应继续调用原有编译和扫描接口。

首次体验建议使用[hspgo工具流程](./quick_start.md#hspgo正则匹配反馈优化技术工具)，应用集成时请参考[正则匹配反馈优化技术API](./api_reference.md#4-正则匹配反馈优化技术api)。

## 通用字节码

需要将同一规则集字节码部署到x86和鲲鹏计算平台时，可以使用`hsdump`生成通用字节码，或调用`fat_hs_*`公开接口完成编译、序列化和反序列化。

工具操作见[快速入门](./quick_start.md#hsdump通用字节码生成工具)，函数定义见[通用字节码API](./api_reference.md#2-通用字节码api)。
