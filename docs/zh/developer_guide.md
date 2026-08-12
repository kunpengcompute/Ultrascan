# 开发指南

## 文档定位

本文面向需要理解或维护Ultrascan内部实现的开发者，主要说明KHSEL内部接口。

Ultrascan新增公开API的函数定义、参数、返回值、生命周期和最小用例统一收录在[《API参考》](./api_reference.md)中。

开发指南不重复维护这些公开接口的逐项定义，以公共头文件和API参考为准。

## KHSEL内部函数说明

>![](public_sys-resources/icon-notice.gif) **须知：**
>KHSEL中的函数（`KHSEL_xxx`）是Ultrascan内部接口，应用程序无需显式调用。

KHSEL已优化函数如下：

| 名称 | 说明 |
| --- | --- |
| `KHSEL_BuildLily` | 单字节短规则匹配的编译函数。 |
| `KHSEL_LilyRunExec` | 单字节短规则匹配的运行函数。 |
| `KHSEL_BuildLilyForTeddy` | 2~4字节短规则匹配的编译函数。 |
| `KHSEL_LilyForTeddyRunExec` | 2~4字节短规则匹配的运行函数。 |

KHSEL源码已集成在Ultrascan仓库的`src/kunpeng-enhanced`目录中，不需要单独安装KHSEL软件包。

### `KHSEL_BuildLily`

函数功能：基于规则集内的单字节短规则进行规则编译，输出编译后的mask。

```c++
std::vector<u8> KHSEL_BuildLily(
    std::map<char, lilyReport> &lily,
    std::vector<u32> &reportVec,
    std::vector<u32> &ekeyVec);
```

| 参数 | 描述 | 输入/输出 |
| --- | --- | --- |
| `lily` | 单字节短规则集合。 | 输入 |
| `reportVec` | 单字节短规则对应的report ID。 | 输入 |
| `ekeyVec` | 单字节短规则对应的ekey。 | 输入 |

返回规则编译后的mask。

### `KHSEL_LilyRunExec`

函数功能：基于编译输出的mask，对运行期输入数据进行匹配。

```c
hs_error_t KHSEL_LilyRunExec(const struct RoseEngine *rose,
                             hs_scratch_t *scratch);
```

| 参数 | 描述 | 输入/输出 |
| --- | --- | --- |
| `rose` | 保存编译期输出结果的RoseEngine对象，必须非空。 | 输入 |
| `scratch` | 本次扫描使用的临时内存空间，必须非空。 | 输入 |

返回匹配结果错误码。

### `KHSEL_BuildLilyForTeddy`

函数功能：基于规则集内的2~4字节短规则进行规则编译，输出编译后的mask。

```c++
ue2::bytecode_ptr<lilyTeddy> KHSEL_BuildLilyForTeddy(
    std::map<std::string, lilyReport> &lilyForTeddy,
    std::priority_queue<LilyForTeddyPair,
                        std::vector<LilyForTeddyPair>,
                        CompareStringLength> &lilyForTeddyPQ,
    std::vector<u32> &reportVec,
    std::vector<u32> &ekeyVec,
    std::vector<u32> &lenVec);
```

| 参数 | 描述 | 输入/输出 |
| --- | --- | --- |
| `lilyForTeddy` | 2~4字节规则集合。 | 输入 |
| `lilyForTeddyPQ` | 按规则长度排序的优先队列。 | 输入 |
| `reportVec` | 规则对应的report ID。 | 输入 |
| `ekeyVec` | 规则对应的ekey。 | 输入 |
| `lenVec` | 规则对应的长度。 | 输入 |

返回规则编译后的mask。

### `KHSEL_LilyForTeddyRunExec`

函数功能：基于编译输出的mask，对运行期输入数据进行匹配。

```c
hs_error_t KHSEL_LilyForTeddyRunExec(const struct RoseEngine *rose,
                                     hs_scratch_t *scratch);
```

| 参数 | 描述 | 输入/输出 |
| --- | --- | --- |
| `rose` | 保存编译期输出结果的RoseEngine对象，必须非空。 | 输入 |
| `scratch` | 本次扫描使用的临时内存空间，必须非空。 | 输入 |

返回匹配结果错误码。

## 修订记录

| 文档版本 | 发布日期 | 修改说明 |
| --- | --- | --- |
| 03 | 2026-09-30 | 第三次正式发布（V5.8.0）：新增mcsheng算法性能优化和正则匹配反馈优化技术。 |
| 02 | 2026-06-30 | 第二次正式发布：新增通用字节码功能。 |
| 01 | 2026-03-30 | 第一次正式发布：优化Ultrascan 2~4字节短规则匹配算法，新增`KHSEL_BuildLilyForTeddy`、`KHSEL_LilyForTeddyRunExec`。 |
