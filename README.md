# Sqrtalk

一个轻量级的 C++17 WebSocket 聊天服务器，支持用户认证、权限组、文件共享、频道管理和 SQLite 存储。

## 快速开始

```bash
# 安装依赖（Debian/Ubuntu）
sudo apt install -y cmake g++ libboost-all-dev libssl-dev nlohmann-json3-dev sqlite3 libsqlite3-dev
# 安装 websocketpp（仅头文件）：
git clone https://github.com/zaphoyd/websocketpp.git
sudo cp -r websocketpp/websocketpp /usr/local/include/

# 构建（需要 SQLite 开发库）
cmake -B build && cmake --build build

# 首次运行前请先创建 config.json（参见下方配置文件章节）
./build/sqrtalk_server
```

默认 WebSocket 端口：`9002` · 文件服务器（HTTP）端口：`3467`

## 客户端协议

通过 WebSocket 连接到 `ws://<host>:9002`。所有消息均为 JSON 格式。

### 登录 / 自动注册

```json
{"type": "login", "username": "alice", "password": "secret123", "channel": "main"}
```

新用户名将自动注册。响应：

```json
{"token": "abc123..."}                                        // 用于重新登录的令牌
{"type": "info", "message": "Login successful."}              // 已有用户，密码正确
{"type": "info", "message": "Registered and logged in successfully"}  // 新用户自动注册
{"type": "info", "message": "User: alice, bob"}               // 在线用户列表
{"type": "warn", "message": "Incorrect password"}             // 密码错误（已有用户）
```

| 字段 | 必需 | 规则 |
|------|------|------|
| `type` | 是 | `"login"` |
| `username` | 是 | 字母、数字、下划线；3–20 个字符 |
| `password` | 是 | 字母、数字、下划线 |
| `channel` | 是 | 要加入的频道名称（例如 `"main"`） |

### 令牌登录

```json
{"type": "login", "token": "abc123..."}
```

### 访客登录

```json
{"type": "guest", "channel": "main"}
```

自动根据 IP 地址生成访客用户名（格式：`Guest <hash前缀><随机串>`）。

### 发送消息 / 命令

```json
{"type": "message", "message": "/help"}
```

### 文件上传

```json
{"type": "uploadfile", "filename": "photo.png", "data": "<base64 内容>"}
```

响应：`{"type": "info", "message": "File uploaded: <file_id>"}`

### 服务器响应格式

所有响应：`{"type": "<type>", "message": "..."}`

| 响应类型 | 含义 |
|---------|------|
| `info` | 成功 / 信息 |
| `warn` | 错误 / 权限不足 |
| `message` | 来自其他用户的聊天消息 |
| _（无 type）_ | 登录时返回的令牌 |

## 命令

命令的访问权限由用户所属权限组中设置的权限决定，而非权限等级。参见[可用权限](#可用权限)列表。

### 通用命令（无需特定权限）

#### `/help [command]`
显示所有命令，或显示某个命令的详细帮助。
```
/help           → 列出所有命令
/help w         → 显示 /w 的详细信息
```

#### `/user_list`
列出当前频道中的在线用户。

#### `/w <username> <message>` — 需要 `w` 权限
```
/w bob Hey, check this out!
```

#### `/lw` — 无需特定权限
查看自己的留言消息。
```
/lw                          → messagelist
                               [2025-01-01 12:00] bob: hello
```

#### `/lw <username> <message>` — 需要 `lw` 权限
给离线用户留言。对方登录时会收到通知。
```
/lw bob I sent the files
```

#### `/join <channel>`
加入或切换频道。加入锁定频道需要 `joinlockroom` 权限。
```
/join developers
```

#### `/me <action>`
发送动作/表情消息。显示为 `@username action`。
```
/me waves
```

#### `/ignore add|remove <username>`
屏蔽或取消屏蔽某个用户的消息。
```
/ignore add annoying_user
/ignore remove annoying_user
```

#### `/permission`
查看您的权限组名称。

#### `/level`
查看您的权限等级和所属组。

#### `/token_list`
列出您的认证令牌，用于免密码登录。

#### `/rmtoken <token>`
移除一个认证令牌。

#### `/setpwd <new_password>`
修改自己的密码（无需特定权限，仅限字母、数字、下划线）。
```
/setpwd newpass456
```

#### `/setpwd <username> <new_password>` — 需要 `setpwd` 权限且自身等级高于目标
修改其他用户的密码（仅限字母、数字、下划线）。
```
/setpwd bob newpass456
```

### 文件管理（权限因操作而异）

```
/file                                     → 列出您的文件
/file download <filename>                 → 下载文件
/file setprivate <filename> true|false    → 设置文件隐私
/file rename <filename> <new_name>        → 重命名
/file remove <filename>                   → 删除
/file manage list                         → 列出有 file 权限的用户
/file manage list <username>              → 查看指定用户的文件列表
/file manage download <filename>          → 下载任意文件（需要 managefile 权限）
/file manage remove <filename>            → 删除任意文件（需要 managefile 权限）
```

所有 `/file` 操作均需 `file` 权限。管理操作（`manage`）还需额外的 `managefile` 权限。

### 用户管理

#### `/kick <username>` — 需要 `kick` 权限
踢出用户。无法踢出权限等级不低于您的用户。
```
/kick bob
```

#### `/ban_list` — 需要 `ban` 权限
列出所有被封禁的用户和 IP。

#### `/ban <username>` — 需要 `ban` 权限
封禁用户及其 IP。被封禁的用户无法登录。
```
/ban bob
```

#### `/unban <IP_or_username>` — 需要 `ban` 权限
解封 IP 或用户名。
```
/unban 192.168.1.1
/unban bob
```

#### `/unbanall` — 需要 `ban` 权限
解封所有 IP 和用户。请谨慎使用。

#### `/banip <IP>` — 需要 `ban` 权限
直接封禁 IP。该 IP 上的所有用户将立即断开连接。
```
/banip 10.0.0.99
```

#### `/mute <username>` · `/unmute <username>` — 需要 `mute` 权限
禁言/解除禁言用户。被禁言的用户无法发送频道消息（悄悄话仍可发送）。
```
/mute bob
/unmute bob
```

### 频道管理

#### `/lock site|channel <name>` — 需要 `lock` 权限
锁定/解锁。再次运行可切换状态。
```
/lock site               → 锁定/解锁整个服务器
/lock channel general    → 锁定/解锁指定频道
```

#### `/channel_list` — 需要 `channel_list` 权限
列出所有活跃频道。

### 广播与日志

#### `/boardcast <message>` — 需要 `boardcast` 权限
向所有在线用户广播消息。
```
/boardcast Server maintenance in 5 minutes!
```

### 状态查询

#### `/status user <username>`
查询自己无需特定权限；查询他人需要权限组等级 >= 2 且自身等级高于目标。
```
/status user alice       → User: alice (online)
                           Level: 0
                           Group: normal
                           Channel: main
                           IP: 127.0.0.1
                           Recent IPs: ...
```

#### `/status channel <name>` — 需要权限组等级 >= 2
```
/status channel general  → Channel: general
                           onlineUser: alice, bob
```

#### `/log [filename]` — 需要 `test` 权限
查看服务器日志。
```
/log                     → 今天的日志
/log server_2025.log     → 指定日志文件
```

### 权限组管理（需要 `setpms` 权限或权限组等级 >= 2）

#### `/gperm list`
列出所有权限组及其等级和标签。

#### `/gperm create <name> <level> [tag] [color]`
创建新的权限组（不能创建等级不低于您的组）。
```
/gperm create moderator 1 [Mod]
/gperm create vip 0 [VIP] #gold
```

#### `/gperm set <username> <group>`
设置用户的权限组。不能修改自己，也不能设置等级不低于您的组。
```
/gperm set bob moderator
```

#### `/gperm modify <group> <field> <value>`
修改组的属性。

| 字段 | 描述 | 示例 |
|------|------|------|
| `tag` | 显示标签 | `/gperm modify moderator tag [Moderator]` |
| `color` | 标签颜色 | `/gperm modify admin color #ff0000` |
| `level` | 权限等级 | `/gperm modify moderator level 1` |
| `addperm` | 添加权限 | `/gperm modify moderator addperm ban` |
| `rmperm` | 移除权限 | `/gperm modify moderator rmperm kick` |

#### `/gperm delete <group>`
删除权限组。
```
/gperm delete moderator
```

#### 可用权限

| 权限 | 授予的访问权限 |
|------|---------------|
| `w` | 发送频道消息 |
| `lw` | 留言消息 |
| `file` | 文件上传/下载 |
| `managefile` | 管理所有用户文件 |
| `kick` | 踢出用户 |
| `ban` | 封禁/解封用户和 IP（含 banip、ban_list、unbanall） |
| `mute` | 禁言/解除禁言用户 |
| `lock` | 锁定频道或站点 |
| `joinlockroom` | 加入锁定频道 |
| `channel_list` | 列出所有频道 |
| `boardcast` | 广播消息 |
| `setpwd` | 修改其他用户的密码 |
| `setpms` | 管理权限组 |
| `test` | 查看服务器日志 |

## 频率限制

| 限制项 | 数值 |
|-------|------|
| 消息间隔 | 300ms |
| 消息突发 | 每 60 秒 30 条 |
| 登录间隔 | 每个连接 5 秒 |
| 每个 IP 最大注册数 | 5 |

## 配置文件 (`config.json`)

> **注意：** `config.json` 需要您手动创建，将其放在与可执行文件相同的目录下。服务器启动时会读取该文件。以下是一个参考配置：

```json
{
    "server": {
        "ws_port": 9002,
        "file_server_port": 3467,
        "thread_pool_size": 4,
        "max_http_body_size": 65536,
        "ping_interval_sec": 10
    },
    "password": {
        "salt": "ChangeThisSaltInProduction!"
    },
    "rate_limit": {
        "msg_interval_ms": 300,
        "msg_burst": 30,
        "msg_window_sec": 60,
        "login_interval_sec": 5,
        "max_registrations_per_ip": 5
    },
    "validation": {
        "min_username_len": 3,
        "max_username_len": 20,
        "max_message_len": 256,
        "guest_name_len": 3
    }
}
```

## 数据库

SQLite（`sqrtalk.db`），包含 3 张表：

- **users** — 用户名、密码（SHA-256 + 盐值）、等级、禁言状态、权限组、被忽略的用户、用户文件、注册 IP
- **permission_groups** — 名称、标签、颜色、等级、权限列表
- **login_history** — 用户名、IP、登录时间

首次运行时，服务器会将 JSON 文件（`users.json`、`permission_groups.json`）中的现有数据迁移到 SQLite。

## 默认权限组

| 组 | 等级 | 标签 | 颜色 | 权限 |
|----|------|------|------|------|
| `normal` | 0 | — | — | w, lw, file |
| `admin` | 2 | [Admin] | #ff0000 | ban, boardcast, channel_list, file, joinlockroom, kick, lock, lw, managefile, mute, setpms, setpwd, test, w |

## 目录结构

```
├── build/               # 构建输出
├── files/               # 上传的文件
├── logs/                # 服务器日志
├── config.json          # 服务器配置（需自行创建）
├── sqrtalk.db           # SQLite 数据库
├── banned_users.json    # 被封禁的 IP 和用户
└── files.json           # 文件元数据
```
