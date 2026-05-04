# syslogd

从 busybox 1.36.1 sysklogd 剥离的独立 syslog 守护进程，专为嵌入式设备设计。

## 特性

- **内存环形缓冲区**（SysV 共享内存）：512 条 × 1KB，无锁单生产者多消费者，进程崩溃消息不丢
- **TCP/UDP 远程转发**：单远端 syslog 服务器，TCP 断连自动重连
- **TCP/UDP syslog 服务器**：监听接收其他设备的日志写入环形缓冲区
- **日志读取模式**（`-v`）：dump 全部 / 最后 N 条 / follow 实时追踪
- **运行时配置**（`-c`）：Unix 域套接字控制通道，`--set`/`--get`/`--reload` 在线修改参数，零轮询开销
- **本地文件写入**：支持按大小轮转，轮转期间不丢日志
- **内核日志**：读取 `/dev/kmsg`（`-k`），写入 `/dev/kmsg`（`-K`）
- **单一二进制**：守护进程 + 控制客户端 + 日志读取 三合一

## 编译

### 本地编译

```bash
./build.sh          # gcc 本地编译 → out/gcc/syslogd
```

### 交叉编译

在 `porting/<平台>/porting.cmake` 中配置工具链后：

```bash
./build.sh openwrt-aarch64                         # 单平台
./build.sh gcc openwrt-aarch64                     # 多平台批量
```

当前已配置平台：
- `gcc` — 本地 x86_64
- `openwrt-aarch64` — ImmortalWrt Mediatek Filogic (aarch64 Cortex-A53, musl)

新增平台只需在 `porting/` 下新建目录放入 `porting.cmake`，`build.sh` 自动识别。

## 用法

### 启动守护进程

```bash
syslogd -n -O /var/log/messages
```

### 完整选项

| 选项 | 说明 |
|------|------|
| `-n` | 前台运行（不 daemonize） |
| `-O FILE` | 本地日志文件路径（默认 `/tmp/log/<mac>.log`） |
| `-l N` | 日志级别过滤 1-8（8=debug 最详细） |
| `-s SIZE` | 文件轮转阈值，单位 KB（默认 200，0=不轮转） |
| `-b N` | 轮转保留份数（默认 1，0=清除旧文件） |
| `-R [tcp://]HOST[:PORT]` | 远程 syslog 服务器（默认 UDP，端口 514） |
| `-D` | 丢弃连续重复消息 |
| `-f FILE` | syslog 规则配置文件（默认 `/etc/syslog.conf`） |
| `-k` | 读取内核日志（`/dev/kmsg`） |
| `-K` | 同时写入 `/dev/kmsg` |
| `-r [PORT]` | 监听 TCP+UDP 接收 syslog（默认 514） |
| `-c FILE` | 运行时配置文件路径（同时启用控制套接字 `FILE.ctl`） |
| `-v` | 读取环形缓冲区全部日志并退出 |
| `-v -f` | 读取全部日志后持续追踪新消息 |
| `-v -n N` | 读取最后 N 条日志 |
| `-h` | 帮助 |

### 运行时配置管理

```bash
# 启动守护进程（指定配置文件）
syslogd -n -c /etc/syslog_conf -O /var/log/messages &

# 查询当前配置
syslogd -c /etc/syslog_conf --get

# 修改单个参数（即时生效，无需重启）
syslogd -c /etc/syslog_conf --set log_level=4
syslogd -c /etc/syslog_conf --set remote=tcp://10.0.0.1:514
syslogd -c /etc/syslog_conf --set server_port=514

# 从配置文件重新加载全部参数
syslogd -c /etc/syslog_conf --reload
```

可在线修改的参数：

| 参数 | 说明 |
|------|------|
| `log_file` | 本地日志文件路径 |
| `log_level` | 日志级别 1-8 |
| `log_size` | 轮转阈值（KB），0=不轮转 |
| `log_rotate` | 轮转保留份数 |
| `remote` | 远程 syslog 服务器 `[tcp://]HOST[:PORT]` |
| `server_port` | 监听端口，0=关闭 |
| `kernel_log` | 0/1，读取内核日志 |
| `kmsg` | 0/1，写入 `/dev/kmsg` |

## 架构

```
/dev/log ──────┐
/dev/kmsg ─────┤
TCP:PORT ──────┼──→ ringbuf_produce() ──→ [环形缓冲区 共享内存]
UDP:PORT ──────┤
logger ────────┘
                       │
         ┌─────────────┼─────────────┐
         ▼             ▼             ▼
   文件写出      远程转发       syslogd -v
   (带轮转)    (TCP/UDP)     (读取模式)
```

- **生产者**：`/dev/log`、`/dev/kmsg`、TCP/UDP 服务器端口接收的消息统一写入环形缓冲区
- **消费者**：文件写出和远程转发各自独立出队，互不阻塞；读取模式附加只读访问
- **崩溃恢复**：消费者进度（`file_seq`、`remote_seq`）存在共享内存头，进程崩溃重启后自动恢复

## 配置文件格式

**守护进程配置**（`-c FILE`）：

```
log_file=/var/log/messages
log_level=8
log_size=200
log_rotate=1
remote=tcp://192.168.1.1:514
server_port=0
kernel_log=0
kmsg=0
```

**syslog 规则配置**（`-f FILE`，传统 busybox 格式）：

```
*.info;authpriv.none     /var/log/messages
authpriv.*               /var/log/secure
```

## 不丢日志设计

1. **环形缓冲区**：所有消息先入共享内存，消费者滞后不影响生产者
2. **文件轮转**：轮转时先 rename 再创建新文件，期间消息留在环缓冲
3. **TCP 断连**：发送失败仅关闭连接，消息留在环缓冲，重连后从断点续发
4. **崩溃恢复**：消费者进度持久化在共享内存，进程重启后自动赶上

## 文件说明

| 文件 | 作用 |
|------|------|
| `syslogd.c` | 主程序：环形缓冲区、poll 事件循环、TCP/UDP 收发、配置管理、读取模式 |
| `syslogd_main.c` | `main()` 入口 |
| `compat.h` / `compat.c` | busybox libbb 兼容层（最小子集） |
| `syslog_names.c` | syslog facility/priority 名称表 |
| `CMakeLists.txt` | cmake 构建配置 |
| `build.sh` | 多平台批量编译脚本 |
| `porting/` | 交叉编译工具链配置目录 |

## 许可证

GPLv2 or later
