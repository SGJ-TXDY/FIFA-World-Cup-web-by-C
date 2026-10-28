# FIFA World Cup web by C —— 世界杯球员信息查询网站

用 C 语言从零实现的 Web 服务器：TCP socket 收发、HTTP 报文解析、路由分发、SQLite 存储、动态生成 HTML，
提供世界杯 48 支球队、1254 名参赛球员的基础信息查询。

## 功能

| 地址 | 内容 |
|---|---|
| `/` | 统计横幅、射手榜前 10、12 个小组积分表 |
| `/team?id=fra` | 球队资料（小组、主教练、FIFA 排名、身价、战绩）和按位置排序的大名单 |
| `/player?id=858` | 球员资料（球队、位置、俱乐部、进球、助攻）和同队名单 |
| `/search?name=关键词` | 按姓名 / 俱乐部 / 球队 / 位置搜索，姓名唯一命中时直接跳转详情页 |
| `/index.html`、`/register.html` | 登录、注册（账号保存在 `mima.db`，登录状态用 Cookie 记录） |

## 目录结构

```text
.
├── work file/              # 世界杯球员查询网站（主项目）
│   ├── webserver.c         # 服务器框架：监听、解析请求、路由、发送响应、main
│   ├── user.c              # 注册、登录、表单 / URL 参数解析、Cookie
│   ├── db.c                # 启动时从 teams.txt / players.txt 导入 worldcup.db
│   ├── player.c            # 球员 / 球队查询与页面生成
│   ├── head.h              # 结构体与函数声明
│   ├── style.css           # 动态页面公共样式
│   ├── index.html          # 登录页
│   ├── register.html       # 注册页
│   ├── teams.txt           # 球队数据（TAB 分隔）
│   ├── players.txt         # 球员数据（TAB 分隔）
│   ├── tools/export-data.mjs  # 从原始 json/csv 数据导出 teams.txt / players.txt
│   ├── makefile
│   └── PLAN.md             # 改造规划与各步骤说明
└── webserver_wordmean/     # 参考项目：英汉词典查询服务器（同时提供 sqlite3.c）
```

## 编译运行（Linux）

```bash
cd "work file"
make run
```

首次启动会自动创建 `worldcup.db` 并导入数据，随后在浏览器访问 `http://<服务器IP>:8080/`。
队徽图片为 football-data.org 外链，需要联网才能显示。

- `make`：只编译，生成 `worldcup_server`
- `make clean`：删除可执行文件和本项目 `.o`（保留编译较慢的 `sqlite3.o`）
- `make distclean`：连同 `sqlite3.o` 一起删除

## 更新数据

```bash
node tools/export-data.mjs [原始数据目录]   # 重新生成 teams.txt / players.txt
rm worldcup.db                              # 删除旧库，下次启动时重新导入
```

## 说明

- 所有数据库查询使用参数绑定，页面输出对用户输入做 HTML 转义，静态文件禁止访问 `..` 和 `.db` 等文件。
- 密码目前明文存储、Cookie 未签名，仅适合学习使用（见 `work file/PLAN.md` 第 6 步）。
