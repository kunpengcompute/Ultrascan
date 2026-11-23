# 用户指南

本文档介绍Ultrascan的Grey编译参数设置、正则匹配反馈优化技术和通用字节码使用方法，涵盖编译参数覆盖与重置、反馈优化流程、跨平台字节码部署，适用于在AArch64和x86平台编译、扫描规则集的用户。

## 前提条件

请参考《[安装指南](./installation_guide.md)》完成Ultrascan安装和编译。需要使用正则匹配反馈优化技术时，必须在AArch64平台显式开启对应构建选项。

## Grey编译参数设置

Ultrascan不再从仓库根目录、可执行文件目录或其父目录读取`config.txt`。需要调整Grey编译参数时，应在编译数据库之前调用`hs_set_grey_overrides()`；编译结束后可调用`hs_reset_grey_overrides()`恢复默认值。

常用配置项如下：

- `allowLily`：用于控制短字节优化特性开关，`1`表示开启，`0`表示关闭。
- `allowNeoFdr`：用于控制假阳性阻断特性开关，`1`表示开启，`0`表示关闭。该配置项不用于开启V5.8.0新增的正则匹配反馈优化技术。
- `allowHao`：用于控制Hao多模匹配算法，默认`0`表示关闭；`1`表示允许编译器选择Hao，具体条件见下文。

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

完整的配置格式、错误处理、示例程序参数和API定义见[Grey配置API](./api_reference.md#grey配置api)。

## Hao多模匹配算法

Hao是AArch64平台的多模匹配算法，通过位图及一级哈希表过滤和二级SVE并行校验减少候选匹配开销。它作用于编译器提取的literal或fragment，后续仍由Ultrascan完成完整规则的匹配和上报，因此原有正则表达式语法及扫描API保持不变。

### 启用与适用范围

Hao默认关闭。在编译数据库之前调用`hs_set_grey_overrides("allowHao:1;")`，或在`hsdump`、`hsbench`中传入`-G "allowHao:1;"`即可允许使用Hao，无需单独调用内部引擎接口。开启后仍需满足以下条件：

- 进入FDR引擎选择流程；Noodle、Teddy等引擎仍可优先处理适合它们的literal集合。
- 按每个内部matcher的literal集合计算：少于1000条不选Hao；1000~3999条要求平均长度至少为7.5字节；4000条及以上通过规模筛选。这些数值不等同于输入正则表达式的条数或长度。
- DOT候选额外要求至少4000条内部literal；自动选择、显式设置`haoHashMode:2`及通用字节码中的DOT变体均执行这一检查，并需满足后续可行性和碰撞桶条件。`haoHashMode`只选择哈希路径，不绕过准入。BEXT的规模条件保持不变。
- 编译器必须能够完整覆盖该集合，包括大小写不敏感和附加mask/cmp约束。存在无法支持的literal、掩码冲突、展开超限或表容量不足时，继续使用其他匹配引擎。
- 候选构建完成后，碰撞桶占非空一级表桶的比例不得超过5%（等于5%允许）。超过门槛时，普通自动编译继续使用FDR；通用编译分别检查DOT/BEXT，不保留不合格的变体。该比例是规则建表统计，不能保证真实语料下的吞吐一定高于FDR，部署前仍应进行同条件性能对照。
- 构建产物包含所需内核，且运行平台满足下表要求。

| 路径 | 指令集要求 | 运行期SVE向量长度 |
| --- | --- | --- |
| DOT | SVE | 至少256位 |
| BEXT | SVE、SVE2、SVE2 BitPerm | 至少256位 |

普通编译及`hsbench -e`使用单一路径选择：`haoHashMode:0`在BEXT可用时优先选BEXT，否则尝试DOT。显式指定BEXT而平台不支持时，Hao构建不被采用，不会自动改选DOT。运行时向量长度不足时使用保留的FDR路径。Hao的性能收益取决于规则集和语料，建议按《[快速入门](./quick_start.md#hao算法验证与调优)》对比命中数、吞吐和数据库大小。

### Grey参数

Hao参数统一通过Grey覆盖字符串设置，不读取旧的Hao环境变量或`config.txt`。

| 参数 | 默认值 | 说明 |
| --- | ---: | --- |
| allowHao | 0 | 1允许选择Hao，0关闭；不跳过规模与可行性检查。 |
| haoHashMode | 0 | 单一路径编译时：0自动、1指定BEXT、2指定DOT。通用字节码的多变体编译行为见下节。 |
| haoKeyBits | 0 | BEXT自动selector搜索中的目标键宽；0默认搜索18~22位，手动调优可使用12~22位。最终键宽还取决于可选bit数。 |
| haoSelectorMode | 0 | BEXT的bit选择策略：0自动、1默认、2动态展开、3哈希优化、4高位对齐。自动策略默认比较默认、动态展开和高位对齐候选。 |
| haoDebug | 0 | 位掩码：1向stderr输出引擎选择信息，2输出编译统计，4启用运行统计。可按位组合，例如3输出选择信息和编译统计。运行统计还需要专门构建，见《[开发指南](./developer_guide.md#诊断与验证)》。 |
| haoDebugAuto | 0 | 仅支持DUMP_SUPPORT的构建：非零值输出BEXT自动搜索的候选统计；小于18的有效值还会向下扩展搜索范围，例如16搜索16~22位。 |

`haoKeyBits`和`haoDebugAuto`用于`haoSelectorMode:0`的BEXT自动搜索；显式selector策略使用22位目标键宽。DOT使用22位键空间，不使用BEXT的键宽和selector调优参数。建议保留自动配置，先通过`haoDebug:3`确认是否实际生成Hao。

`haoDebug:1`还会输出`[HAO][Admission]`，包含路径、literal数、碰撞桶比例、5%门槛及`ACCEPT`或`REJECT_COLLISION`决策。通用字节码中拒绝BEXT后，若DOT通过准入且可执行，仍可能选择DOT；只有没有可用Hao变体时才使用FDR回退。新增准入条件用于编译期，已有数据库需要重新编译才能应用。

### 通用字节码中的Hao

公开`fat_hs_*`编译接口和`hsdump -U`在允许Hao且通过筛选时，为AArch64部分保留FDR回退表，并附加编译主机支持且成功构建的DOT/BEXT变体；x86部分禁用Hao。多变体编译会分别尝试DOT和BEXT，`haoHashMode`不用于限制只保留其中一个变体。编译主机缺少的能力不会凭空生成对应变体，例如在x86主机上生成通用字节码不会携带Hao变体。

加载后，根据数据库携带的变体、运行库已编入的内核、CPU/操作系统能力和当前SVE向量长度，按BEXT、DOT、FDR回退的优先级执行。这样，同一份通用字节码可在满足基础部署要求、但SVE能力不同的平台上运行。

`hsbench -e`用于比较Grey指定的单一路径；它虽使用通用字节码容器，但不生成上述多变体组合。验证实际部署文件时，应使用`hsdump -U`生成文件，再通过`hsbench -U FILE`加载。加载已有数据库时，`-G`不会重建或更改其中的哈希路径。

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

首次体验建议使用[hspgo工具流程](./quick_start.md#hspgo正则匹配反馈优化技术工具)，应用集成时请参考[正则匹配反馈优化技术API](./api_reference.md#正则匹配反馈优化技术api)。

## 通用字节码

需要将同一规则集字节码部署到x86和鲲鹏计算平台时，可以使用`hsdump`生成通用字节码，或调用`fat_hs_*`公开接口完成编译、序列化和反序列化。

工具操作见《[快速入门](./quick_start.md#hsdump通用字节码生成工具)》，函数定义见[通用字节码API](./api_reference.md#通用字节码api)。

## 修订记录

| 文档版本 | 发布日期 | 修改说明 |
| --- | --- | --- |
| 01 | 2026-12-30 | 第一次正式发布 |
