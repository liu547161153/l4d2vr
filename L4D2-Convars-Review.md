# L4D2 Convars 说明整理

基于 Valve Developer Community 当前页面整理：

- L4D2 专页：`List of Left 4 Dead 2 console commands and variables`
- Flags 专页：`Console command flags`
- 通用 Source 专页：`Console Command List`
- L4D1 专页：`List of Left 4 Dead console commands and variables`

## Flags 速查

| 标签 | 含义 |
| --- | --- |
| `cl` | 客户端变量，本地渲染/表现为主 |
| `sv` | 服务端变量，来自 `server.dll` |
| `cheat` | 多人里通常需要 `sv_cheats 1` |
| `rep` | 服务端值会同步给客户端，客户端本地改动可能被覆盖 |
| `a` | 会写入 `config.cfg`，重启后保留 |
| `launcher` | 更偏启动期/加载期变量，常见于启动参数或加载前生效 |
| `demo` | 录 demo 时会记录这个变量 |

## 原始脚本里的问题

- `Convars.SetValue("r_staticprop_lod " "4");`
  这行少了逗号，变量名尾部还带空格。
- `Convars.SetValue("r_lod " "2");`
  这行也少了逗号，变量名尾部还带空格。
- `con_filter_text_out` 连续写 3 次不是叠加，最后通常只剩最后一次的值，也就是 `particle`。
- `snd_mixahead` 写了两次，最终会以最后一行 `0.1` 为准。
- `cl_maxrenderable_dist` 不在 L4D2 专页里，我是用通用 Source 命令页补的。
- `survivor_draw_addons` 不在 L4D2 专页里，只能在 L4D1 页确认到。

## AI、面部、口型

| 变量 | 设值 | 默认 | Flags | 含义 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `ai_expression_frametime` | `0` | `0.05` | `sv, launcher` | 背景表情仍允许播放的最大帧时间 | 设 `0` 基本就是尽量不播背景表情 |
| `ai_expression_optimization` | `1` | `0` | `sv, launcher` | 看不见 NPC 时停用背景表情 | 明确的省 CPU 项 |
| `flex_rules` | `0` | `1` | `cl, launcher` | 是否执行面部 flex 动画规则 | 关掉后表情会更木 |
| `r_flex` | `0` | `1` | `launcher` | 面部 flex 渲染总开关 | 和 `flex_rules` 目标一致 |
| `phonemedelay` | `-30` | `0` | `cl, launcher` | 口型延迟补偿 | 负值很激进，可能让口型对不上 |
| `phonemefilter` | `0.01` | `0.08` | `cl, launcher` | 口型滤波时长 | 更小更生硬 |
| `phonemesnap` | `0` | `2` | `cl, launcher` | viseme/口型 LOD 切换阈值 | 更偏粗糙口型 |
| `r_jiggle_bones` | `0` | `1` | `cl, launcher` | 抖动骨骼效果 | 头发、挂件、部分细小骨骼会更僵 |

## 客户端预测、控制台、网络

| 变量 | 设值 | 默认 | Flags | 含义 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `cl_pred_optimize` | `1` | `2` | `cl, launcher` | 客户端预测优化等级 | 从 `2` 降到 `1`，不一定更快，只是路径不同 |
| `cl_smooth` | `0` | `1` | `cl, launcher` | 预测误差后的视角平滑 | 关掉后更“硬”，可能更抖 |
| `con_filter_text_out` | `materials/models/particle` | `""` | `launcher` | 过滤掉含特定文本的控制台输出 | 这三行不是累加，通常只剩最后一条 |
| `net_compresspackets` | `0` | `1` | `launcher` | 是否对游戏包做压缩 | 省 CPU，但增带宽 |
| `net_splitrate` | `1` | `1` | 无 | 每帧可发送的 splitpacket 分片数 | 这里其实没改到默认值 |
| `fps_max` | `999` | `300` | 无 | 帧率上限 | 不是省性能，而是放开上限 |

## 命中特效、碎片、控制台噪音

| 变量 | 设值 | 默认 | Flags | 含义 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `cl_ejectbrass` | `0` | `1` | `cl, launcher` | 是否显示弹壳抛出 | 常见省特效项 |
| `cl_impacteffects_limit_exit` | `3` | `3` | `cl, launcher` | 每帧出口类 impact 特效上限 | 没改到默认值 |
| `cl_impacteffects_limit_general` | `3` | `10` | `cl, launcher` | 每帧普通 impact 特效上限 | 明显减特效 |
| `cl_impacteffects_limit_water` | `3` | `2` | `cl, launcher` | 每帧水面 impact 特效上限 | 这里反而比默认更高 |
| `cl_new_impact_effects` | `0` | `1` | `cl, launcher` | 新版命中特效路径 | 关掉通常是退回更旧/更便宜的效果 |
| `func_break_max_pieces` | `3` | `15` | `a, rep, cl` | `func_breakable` 最大碎片数 | `rep`，联机里可能受服务器值影响 |
| `props_break_max_pieces` | `3` | `50` | `rep, cl, launcher` | 可破坏 prop 最大碎片数 | 明显减负载 |
| `props_break_max_pieces_perframe` | `3` | `-1` | `rep, cl, launcher` | 每帧可生成的 prop 碎片数 | 从“模型默认”改成强限流 |

## 材质、DX 等级、后处理

| 变量 | 设值 | 默认 | Flags | 含义 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `mat_dxlevel` | `81` | `92` | `launcher` | DirectX 功能等级 | 老派低画质手段，可能影响兼容和观感 |
| `mat_force_vertexfog` | `0` | `1` | `launcher` | 强制使用顶点雾 | 官方页无描述；按名字看是雾计算路径开关 |
| `mat_forcehardwaresync` | `0` | `1` | `launcher` | 强制 CPU/GPU 同步 | 官方页无描述；常见于同步策略切换 |
| `mat_framebuffercopyoverlaysize` | `0` | `128` | `cl, launcher` | framebuffer copy overlay 尺寸 | 设 `0` 偏向砍相关拷贝/覆盖层 |
| `mat_max_worldmesh_vertices` | `2` | `65536` | `launcher` | world mesh 顶点上限 | 这个值极端，可疑，可能带来异常 |
| `mat_postprocess_x` | `0` | `4` | `cl, launcher` | 后处理相关参数 X | 官方页无描述，偏内部 |
| `mat_postprocess_y` | `0` | `1` | `cl, launcher` | 后处理相关参数 Y | 官方页无描述，偏内部 |
| `mat_reducefillrate` | `1` | `0` | `launcher` | 降低填充率开销 | 明确为低画质项 |
| `mat_reduceparticles` | `1` | `0` | `launcher` | 减少粒子 | 明确为低画质项 |
| `mat_software_aa_blur_one_pixel_lines` | `0` | `0.5` | `cl, launcher` | 软件 AA 对 1 像素线条的模糊强度 | 设 `0` 更锐但更锯齿 |
| `mat_software_aa_edge_threshold` | `9` | `1.0` | `cl, launcher` | 软件 AA 边缘检测阈值 | 设很高通常意味着更少边缘被处理 |
| `mat_software_aa_quality` | `0` | `0` | `cl, launcher` | 软件 AA 质量模式 | 没改到默认值 |
| `mat_software_aa_strength` | `0` | `-1.0` | `cl, launcher` | 软件 AA 强度 | `0` 等于关 |
| `mat_software_aa_strength_vgui` | `0` | `-1.0` | `cl, launcher` | VGUI pass 的软件 AA 强度 | `0` 等于关 |
| `mat_software_aa_tap_offset` | `0` | `1.0` | `cl, launcher` | 软件 AA 采样偏移 | 设 `0` 很激进 |

## 异步加载、Cubemap、雾

| 变量 | 设值 | 默认 | Flags | 含义 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `building_cubemaps` | `0` | `0` | `launcher` | 是否处在构建 cubemap 状态 | 更像内部状态，不像常规优化项 |
| `fast_fogvolume` | `1` | `0` | `launcher` | 使用更快的 fog volume 路径 | 官方页无描述；按名字推断 |
| `mod_load_anims_async` | `0` | `0` | `launcher` | 模型动画异步加载 | 没改到默认值 |
| `mod_load_mesh_async` | `0` | `0` | `launcher` | 模型网格异步加载 | 没改到默认值 |
| `mod_load_vcollide_async` | `0` | `0` | `launcher` | 碰撞数据异步加载 | 没改到默认值 |

## 场景、LOD、可见距离

| 变量 | 设值 | 默认 | Flags | 含义 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `cl_maxrenderable_dist` | `2000` | `3000` | `cheat` | 摄像机外物体最远渲染距离 | 来自通用 Source 页，L4D2 专页未列出 |
| `r_3dsky` | `0` | `1` | `cl, launcher` | 是否渲染 3D skybox | 经典省性能项 |
| `r_DrawDetailProps` | `0` | `1` | `cl, launcher` | 细节物件显示 | `0=关` |
| `r_lod` | `2` | `-1` | `launcher` | 模型 LOD 级别 | 你的原始脚本这行有语法错误 |
| `r_rootlod` | `2` | `0` | `launcher` | 根 LOD | 越大越糊，越省 |
| `r_staticprop_lod` | `4` | `-1` | `launcher` | 静态 prop LOD | 你的原始脚本这行也有语法错误 |
| `r_occludermincount` | `2` | `0` | `launcher` | 至少使用这么多个遮挡体 | 不一定更快，依场景而定 |

## 光照、阴影、水面

| 变量 | 设值 | 默认 | Flags | 含义 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `r_WaterDrawReflection` | `0` | `1` | `cl, launcher` | 水面反射 | 明确省 GPU |
| `r_WaterDrawRefraction` | `0` | `1` | `cl, launcher` | 水面折射 | 明确省 GPU |
| `r_ambientboost` | `0` | `1` | `launcher` | 环境光补偿增强 | 关掉更暗更省 |
| `r_ambientfactor` | `0` | `5` | `launcher` | 环境光增强上限 | 配合上面一起砍 |
| `r_ambientfraction` | `0` | `0.2` | `cheat` | 直接光用于补环境光的比例 | `cheat` 项 |
| `r_ambientmin` | `0` | `0.3` | `launcher` | 环境光不再提升的亮度阈值 | 也是偏光照修正项 |
| `r_dynamic` | `0` | `1` | 无 | 动态光 | 经典省性能项 |
| `r_flashlightmuzzleflash` | `0` | `1` | `cl, launcher` | 手电/枪口相关闪光 | 官方页无描述；按名字推断 |
| `r_lightaverage` | `0` | `1` | `launcher` | 平均光照计算 | 官方页明确是 light averaging |
| `r_maxdlights` | `0` | `32` | `launcher` | 最大动态灯数 | 明确省性能项 |
| `r_maxnewsamples` | `0` | `6` | `launcher` | 光照/采样相关上限 | 官方页无描述，偏内部 |
| `r_maxsampledist` | `""` | `128` | `launcher` | 最大采样距离 | 你这里给了空字符串，可疑 |
| `r_PhysPropStaticLighting` | `0` | `0` | `cl` | 物理 prop 静态光照 | 这里其实没改到默认值 |
| `r_pixelfog` | `0` | `1` | `launcher` | 像素雾 | 关掉偏低画质 |
| `r_pixelvisibility_partial` | `0` | `1` | `cl, launcher` | 部分像素可见性计算 | 官方页无描述，偏内部 |
| `r_shadowfromworldlights` | `0` | `1` | `cl, launcher` | 世界光源投影 | 关掉省阴影开销 |
| `r_shadowlod` | `-2` | `-1` | `launcher` | 阴影 LOD | 更激进的阴影降级 |
| `r_shadowmaxrendered` | `0` | `64` | `cl, launcher` | 最多渲染阴影数量 | `0` 基本是全砍 |
| `r_shadowrendertotexture` | `0` | `1` | `launcher` | 阴影贴图渲染 | 关掉省性能 |
| `r_shadows_on_renderables_enable` | `0` | `0` | `cl, launcher` | 渲染对象间 RTT 阴影 | 这里也没改到默认值 |
| `r_worldlights` | `0` | `2` | `launcher` | 每顶点使用的世界光数量 | 也是常见低画质项 |
| `z_infected_shadows` | `0` | `1` | `cl, launcher` | 感染者阴影 | 明确省性能 |
| `z_mob_simple_shadows` | `2` | `0` | `cl, launcher` | 尸群阴影质量 | 官方说明：`0` 全阴影，`1` 简单阴影，`2` 无阴影 |

## Rope、实体、附加渲染

| 变量 | 设值 | 默认 | Flags | 含义 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `r_entityclips` | `0` | `1` | `cl, launcher` | 实体裁剪相关 | 官方页无描述，偏内部 |
| `r_glint_procedural` | `1` | `0` | `launcher` | 程序化 glint 效果 | 这个不像省性能项，反而可疑 |
| `r_renderoverlayfragment` | `0` | `1` | `launcher` | overlay fragment 渲染 | 官方页无描述，偏内部 |
| `r_ropetranslucent` | `0` | `1` | `cl, launcher` | 绳索半透明渲染 | 关掉更便宜 |
| `rope_averagelight` | `0` | `1` | `cl, launcher` | 绳索使用平均 cubemap 光照 | 低成本但更糙 |
| `rope_collide` | `0` | `1` | `cl, launcher` | 绳索与世界碰撞 | 明确省 CPU |
| `rope_rendersolid` | `0` | `1` | `cl, launcher` | 绳索实体部分渲染 | 关掉更便宜 |
| `rope_shake` | `0` | `0` | `cl, launcher` | 绳索抖动 | 没改到默认值 |
| `rope_smooth` | `0` | `1` | `cl, launcher` | 绳索抗锯齿/平滑 | 明显降画质 |
| `rope_subdiv` | `0` | `2` | `cl, launcher` | 绳索细分 | `0` 很省，但观感差 |
| `rope_wind_dist` | `0` | `1000` | `cl, launcher` | 超过此距离不再给绳索加小风效 | 设 `0` 基本全禁 |

## 调试、Overlay、统计

| 变量 | 设值 | 默认 | Flags | 含义 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `r_draw_flashlight_3rd_person` | `0` | `1` | `cl, launcher` | 是否绘制其他玩家手电光束 | 联机视觉会更少 |
| `r_drawmodelstatsoverlaydistance` | `0` | `500` | `cheat` | 模型统计 overlay 的显示距离 | 调试项 |
| `r_drawmodelstatsoverlaymax` | `0` | `1.5` | `a` | overlay 变成全红的耗时阈值 | 调试项 |
| `r_drawmodelstatsoverlaymin` | `0` | `0.1` | `a` | 触发 overlay 的最小时长阈值 | 调试项 |

## 音频

| 变量 | 设值 | 默认 | Flags | 含义 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `dsp_slow_cpu` | `1` | `0` | `a, demo` | 慢 CPU 音频路径 | 常见低音质/低负载项 |
| `snd_delay_sound_shift` | `0.03` | `0.03` | `launcher` | 声音延迟补偿偏移 | 没改到默认值 |
| `snd_mix_async` | `0` | `0` | `launcher` | 异步混音 | 没改到默认值 |
| `snd_mixahead` | `0.048 -> 0.1` | `0.1` | `a` | 预混音前瞻时间 | 最终还是默认值 `0.1` |
| `snd_pitchquality` | `0` | `1` | `a` | 音高变换质量 | 降到最低 |

## 特殊项

| 变量 | 设值 | 默认 | Flags | 含义 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `smoothstairs` | `0` | `1` | `rep, cl, launcher` | 上下楼梯时平滑眼位高度 | 关掉后镜头更生硬 |
| `survivor_draw_addons` | `0` | `2` | `cl, launcher` | `0=none, 1=guns, 2=all` | 这是 L4D1 条目，L4D2 专页没有它 |

## 总评

- 这份配置里大多数条目确实是“低画质/低特效”思路。
- 真正能明确归类为 `sv` 或 `cheat` 的不多，远少于 `cl` 和 `launcher`。
- 有几项属于“老 Source 时代经验参数”或内部参数，未必真的有效，甚至可能副作用更大：
  - `mat_max_worldmesh_vertices 2`
  - `r_glint_procedural 1`
  - `r_occludermincount 2`
  - `r_maxsampledist ""`
  - `mat_dxlevel 81`
- 如果目标是做一份更稳妥的 L4D2 VR 性能配置，建议下一步把它分成三类：
  - 明确有效且低风险
  - 可疑/重复/语法错误
  - 可能只在旧分支、启动期或特定场景有效

## 来源

- https://developer.valvesoftware.com/wiki/List_of_Left_4_Dead_2_console_commands_and_variables
- https://developer.valvesoftware.com/wiki/Console_command_flags
- https://developer.valvesoftware.com/wiki/Console_Command_List
- https://developer.valvesoftware.com/wiki/List_of_Left_4_Dead_console_commands_and_variables
