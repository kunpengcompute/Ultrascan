# 版本说明书<a name="ZH-CN_TOPIC_0000002518419160"></a>

## 修改记录<a name="ZH-CN_TOPIC_0000002549904611"></a>

|文档版本|发布日期|修改说明|
|--|--|--|
|02|2026-06-30|第二次正式发布，基于鲲鹏920新型号处理器新增通用字节码功能。|
|01|2026-03-30|第一次正式发布，基于鲲鹏920新型号处理器优化Ultrascan 2~4字节短字节规则匹配算法，新增KHSEL_BuildLilyForTeddy、KHSEL_LilyForTeddyRunExec算法。|

## 版本配套说明<a name="ZH-CN_TOPIC_0000002518405392"></a>

### 产品版本信息<a name="ZH-CN_TOPIC_0000002518405384"></a>

<a name="table62675726"></a>
<table><tbody><tr id="row41561572"><th class="firstcol" valign="top" width="42.17%" id="mcps1.1.3.1.1"><p id="p11044137"><a name="p11044137"></a><a name="p11044137"></a>产品名称</p>
</th>
<td class="cellrowborder" valign="top" width="57.830000000000005%" headers="mcps1.1.3.1.1 "><p id="p1597721693713"><a name="p1597721693713"></a><a name="p1597721693713"></a>Kunpeng BoostKit</p>
</td>
</tr>
<tr id="row24726251"><th class="firstcol" valign="top" width="42.17%" id="mcps1.1.3.2.1"><p id="p56669300"><a name="p56669300"></a><a name="p56669300"></a>产品版本</p>
</th>
<td class="cellrowborder" valign="top" width="57.830000000000005%" headers="mcps1.1.3.2.1 "><p id="p11923034"><a name="p11923034"></a><a name="p11923034"></a><span id="text152431189308"><a name="text152431189308"></a><a name="text152431189308"></a>26.1.RC1</span></p>
</td>
</tr>
<tr id="row1930811171892"><th class="firstcol" valign="top" width="42.17%" id="mcps1.1.3.3.1"><p id="p2030912172097"><a name="p2030912172097"></a><a name="p2030912172097"></a>软件名称</p>
</th>
<td class="cellrowborder" valign="top" width="57.830000000000005%" headers="mcps1.1.3.3.1 "><p id="p1730912179911"><a name="p1730912179911"></a><a name="p1730912179911"></a>Ultrascan</p>
</td>
</tr>
<tr id="row5497143514612"><th class="firstcol" valign="top" width="42.17%" id="mcps1.1.3.4.1"><p id="p162251517551"><a name="p162251517551"></a><a name="p162251517551"></a>软件版本</p>
</th>
<td class="cellrowborder" valign="top" width="57.830000000000005%" headers="mcps1.1.3.4.1 "><p id="p6225131165519"><a name="p6225131165519"></a><a name="p6225131165519"></a>5.7.0</p>
</td>
</tr>
</tbody>
</table>

### 与操作系统、编译器和CPU配套说明<a name="ZH-CN_TOPIC_0000002518245458"></a>

|操作系统|CPU类型|编译器|
|--|--|--|
|openEuler 22.03 LTS SP4|鲲鹏920系列处理器|GCC 10.3.1|
|openEuler 24.03 LTS SP3|鲲鹏920系列处理器|GCC 12.3.1|

### 病毒扫描结果<a name="ZH-CN_TOPIC_0000002549885239"></a>

本特性以源码的形式发布，不涉及软件包，因此暂不需要病毒扫描。

## V5.7.0

### 更新说明

**新增特性**

|特性描述| 更新说明                                     |
|--|------------------------------------------|
|通用字节码特性| 基于鲲鹏920新型号处理器新增通用字节码功能。  |

## V2.6.0

### 更新说明

**新增特性**

|特性描述| 更新说明                                     |
|--|------------------------------------------|
|短规则旁路技术| 基于鲲鹏920新型号处理器优化Ultrascan 2~4字节短字节规则匹配算法。  |

## V2.5.3<a name="ZH-CN_TOPIC_0000002518245468"></a>

### 更新说明<a name="ZH-CN_TOPIC_0000002549765235"></a>

**新增特性<a name="section7266645154314"></a>**

无

**修改特性<a name="section16177195184414"></a>**

|特性描述|更新说明|
|--|--|
|KHSEL|优化Ultrascan多模匹配算法。优化Rose解释器后端长字符串校验。增加短规则旁路算法开关。|

**删除特性<a name="section17862111013445"></a>**

无

### 已解决的问题<a name="ZH-CN_TOPIC_0000002518405386"></a>

无

### 遗留问题<a name="ZH-CN_TOPIC_0000002518245472"></a>

无

## V2.5.1<a name="ZH-CN_TOPIC_0000002518405390"></a>

### 更新说明<a name="ZH-CN_TOPIC_0000002549765229"></a>

**新增特性<a name="section11862975"></a>**

|特性描述|更新说明|
|--|--|
|KHSEL|实现大数据Flink的replaceAll功能函数C版本算法优化，性能对比Java版本replaceAll提升2倍。基于鲲鹏920新型号处理器优化Ultrascan短字节规则匹配算法，性能领先20%。|

**修改特性<a name="section16450949161512"></a>**

无

**删除特性<a name="section9218125814159"></a>**

无

### 已解决的问题<a name="ZH-CN_TOPIC_0000002549765223"></a>

无

### 遗留问题<a name="ZH-CN_TOPIC_0000002549885237"></a>

无

## V2.4.0<a name="ZH-CN_TOPIC_0000002549885233"></a>

### 更新说明<a name="ZH-CN_TOPIC_0000002549765225"></a>

**新增特性<a name="section11862975"></a>**

|特性描述|更新说明|
|--|--|
|KHSEL|新增基于开源Ultrascan的算法优化。|

**修改特性<a name="section16450949161512"></a>**

无

**删除特性<a name="section9218125814159"></a>**

无

### 已解决的问题<a name="ZH-CN_TOPIC_0000002518245460"></a>

无

### 遗留问题<a name="ZH-CN_TOPIC_0000002518245464"></a>

无

## 版本配套文档<a name="ZH-CN_TOPIC_0000002547130797"></a>

### 版本配套文档<a name="ZH-CN_TOPIC_0000002547210821"></a>

<table>
  <thead>
    <tr>
      <th style="text-align: left;">文档名称</th>
      <th style="text-align: left;">内容简介</th>
      <th style="text-align: left;">交付形式</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td style="text-align: left;">版本说明书</td>
      <td style="text-align: left;">提供Ultrascan的版本发布信息。</td>
      <td style="text-align: left;">开源仓</td>
    </tr>
    <tr>
      <td style="text-align: left;">快速入门</td>
      <td style="text-align: left;">提供Ultrascan的快速上手教程，帮助用户快速了解和使用Ultrascan。</td>
      <td style="text-align: left;">开源仓</td>
    </tr>
    <tr>
      <td style="text-align: left;">安装指南</td>
      <td style="text-align: left;">提供Ultrascan的安装部署指导。</td>
      <td style="text-align: left;">开源仓</td>
    </tr>
    <tr>
      <td style="text-align: left;">使用指南</td>
      <td style="text-align: left;">提供Ultrascan的使用操作指导。</td>
      <td style="text-align: left;">开源仓</td>
    </tr>
    <tr>
      <td style="text-align: left;">开发指南</td>
      <td style="text-align: left;">提供Ultrascan的开发指导。</td>
      <td style="text-align: left;">开源仓</td>
    </tr>
  </tbody>
</table>

### 获取文档的方法<a name="ZH-CN_TOPIC_0000002547210757"></a>

您可以通过访问[开源仓](https://gitcode.com/boostkit/Ultrascan/tree/master)浏览和获取相关文档。
