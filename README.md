# syslogd

独立 syslog 守护进程 + LuCI 诊断工具，面向嵌入式设备

## 目录

| 目录 | 说明 |
|------|------|
| `syslogd/` | 守护进程本体（OpenWrt 包 + 独立编译） |
| `luci-app-diagnosis/` | LuCI Web 诊断工具（开发中） |

## 独立编译

```bash
cd syslogd
./build.sh                    # gcc 本地编译 → out/gcc/syslogd
./build.sh openwrt-aarch64    # 交叉编译 → out/openwrt-aarch64/syslogd
```

新增平台：在 `syslogd/porting/` 下新建目录放入 `porting.cmake`，`build.sh` 自动识别。

## OpenWrt 集成

本仓库同时是一个 OpenWrt feed，可被 OpenWrt 构建系统直接引用。

### feed 方式（推荐）

在 OpenWrt 根目录的 `feeds.conf.default` 添加：

```
src-git syslogd_feed https://github.com/xmprocat/ysyslogd
```

然后：

```bash
./scripts/feeds update syslogd_feed
./scripts/feeds install syslogd luci-app-diagnosis
make menuconfig  # Utilities → syslogd, LuCI → luci-app-diagnosis
```

### 软链接方式（开发调试）

```bash
ln -s /path/to/syslogd-feed/syslogd package/diy/syslogd
ln -s /path/to/syslogd-feed/luci-app-diagnosis package/diy/luci-app-diagnosis
```

## 详细信息

- syslogd 守护进程的功能、选项、配置管理：见 [syslogd/README.md](syslogd/README.md)
- luci-app-diagnosis 开发进度：见 [luci-app-diagnosis/](luci-app-diagnosis/)

