# PS4 / PlayStation VR 游戏清单：热度、体积与分类

> 数据日期：2026-09-05
> 完整逐条数据见 [`psvr-games-list.csv`](psvr-games-list.csv)（527 行，16 列，按热度降序）。

## 1. 数据来源与口径

| 项目 | 来源 | 覆盖 |
|---|---|---|
| 游戏清单 | [Wikipedia: List of PlayStation VR games](https://en.wikipedia.org/wiki/List_of_PlayStation_VR_games) | 527/527 |
| 外设标记 | 同上（Addons 列：Move / Aim / HOTAS / BD） | 527/527 |
| 热度 | [Wikimedia Pageviews API](https://wikimedia.org/api/rest_v1/) 英文维基月均浏览量 | 123/527（23%） |
| 包体积 | [NoPayStation](https://nopaystation.com/) `PS4_GAMES.tsv` | 62/527（12%） |

### 热度（★）怎么算的

**取 PSVR 生命周期窗口 2016-10 ~ 2021-12 的月均浏览量**，而不是近期数据。这一点关键：Beat Saber 当年 15,651/mo，近 24 个月只剩 3,368/mo——因为它的主战场早已转到 Quest/PCVR。用近期数据会把所有 PSVR 游戏的历史地位一起抹平。10 条 2021 年后才建立维基条目的游戏回退到近期窗口。

两类系统性偏差做了折算，判据尽量客观：

| 情形 | 判据 | 系数 | 例 |
|---|---|---|---|
| **IP 本体污染** | 维基条目名 ≠ 游戏名，说明条目讲的是 IP / 系列 / 剧集 | **×0.10** | `Dreamworks Voltron VR Chronicles` → 条目实为动画剧集《Voltron: Legendary Defender》；`NBA 2K: VR Experience` → 整个 2K 系列 |
| **跨平台大作** | 条目即本作，但游戏本身跨平台，PSVR 只是其中一个模式 | **×0.30** | Minecraft、Skyrim、RE7、Tekken 7 |

IP 本体污染是**自动检测**的（标题归一化后比对条目名），共命中 20 条；跨平台名单 22 条为人工判定。表中分别标 ⁱᵖ 和 ˣᵖ。

星级阈值（对折算后分数）：★★★★★ ≥ 8000，★★★★ ≥ 2500，★★★ ≥ 700，★★ ≥ 150，★ < 150。无维基条目者：有实体碟版 = ★★，否则 ★。

| 星级 | 数量 | 占比 |
|---|---|---|
| ★★★★★ | 9 | 2% |
| ★★★★ | 31 | 6% |
| ★★★ | 31 | 6% |
| ★★ | 103 | 20% |
| ★ | 353 | 67% |

**这是"知名度"而非"销量"或"质量"**。维基浏览量偏向英语圈与话题性，日系小品和 F2P 会被低估。404 款（77%）没有维基条目，只能靠实体碟发行与否二分——它们落在 ★★/★ 是合理近似，但不精确。

### 体积的三个限制

1. **只有 62/527（12%）有精确数字**。`Astro Bot Rescue Mission`、`Farpoint`、`Blood & Truth`、`Marvel's Iron Man VR` 等重要独占都不在 NoPayStation 索引中。
2. **是首发基础包，不含补丁与 DLC**。Beat Saber 记录 0.29 GB，实际装机加歌曲包远不止。表中数字是**下限**。
3. 已剔除 4 条经复核确认的误匹配（`Sniper Elite VR`→Sniper Elite 4、`Project DIVA X`→DEMO 包、两款 Persona Dancing 串位）。

搜索引擎上的 PSVR 体积数据混淆严重（「Astro Bot 66 GB」实为 2024 年 PS5 版《Astro Bot》，与 PSVR 版《Astro Bot Rescue Mission》无关），故只采用可核验的结构化来源。

## 2. 总览

| 分类 | 数量 |
|---|---|
| 条目总数 | **527** |
| 付费 / 免费 | 480 / 47 |
| **Move 必需**（纯 VR 强信号） | **165** |
| Move 可选 | 139 |
| 无 Move 标记（手柄 / Aim / HOTAS） | 223 |
| Aim controller / HOTAS | 24 / 5 |
| 有 Blu-ray 实体版 | 154 |

Wikipedia 不区分 VR-only 与 VR-optional，本文用 **Move 必需** 作为纯 VR 的强信号——但这是近似：`Astro Bot`、`Farpoint` 用手柄/Aim 而非 Move，同样是纯 VR。精确区分需逐条查 PS Store 的 "PS VR required" 标记。

### 按发行年份

| 年份 | 数量 |
|---|---|
| 2016 | 80 |
| 2017 | 143 |
| 2018 | 160 |
| 2019 | 102 |
| 2020 | 27 |
| 2021 | 4 |
| 2022 | 1 |
| 2023 | 1 |

2016–2018 是绝对主力（383 款，73%）；2019 年后 Sony 重心转向 PS5，2021 年后仅剩零星发行。

## 3. 热度排行 TOP 60

ⁱᵖ = IP 本体污染已折算 ×0.1；ˣᵖ = 跨平台大作已折算 ×0.3。「当年月均」为折算**前**的原始浏览量。

| # | 热度 | 标题 | 开发商 | 年份 | Move | GB | 当年月均 |
|---:|:---|---|---|---:|:---:|---:|---:|
| 1 | ★★★★★ | Minecraftˣᵖ | Mojang | — | — | 0.21 | 280,191 |
| 2 | ★★★★★ | Resident Evil 7: Biohazardˣᵖ | Capcom | 2017 | — | 20.64 | 132,886 |
| 3 | ★★★★★ | The Elder Scrolls V: Skyrimˣᵖ | Bethesda Game Studios | 2017 | 可选 | — | 97,659 |
| 4 | ★★★★★ | Tekken 7ˣᵖ | Bandai Namco Ent. | 2017 | — | 38.74 | 56,598 |
| 5 | ★★★★★ | The Inpatient | Supermassive Games | 2018 | 可选 | — | 15,946 |
| 6 | ★★★★★ | Beat Saber | Beat Games | 2018 | **必需** | 0.29 | 15,651 |
| 7 | ★★★★★ | Star Wars: Squadronsˣᵖ | Motive Studios | 2020 | — | — | 45,566 |
| 8 | ★★★★★ | Gran Turismo Sportˣᵖ | Polyphony Digital | 2017 | — | 40.04 | 30,722 |
| 9 | ★★★★★ | Déraciné | FromSoftware | 2018 | **必需** | — | 8,183 |
| 10 | ★★★★ | Persona 5: Dancing in Starlight | P-Studio | 2018 | — | — | 7,869 |
| 11 | ★★★★ | Until Dawn: Rush of Blood | Supermassive Games | 2016 | 可选 | 12.16 | 7,466 |
| 12 | ★★★★ | Rise of the Tomb Raider: Blood Tiesⁱᵖ | Crystal Dynamics | 2016 | — | — | 61,092 |
| 13 | ★★★★ | Batman: Arkham VR | Rocksteady Studios | 2016 | 可选 | 7.99 | 6,030 |
| 14 | ★★★★ | Dreamworks Voltron VR Chroniclesⁱᵖ | Digital Domain Interactive | 2017 | — | — | 53,725 |
| 15 | ★★★★ | No Man's Sky Beyondⁱᵖ | Hello Games | 2019 | 可选 | — | 52,963 |
| 16 | ★★★★ | Star Trek: Bridge Crew | Red Storm Entertainment | 2017 | 可选 | — | 4,992 |
| 17 | ★★★★ | Astro Bot Rescue Mission | Japan Studio (Team Asobi) | 2018 | — | — | 4,508 |
| 18 | ★★★★ | Five Nights at Freddy's: Help Wantedˣᵖ | Steel Wool | 2019 | 可选 | — | 14,460 |
| 19 | ★★★★ | Dead or Alive Xtreme 3ˣᵖ | Koei Tecmo | 2017 | — | — | 14,291 |
| 20 | ★★★★ | Wolfenstein: Cyberpilot |  | 2019 | 可选 | — | 4,131 |
| 21 | ★★★★ | Sega Genesis Classicsˣᵖ | Sega | 2018 | — | — | 13,671 |
| 22 | ★★★★ | Fate/Grand Order VR feat. Mashu Kyrielightⁱᵖ | Aniplex | 2017 | — | — | 40,904 |
| 23 | ★★★★ | Psychonauts in the Rhombus of Ruin | Double Fine Productions | 2017 | — | — | 3,994 |
| 24 | ★★★★ | Hatsune Miku: Project DIVA X | Sega | 2016 | 可选 | — | 3,914 |
| 25 | ★★★★ | Blood & Truth | London Studio | 2019 | 可选 | — | 3,872 |
| 26 | ★★★★ | Job Simulator | Owlchemy Labs | 2016 | **必需** | 0.90 | 3,850 |
| 27 | ★★★★ | The Last Guardian VR Demoⁱᵖ | Sony Interactive Entertainment | 2017 | — | 1.23 | 38,171 |
| 28 | ★★★★ | Persona 3: Dancing in Moonlight | P-Studio | 2018 | — | — | 3,751 |
| 29 | ★★★★ | Rec Room | Against Gravity | 2017 | **必需** | 1.79 | 3,721 |
| 30 | ★★★★ | Creed: Rise to Glory | Survios | 2018 | **必需** | — | 3,426 |
| 31 | ★★★★ | Eve: Valkyrie | CCP Games | 2016 | — | 5.46 | 3,200 |
| 32 | ★★★★ | Disaster Report 4 Plus: Summer Memories | Granzella | 2018 | — | — | 2,913 |
| 33 | ★★★★ | Accounting+ | Squanch Games | 2017 | 可选 | 0.60 | 2,891 |
| 34 | ★★★★ | Vader Immortal: A Star Wars VR Series | ILMxLAB | 2020 | **必需** | — | 2,878 |
| 35 | ★★★★ | Rick and Morty: Virtual Rick-ality | Owlchemy Labs | 2018 | **必需** | — | 2,853 |
| 36 | ★★★★ | Moss | Polyarc Games | 2018 | — | 5.92 | 2,828 |
| 37 | ★★★★ | Polybius | Llamasoft | 2017 | — | — | 2,713 |
| 38 | ★★★★ | Bound | Plastic | 2016 | — | 1.45 | 2,698 |
| 39 | ★★★★ | Thumper | Drool | 2016 | — | — | 2,599 |
| 40 | ★★★★ | Robinson: The Journey | Crytek | 2016 | — | — | 2,500 |
| 41 | ★★★ | Dirt Rallyˣᵖ | Codemasters | 2017 | — | 40.33 | 7,853 |
| 42 | ★★★ | Arizona Sunshine | Vertigo Games | 2017 | 可选 | — | 2,290 |
| 43 | ★★★ | Doom VFR | Bethesda | 2017 | 可选 | 13.55 | 2,003 |
| 44 | ★★★ | Cyber Danganronpa VR: The Class Trial | Spike Chunsoft | 2016 | — | 0.65 | 1,931 |
| 45 | ★★★ | Monster of the Deep: Final Fantasy XV | Square Enix | 2017 | **必需** | — | 1,930 |
| 46 | ★★★ | The Idolmaster Cinderella Girlsˣᵖ | Bandai Namco Ent. | 2016 | — | — | 6,230 |
| 47 | ★★★ | The Walking Dead: Saints & Sinnersˣᵖ | Skydance Interactive | 2020 | **必需** | — | 6,173 |
| 48 | ★★★ | Megaton Rainfall | Alfonso del Cerro | 2017 | 可选 | — | 1,809 |
| 49 | ★★★ | RIGS: Mechanized Combat League | Guerrilla Cambridge | 2016 | — | 18.59 | 1,744 |
| 50 | ★★★ | Firewall: Zero Hour | First Contact Ent. | 2018 | — | 9.69 | 1,709 |
| 51 | ★★★ | Tetris Effectˣᵖ | Enhance Games | 2018 | — | — | 5,586 |
| 52 | ★★★ | Concrete Genieˣᵖ | Worldwide Studios | 2019 | **必需** | — | 5,385 |
| 53 | ★★★ | PlayStation VR Worlds | London Studio | 2016 | 可选 | — | 1,601 |
| 54 | ★★★ | Keep Talking and Nobody Explodesˣᵖ | Steel Crate Games | 2016 | — | 0.65 | 5,232 |
| 55 | ★★★ | Farpoint | Impulse Gear | 2017 | — | — | 1,543 |
| 56 | ★★★ | Obductionˣᵖ | Cyan Worlds | 2017 | — | — | 5,104 |
| 57 | ★★★ | NBA 2K: VR Experienceⁱᵖ | 2K Games | 2016 | 可选 | — | 15,134 |
| 58 | ★★★ | Don't Knock Twice | Wales Interactive | 2017 | 可选 | — | 1,473 |
| 59 | ★★★ | Home Sweet Homeˣᵖ | Yggdrazil Group | 2018 | — | — | 4,704 |
| 60 | ★★★ | SuperHyperCube | Kokoromi | 2016 | — | 0.21 | 1,406 |

读这张表要注意：前 8 名里有 6 个带折算标记，说明**榜首集中的是"带 VR 模式的平面大作"，不是 VR 原生作品**。真正的 PSVR 原生标杆是 The Inpatient（第 5）、Beat Saber（第 6）、Déraciné（第 9）、Until Dawn: Rush of Blood（第 11）、Batman: Arkham VR（第 13）、Astro Bot Rescue Mission（第 17）、Blood & Truth（第 25）、Moss（第 36）。

## 4. 体积（62 款有精确数据，降序）

| GB | 热度 | 标题 | 开发商 | 年份 | Move | CUSA |
|---:|:---|---|---|---:|:---:|---|
| 40.33 | ★★★ | Dirt Rally | Codemasters | 2017 | — | `CUSA03648` |
| 40.04 | ★★★★★ | Gran Turismo Sport | Polyphony Digital | 2017 | — | `CUSA03220` |
| 38.74 | ★★★★★ | Tekken 7 | Bandai Namco Ent. | 2017 | — | `CUSA05972` |
| 22.23 | ★★★ | Wipeout Omega Collection | Sony XDev Europe | 2018 | — | `CUSA07671` |
| 20.64 | ★★★★★ | Resident Evil 7: Biohazard | Capcom | 2017 | — | `CUSA03962` |
| 18.59 | ★★★ | RIGS: Mechanized Combat League | Guerrilla Cambridge | 2016 | — | `CUSA00257` |
| 14.66 | ★ | Driveclub VR | Evolution Studios | 2016 | — | `CUSA00093` |
| 13.55 | ★★★ | Doom VFR | Bethesda | 2017 | 可选 | `CUSA09090` |
| 12.16 | ★★★★ | Until Dawn: Rush of Blood | Supermassive Games | 2016 | 可选 | `CUSA03683` |
| 11.14 | ★ | Hikaru Utada Laughter in the Dark Tour 2018 | Sony Interactive Entertainment | 2018 | — | `CUSA14194` |
| 9.69 | ★★★ | Firewall: Zero Hour | First Contact Ent. | 2018 | — | `CUSA11182` |
| 7.99 | ★★★★ | Batman: Arkham VR | Rocksteady Studios | 2016 | 可选 | `CUSA05340` |
| 7.40 | ★★ | Everybody's Golf VR | Japan Studio | 2019 | 可选 | `CUSA04687` |
| 5.92 | ★★★★ | Moss | Polyarc Games | 2018 | — | `CUSA09760` |
| 5.71 | ★★ | Here They Lie | Tangentlemen | 2016 | — | `CUSA05018` |
| 5.46 | ★★★★ | Eve: Valkyrie | CCP Games | 2016 | — | `CUSA05789` |
| 4.75 | ★ | We Happy Few: Uncle Jack Live VR | Gearbox Publishing | 2018 | 可选 | `CUSA13206` |
| 4.40 | ★ | Vroom Kaboom | Ratloop Games Canada | 2018 | 可选 | `CUSA09670` |
| 4.01 | ★ | Dancing Beauty: Idol Project | Shanghai Oriental Pearl Culture Development | 2018 | **必需** | `CUSA12671` |
| 3.44 | ★ | Crow: The Legend | Baobab Studios | 2018 | 可选 | `CUSA14043` |
| 3.03 | ★ | Kingdom Hearts VR Experience | Square Enix | 2019 | 可选 | `CUSA15095` |
| 2.84 | ★ | Stranger Things: The VR-Experience | Netflix | 2017 | **必需** | `CUSA10298` |
| 2.49 | ★ | Tumble VR | Supermassive Games | 2016 | 可选 | `CUSA05406` |
| 2.43 | ★ | Megalith | Disruptive Games | 2019 | 可选 | `CUSA11089` |
| 2.39 | ★ | VirZoom Arcade | VirZoom | 2016 | — | `CUSA06167` |
| 2.28 | ★ | Air Force Special Ops: Nightfall | Sony Interactive Entertainment | 2017 | **必需** | `CUSA07936` |
| 2.18 | ★ | Khalid Young Dumb and Broke VR | Sony Music Entertainment | 2018 | 可选 | `CUSA11223` |
| 2.12 | ★ | Atom Universe | Atom Republic | 2016 | — | `CUSA02432` |
| 2.06 | ★★ | The Playroom VR | Japan Studio (Team Asobi) | 2016 | — | `CUSA04318` |
| 1.91 | ★ | The Ministry of Time VR: Save the time | El Faro Del Futuro | 2017 | — | `CUSA10017` |
| 1.79 | ★★★★ | Rec Room | Against Gravity | 2017 | **必需** | `CUSA08481` |
| 1.45 | ★★★★ | Bound | Plastic | 2016 | — | `CUSA04193` |
| 1.27 | ★ | Invasion! | Baobab Studios | 2016 | — | `CUSA06761` |
| 1.27 | ★ | Anyone's Diary | World Domination Project | 2019 | 可选 | `CUSA14221` |
| 1.23 | ★★★★ | The Last Guardian VR Demo | Sony Interactive Entertainment | 2017 | — | `CUSA10479` |
| 1.16 | ★★ | Croixleur Sigma | Playism | 2017 | — | `CUSA02163` |
| 1.11 | ★ | ChromaGun VR | Pixel Maniacs | 2019 | — | `CUSA04908` |
| 1.09 | ★ | Statik | Tarsier Studios | 2017 | — | `CUSA06931` |
| 1.07 | ★ | Carnival Games VR | Cat Daddy Games | 2016 | **必需** | `CUSA05697` |
| 1.06 | ★★ | Weeping Doll | Oasis Games | 2016 | — | `CUSA06439` |
| 1.02 | ★★ | Infinite Minigolf | Zen Studios | 2017 | — | `CUSA07813` |
| 0.93 | ★ | I Expect You To Die | Schell Games | 2016 | 可选 | `CUSA07396` |
| 0.90 | ★★★★ | Job Simulator | Owlchemy Labs | 2016 | **必需** | `CUSA05818` |
| 0.85 | ★ | Allumette | Penrose Studios | 2016 | — | `CUSA06821` |
| 0.73 | ★ | Hustle Kings VR | EPOS Game Studios | 2016 | 可选 | `CUSA01332` |
| 0.72 | ★ | Gary the Gull | Limitless / Motional | 2016 | 可选 | `CUSA06780` |
| 0.72 | ★ | Tom Grennan VR | Sony Music Entertainment | 2018 | — | `CUSA13051` |
| 0.71 | ★ | Rollercoaster Dreams | Bimboosoft | 2016 | — | `CUSA05238` |
| 0.69 | ★ | Hatsune Miku: VR Future Live | Sega | 2016 | 可选 | `CUSA04771` |
| 0.65 | ★★★ | Keep Talking and Nobody Explodes | Steel Crate Games | 2016 | — | `CUSA06561` |
| 0.65 | ★★★ | Cyber Danganronpa VR: The Class Trial | Spike Chunsoft | 2016 | — | `CUSA07125` |
| 0.60 | ★★★★ | Accounting+ | Squanch Games | 2017 | 可选 | `CUSA10154` |
| 0.55 | ★ | The Illusionist-Andres Iniesta | Gamepoch | 2017 | — | `CUSA10516` |
| 0.50 | ★ | Dark Eclipse | Sunsoft | 2018 | 可选 | `CUSA08190` |
| 0.49 | ★★ | Rez Infinite | Monstars | 2016 | 可选 | `CUSA06209` |
| 0.43 | ★ | AnywhereVR | Sony Music Entertainment Japan | 2016 | — | `CUSA08643` |
| 0.37 | ★ | Pinball FX2 VR | Zen Studios | 2016 | — | `CUSA06390` |
| 0.29 | ★★★★★ | Beat Saber | Beat Games | 2018 | **必需** | `CUSA12878` |
| 0.21 | ★★★★★ | Minecraft | Mojang | — | — | `CUSA00744` |
| 0.21 | ★★★ | SuperHyperCube | Kokoromi | 2016 | — | `CUSA04894` |
| 0.13 | ★ | Within | Within Unlimited | 2016 | — | `CUSA07204` |
| 0.13 | ★★ | Race the Sun | Flippfly | 2017 | — | `CUSA00708` |


### 分布

| 区间 | 数量 |
|---|---|
| < 1 GB | 21 |
| 1–5 GB | 25 |
| 5–15 GB | 10 |
| 15–30 GB | 3 |
| > 30 GB | 3 |

中位数 **1.62 GB**，平均 **5.48 GB**，62 款合计 **340 GB**。

呈明显双峰：一端是 20–40 GB 的 3A 大作附带 VR 模式（Dirt Rally、GT Sport、Tekken 7、RE7），另一端是 0.1–1 GB 的纯 VR 小体量作品。**VR 原生游戏几乎都在 5 GB 以下**——VR 内容量普遍小于同期平面游戏。

按中位数外推 527 款全集约 **1–1.5 TB**；考虑大作与补丁，实际更可能 **2–3 TB**。这是量级估算，不是实测。

## 5. 高热度但无体积数据的作品

以下为 ★★★★ 及以上、但不在 NoPayStation 索引中的作品。体积留空而非填入网络估值。

| 热度 | 标题 | 开发商 | 年份 | Move |
|:---|---|---|---:|:---:|
| ★★★★★ | The Elder Scrolls V: Skyrim | Bethesda Game Studios | 2017 | 可选 |
| ★★★★★ | The Inpatient | Supermassive Games | 2018 | 可选 |
| ★★★★★ | Star Wars: Squadrons | Motive Studios | 2020 | — |
| ★★★★★ | Déraciné | FromSoftware | 2018 | **必需** |
| ★★★★ | Persona 5: Dancing in Starlight | P-Studio | 2018 | — |
| ★★★★ | Rise of the Tomb Raider: Blood Ties | Crystal Dynamics | 2016 | — |
| ★★★★ | Dreamworks Voltron VR Chronicles | Digital Domain Interactive | 2017 | — |
| ★★★★ | No Man's Sky Beyond | Hello Games | 2019 | 可选 |
| ★★★★ | Star Trek: Bridge Crew | Red Storm Entertainment | 2017 | 可选 |
| ★★★★ | Astro Bot Rescue Mission | Japan Studio (Team Asobi) | 2018 | — |
| ★★★★ | Five Nights at Freddy's: Help Wanted | Steel Wool | 2019 | 可选 |
| ★★★★ | Dead or Alive Xtreme 3 | Koei Tecmo | 2017 | — |
| ★★★★ | Wolfenstein: Cyberpilot |  | 2019 | 可选 |
| ★★★★ | Sega Genesis Classics | Sega | 2018 | — |
| ★★★★ | Fate/Grand Order VR feat. Mashu Kyrielight | Aniplex | 2017 | — |
| ★★★★ | Psychonauts in the Rhombus of Ruin | Double Fine Productions | 2017 | — |
| ★★★★ | Hatsune Miku: Project DIVA X | Sega | 2016 | 可选 |
| ★★★★ | Blood & Truth | London Studio | 2019 | 可选 |
| ★★★★ | Persona 3: Dancing in Moonlight | P-Studio | 2018 | — |
| ★★★★ | Creed: Rise to Glory | Survios | 2018 | **必需** |
| ★★★★ | Disaster Report 4 Plus: Summer Memories | Granzella | 2018 | — |
| ★★★★ | Vader Immortal: A Star Wars VR Series | ILMxLAB | 2020 | **必需** |
| ★★★★ | Rick and Morty: Virtual Rick-ality | Owlchemy Labs | 2018 | **必需** |
| ★★★★ | Polybius | Llamasoft | 2017 | — |
| ★★★★ | Thumper | Drool | 2016 | — |
| ★★★★ | Robinson: The Journey | Crytek | 2016 | — |


## 6. CSV 字段

| 列 | 含义 |
|---|---|
| `popularity` | ★ 热度，1–5 |
| `title` / `genre` / `developer` / `year` | 标题、类型、开发商、最早区域发行年 |
| `move` | `必需` / `可选` / `—` |
| `aim` / `hotas` / `retail_bd` / `free_to_play` | `Y` 或空 |
| `cusa` | PS4 title ID，可用于 orbispatches 等库交叉查询 |
| `size_gb` / `size_bytes` | 首发基础包，**465 行为空** |
| `size_source` | `exact` 精确匹配 / `fuzzy` 模糊匹配（≥0.92，已人工复核） |
| `pv_monthly` | 折算前原始月均浏览量，**404 行为空** |
| `pv_adjust` | `proxy` = IP 本体 ×0.1；`multiplat` = 跨平台 ×0.3；空 = 未折算 |

`size_source=fuzzy` 的 5 条已逐条核对，均为同一游戏的命名差异（`™`、`&` 写法、区域版）：Wipeout Omega Collection, RIGS: Mechanized Combat League, Firewall: Zero Hour, Batman: Arkham VR, Khalid Young Dumb and Broke VR。

## 7. 与 shadPS4 的关系

shadPS4 **无法运行任何 PSVR 内容**，详见 [架构对比文档附录 A](shadps4-citron-architecture-comparison.md#附录-a2026-09-04-复核记录)：`libSceHmd` 恒返回「未检测到头显」，全仓无 OpenXR/OpenVR 对接。

结合热度看，边界很清晰：

- **165 款 Move 必需**的游戏基本无法进入正常流程，其中包括 Beat Saber、Blood & Truth、Déraciné 等高热度作品；
- **223 款无 Move 标记**里有相当部分是 VR 可选的平面游戏——而这批恰好占据了热度榜前列（Minecraft、RE7、Skyrim、Tekken 7、GT Sport）。这些在 shadPS4 上以 **2D 模式**运行是可行的，`libSceHmd` 桩的设计意图正是让它们正常回退。

也就是说，**PSVR 清单里热度最高的那部分，恰恰是最不依赖 VR 支持的那部分**。从模拟器兼容性角度，本清单的用途是划出「必须有 VR 支持」与「2D 回退即可」的边界，而不是可玩清单。
