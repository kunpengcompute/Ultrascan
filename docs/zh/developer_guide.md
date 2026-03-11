# 开发指南<a name="ZH-CN_TOPIC_0000002549765393"></a>

## 函数说明<a name="ZH-CN_TOPIC_0000002549885353"></a>

>![](public_sys-resources/icon-notice.gif) **须知：** 
>KHSEL中的函数（KHSEL\_xxx）无需被显式调用，它们作为Hyperscan的内部接口使用。

KHSEL库已优化函数如[**表 1** KHSEL库已优化函数](#KHSEL库已优化函数)所示。

**表 1** KHSEL库已优化函数<a id="KHSEL库已优化函数"></a>

|名称|说明|
|--|--|
|KHSEL_BuildLily|Hyperscan新增单字节短规则匹配的编译函数。|
|KHSEL_LilyRunExec|Hyperscan新增单字节短规则匹配的运行函数。|
|KHSEL_BuildLilyForTeddy|Hyperscan新增2~4字节短规则匹配的编译函数。|
|KHSEL_LilyForTeddyRunExec|Hyperscan新增2~4字节短规则匹配的运行函数。|

## 使用说明<a name="ZH-CN_TOPIC_0000002518405558"></a>

KHSEL函数源码已集成到Hyperscan仓库dev分支中，位于`src\kunpeng-enhanced`目录下，不再需要单独安装KHSEL相关软件包。

## 函数定义<a name="ZH-CN_TOPIC_0000002549885351"></a>

### KHSEL\_BuildLily<a name="ZH-CN_TOPIC_0000002518245592"></a>

**函数功能<a name="section95941732195012"></a>**

基于规则集内的单字节短规则进行规则编译，输出编译后的mask。

**函数定义<a name="section1183110404506"></a>**

```c
std::vector<u8> KHSEL_BuildLily(std::map<char, lilyReport> &lily, std::vector<u32> &reportVec, std::vector<u32> &ekeyVec); 
```

**参数说明<a name="section1192224915509"></a>**

|参数名|描述|取值范围|输入/输出|
|--|--|--|--|
|lily|短字节规则。|C++ map对象，无非空限制|输入|
|reportVec|短字节规则对应的reportID。|C++ vector对象，无非空限制|输入|
|ekeyVec|短字节规则对应的ekey。|C++ vector对象，无非空限制|输入|

**返回值<a name="section13615359181110"></a>**

规则编译后的输出。

### KHSEL\_LilyRunExec<a name="ZH-CN_TOPIC_0000002518245632"></a>

**函数功能<a name="section95941732195012"></a>**

基于编译输出的mask，进行运行期输入数据的匹配。

**函数定义<a name="section1183110404506"></a>**

```c
hs_error_t KHSEL_LilyRunExec(const struct RoseEngine *rose, hs_scratch_t *scratch); 
```

**参数说明<a name="section1192224915509"></a>**

|参数名|描述|取值范围|输入/输出|
|--|--|--|--|
|rose|RoseEngine对象，保存编译期输出结果，作为运行期输入。|非空|输入|
|scratch|输入数据所需的临时内存空间。|非空|输入|

**返回值<a name="section13615359181110"></a>**

匹配结果的错误码。

### KHSEL\_BuildLilyForTeddy

**函数功能**

基于规则集内的2~4字节短规则进行规则编译，输出编译后的mask。

**函数定义**

```c
ue2::bytecode_ptr<lilyTeddy> KHSEL_BuildLilyForTeddy(std::map<std::string, lilyReport> &lilyForTeddy,
                                        std::priority_queue<LilyForTeddyPair, std::vector<LilyForTeddyPair>, CompareStringLength> &lilyForTeddyPQ,
                                        std::vector<u32> &reportVec, std::vector<u32> &ekeyVec, std::vector<u32> &lenVec);
```

**参数说明**

|参数名| 描述                   | 取值范围                       |输入/输出|
|--|----------------------|----------------------------|--|
|lilyForTeddy| 2~4字节规则。             | C++ map对象，无非空限制            |输入|
|lilyForTeddyPQ| 2~4字节规则依规则长度排序的优先队列。 | C++ priority_queue对象，无非空限制 |输入|
|reportVec| 2~4字节规则对应的reportID。  | C++ vector对象，无非空限制         |输入|
|ekeyVec| 2~4字节规则对应的ekey。      | C++ vector对象，无非空限制         |输入|
|lenVec| 2~4字节规则对应的规则长度。      | C++ vector对象，无非空限制         |输入|

**返回值**

规则编译后的输出。

### KHSEL\_LilyForTeddyRunExec

**函数功能**

基于编译输出的mask，进行运行期输入数据的匹配。

**函数定义**

```c
hs_error_t KHSEL_LilyForTeddyRunExec(const struct RoseEngine *rose, hs_scratch_t *scratch);
```

**参数说明**

|参数名|描述|取值范围|输入/输出|
|--|--|--|--|
|rose|RoseEngine对象，保存编译期输出结果，作为运行期输入。|非空|输入|
|scratch|输入数据所需的临时内存空间。|非空|输入|

**返回值**

匹配结果的错误码。

## 修订记录

|文档版本|发布日期|修改说明|
|--|--|--|
|02|2026-03-30|第二次正式发布，基于鲲鹏920新型号处理器优化Hyperscan 2~4字节短字节规则匹配算法，新增KHSEL_BuildLilyForTeddy、KHSEL_LilyForTeddyRunExec算法。|
|01|2025-12-30|第一次正式发布。|
