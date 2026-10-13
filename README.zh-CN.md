# honor-fan

**荣耀 MagicBook Pro 16（2024，`DRA-XX`）** 在 Linux 下的风扇转速读取与控制。

这台笔记本在 Linux 下**没有任何可用的风扇接口**：既没有风扇 `hwmon` 设备，固件声明的
五个 ACPI 风扇也全是空壳。转速表和手动调速功能确实存在，但只藏在嵌入式控制器（EC）
内部，需要通过一个没有公开文档的内存映射命令信箱才能访问。

本项目从这台机器自己的 ACPI 表中逆向出该信箱协议，并实现为一个 `hwmon` 内核驱动。

> **请先看这里**：风扇**读取**功能稳定可靠；风扇**控制**功能可用但尚不稳定，
> 详见 [当前状态](#当前状态)。因此控制功能默认关闭，需显式开启。

[English](README.md)

---

## 硬件

| | |
|---|---|
| 型号 | 荣耀 MagicBook Pro 16 2024，`DRA-XX`（`M1040`） |
| CPU | Intel Core Ultra 7 155H（Meteor Lake） |
| 显卡 | NVIDIA RTX 4060 Laptop |
| EC | `\_SB.PC00.LPCB.H_EC`，新唐（Nuvoton） |
| 风扇 | **2 个**物理风扇 |
| 测试环境 | Ubuntu 24.04，内核 7.0 |

共用同代 EC 固件的其他荣耀 MagicBook / 华为 MateBook 机型可能同样适用，但下文的地址
只在 `DRA-XX` 上验证过。其他机型可用 `force=1` 加载尝试，风险自负。

## 为什么常规方法全都不行

| 途径 | 结果 |
|---|---|
| `hwmon` 的 `fan*_input` / `pwm*` | **根本不存在**，系统里没有任何风扇 hwmon 设备 |
| ACPI 风扇 `\_TZ_.FAN0..FAN4`（`PNP0C0B`，即 `cooling_device2-6`） | **是摆设**。`_ON`/`_OFF` 只设置 `CVF0..CVF4` 标志位并调用 `FNCL()`，而它最终调用的 `\_SB.PC00.LPCB.UPFS(...)` 是一个**空方法**。写 `cur_state` 毫无作用 |
| `thermal` / DPTF | 只能读温度，无法驱动风扇 |
| `huawei_wmi` | 能加载，但不认领任何风扇 GUID，`ABBC0F5B/5C-...` 处于无驱动状态 |
| `ec_sys` 的 256 字节 EC 空间 | 有温度探头，但**没有转速表** |

风扇完全由 EC 自主控制，唯一的入口就是 EC 本身。

## 工作原理

固件把两个 EC 窗口映射到了物理内存：

```
OperationRegion (ERAM, SystemMemory, 0xFE0B0300, 0x0100)   ; EC RAM 镜像
OperationRegion (SMA2, SystemMemory, 0xFE0B0400, 0x0080)   ; 命令信箱
```

`ERAM` 是传统 256 字节 EC 空间的镜像——已验证：偏移 `0x20..0x2B` 正是温度探头
`TP00..TP0A`，与内核的 `SEN1..SEN8`/`TCPU` 逐项吻合。但它不含任何风扇数据。

`SMA2` 是通往**扩展 EC RAM** 的信箱，风扇寄存器就在那里：

| 扩展地址 | 含义 |
|---|---|
| `0x20020A00/01` | 风扇 1 转速，16 位小端 |
| `0x20020A02/03` | 风扇 2 转速，16 位小端 |
| `0x20020A04/05` | 风扇目标转速，单位 100 rpm |
| `0x20020A1B` | `FSUD` — 手动模式开关（0 = 交由固件，1 = 手动） |
| `0x20020A1C` | `FSVL` — 手动转速值 |

读用命令 `0x80`；写需要先解锁（`DATR=0x4D`、`CMDB=0x29`）再发命令 `0x81`。
完整协议见 **[docs/PROTOCOL.zh-CN.md](docs/PROTOCOL.zh-CN.md)**，
对应的固件源码证据见 **[docs/ASL-EVIDENCE.md](docs/ASL-EVIDENCE.md)**。

### 转速编码

这一项无法从固件静态推出——ACPI 方法 `SFCS` 从来没有用常量调用过，只能实测。
`FSVL` 是**在风扇怠速基准之上的增量，单位 100 rpm**：

```
目标转速 = (base + FSVL) × 100        base = 16（风扇1）、14（风扇2）
```

实测结果，完全线性：

| FSVL | 10 | 20 | 30 | 40 | 50 | 60 | 80 | 100 |
|---|---|---|---|---|---|---|---|---|
| 目标 rpm | 2600 | 3600 | 4600 | 5600 | 6600 | 7600 | 9600 | 11600 |

风扇物理上限约 5800 rpm，因此有效的 `FSVL` 范围是 0–42。
`FSVL = 0` 表示怠速（约 1600 rpm），**不是关闭**。

### 三个会坑到你的坑

1. **跨模式切换时绝不要“释放再重武装”。** 手动模式内直接覆写 `FSUD`/`FSVL`
   完全没问题，但紧接着的 `(0,0)` 释放+快速重武装会让 EC 锁死在**双风扇停转**。
   交还固件必须分步：先单写 `FSVL=0`，静置几秒，再单写 `FSUD=0`，再静置。
2. **EC 受不了背靠背的连续写入。** 每次写都是完整的"解锁+写入"信箱握手；若在几微秒
   内连续发出，EC 会失步、停止响应，并可能让风扇停转。一切操作都要留足间隔——
   风扇调速对延迟不敏感。
3. **锁死的 EC 需要真正的 idle，必要时断电。** 停止一切信箱访问（读也算）等几分钟。
   若 15 分钟以上无恢复就断电重置 EC：关机、拔电源、长按开机键 15 秒、静置一分钟
   再开机。普通重启清不掉 EC——它一直有待机供电。

## 当前状态

| 功能 | 状态 |
|---|---|
| 读取两个风扇转速 | **稳定可用** |
| 读取 11 路 EC 温度探头 | **稳定可用**，已与内核 thermal zone 交叉验证 |
| 从干净状态**设置一次**转速 | **可用。** 如 `set 70` → 稳定在 4440/4260 rpm，与预测的 4500 吻合 |
| 手动模式生效后**再改**转速 | **可用。** 如 `set 70` → `set 50` → `set 30` 得到 4440 → 3660 → 2820 rpm。更新直接覆写，无释放步骤 |
| 交还固件控制（`auto`） | **可用**，分步释放（`FSVL=0`、静置、`FSUD=0`、静置），约 16 秒 |
| 从锁死状态恢复 | 停止一切信箱访问等几分钟；不行就 G3 断电重置 EC。无需重装系统 |

**修复经过。** EC 从不是拒绝第二次写入——它拒绝的是驱动每次包在外面的"释放再重武装"
跳变。已验证：直接覆写（含冗余重写 `FSUD=1`）可反复跟随，分步单字节释放可干净交还。
顺带排除的假设：`ECMT` 竞态与 `QWordAcc` 撕裂（改走固件自己的 `\SFCS` 方法、同一把锁、
同一时序，复现依旧）、缺失的 `_Qxx` 握手（`\SFCS` 在固件内零调用者）、`ZNCJ` 参与
（全程 `0xA0`，EC 从未触发 `_QF8`）。写入现经固件 `\SFCS`/`WTER` 方法
（`fw_path=1`，`fw_path=0` 可切回裸写信箱）。

## 安装

Secure Boot 会让内核进入 `lockdown=integrity` 模式，它封死了所有从用户态访问该硬件的
途径（`/dev/mem`、`ioperm()`、`ec_sys write_support=1`、未签名模块）。而**已签名的**
内核模块不受限制——这正是本驱动存在的原因。

```bash
git clone https://github.com/Little-King2022/honor-fan
cd honor-fan/kernel
make
```

### 保持 Secure Boot 开启（推荐）

```bash
sudo ./sign_and_enroll.sh     # 生成 MOK 密钥、签名模块、注册密钥
sudo reboot                   # 蓝色 MOK 界面：Enroll MOK -> Continue -> Yes -> 输入密码
```

注册 MOK **不会**关闭 Secure Boot。如果是双系统，动这块之前请先准备好 BitLocker
恢复密钥（`account.microsoft.com/devices/recoverykey`）。

每次内核更新后需重新运行 `sign_and_enroll.sh`；密钥会复用，无需再次注册。

### 已关闭 Secure Boot

不需要签名，直接 `sudo insmod honor_fan.ko`。

### 加载

```bash
sudo insmod honor_fan.ko                    # 仅监控
sudo insmod honor_fan.ko enable_control=1   # 同时允许调速

# 开机自动加载
sudo make install
echo honor_fan | sudo tee /etc/modules-load.d/honor_fan.conf
```

## 使用

```console
$ tools/honor-fan status
mode      : auto (EC firmware)
pwm1      : 255/255
fan1      : 1620 rpm
fan2      : 1320 rpm

CPU       : 56 C
SEN1      : 40 C
...
```

这是标准 `hwmon` 接口，`sensors` 及其他支持 hwmon 的工具都能直接识别。

开启 `enable_control=1` 后：

```bash
tools/honor-fan set 70      # 手动，70%
tools/honor-fan auto        # 交还 EC 固件
```

项目还包含温度曲线守护进程（`tools/fan_curve.py`）及其 systemd 服务单元。它在退出时会
把风扇交还固件控制，驱动卸载时同样如此。

### 如果风扇停转了

停止一切 EC 访问然后等待。它一般会自行恢复，但要给足**数分钟**——90 秒是不够的。

```bash
sudo rmmod honor_fan
sleep 300
sudo insmod honor_fan.ko
tools/honor-fan status
```

若 15 分钟以上无恢复，用 G3 断电重置 EC（普通重启清不掉它）：

```bash
sudo rmmod honor_fan
sudo shutdown -h now
# 拔电源、长按开机键 15 秒、静置一分钟再开机
```

无论本驱动做什么，CPU 自身的硬件过热保护（110 °C 强制降频与关机）始终有效。

## 模块参数

| 参数 | 默认值 | 含义 |
|---|---|---|
| `enable_control` | `0` | 是否允许写入风扇转速。关闭时为纯监控 |
| `fsvl_max` | `42` | `pwm1 = 255` 时对应的 `FSVL`；42 约合 5800 rpm 目标 |
| `fsud_manual` | `1` | 写入 `FSUD` 以选择手动模式的值 |
| `fw_path` | `1` | 经固件 `\SFCS`/`WTER` 方法写入；`0` 切回裸写信箱 |
| `write_delay_ms` | `150` | 每次 EC 写后的延时，调小会失步 |
| `release_step_ms` | `8000` | 分步释放各步之间的静置时间（毫秒） |
| `force` | `0` | 跳过 DMI 机型检查（用于其他机型，风险自负） |

## 复现这项逆向工作

```bash
sudo apt install acpica-tools
sudo cp /sys/firmware/acpi/tables/DSDT dsdt.dat
iasl -d dsdt.dat
grep -n "RDER\|WTER\|GFNS\|SFCS" dsdt.dsl
```

`tools/ec_watch.sh` 可对 256 字节 EC 空间做时间序列差分（需要 `ec_sys`），温度探头就是
这样定位的。`tools/fan_probe.sh` 用于扫描 `FSUD`/`FSVL`，且在任何情况下退出都会恢复自动模式。

## 安全说明

- `FSVL = 0` 是怠速而非关闭，驱动不会请求零目标转速
- 驱动在卸载时以及 `pwm1_enable = 2` 时都会把风扇交还固件
- EC 仍有可能进入风扇停转状态（见"当前状态"）。在该问题修复前，请勿在持续高负载下
  无人值守地使用手动控制
- 硬件过热保护独立于本驱动，始终有效

## 许可证

GPL-2.0。驱动是 Linux 内核模块，用户态工具同样遵循该许可证。
