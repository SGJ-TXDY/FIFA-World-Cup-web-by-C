# 改造规划：悦购商城 → 世界杯球员信息查询（C 语言）

目标：保留现有 C 服务器框架（socket + HTTP 解析 + 路由 + SQLite + 动态生成 HTML），
把"商品"换成"球员 / 球队"，做成一个可登录、可搜索的世界杯球员基础信息查询网站。

## 一、数据来源（D:\编程\世界杯网站）

| 文件 | 可用内容 | 用途 |
|---|---|---|
| `data/teams.json` | 48 支球队：id（如 `fra`）、英文名、小组、主教练、战绩（场次/胜/平/负/积分/进失球） | 球队表 |
| `data/i18n/teams.zh-CN.json` | 球队中文名、FIFA 排名、总身价（亿） | 球队表中文字段 |
| `data/players.json` | 1254 名球员：姓名（中文）、所属球队 id、俱乐部、进球、助攻 | 球员表 |
| `世界杯球员.csv` | 按"球队 → 门将/后卫/中场/前锋"分组的名单 | 补出球员**位置**（json 里 position 为空） |
| `球员基础数据.txt` | 射手榜 | 校对进球数（json 已包含，可不单独导入） |

注意：json 里号码、年龄、头像全部为空，第一版页面不展示这些字段；`token.txt`、`.env*` 不使用。

## 二、数据库设计（worldcup.db，替代 123.db）

```sql
create table teams(
    team_id      text primary key,   -- fra
    name_zh      text,               -- 法国
    name_en      text,               -- France
    group_name   text,               -- A ~ L
    coach        text,
    fifa_rank    integer,
    market_value real,               -- 亿
    played integer, wins integer, draws integer, losses integer,
    points integer, goals_for integer, goals_against integer,
    logo_url     text
);
create table players(
    id        integer primary key autoincrement,
    name      text,                  -- 姆巴佩
    team_id   text,                  -- 关联 teams.team_id
    position  text,                  -- 门将/后卫/中场/前锋
    club      text,                  -- 皇家马德里
    goals     integer,
    assists   integer
);
```

## 三、分步实施

1. **（本次）规划 + makefile + 登录/注册页换成世界杯风格**
   - makefile：补上缺失的 `sqlite3.o` 编译规则（源码取自 `../webserver_wordmean/sqlite3.c`）、
     增加头文件依赖、`-Wall -g`、`make run`、更完整的 `clean`；可执行文件改名 `worldcup_server`。
   - `index.html` / `register.html`：换成球场绿 + 深色主题，表单字段、提交地址、校验脚本保持不变，
     现有 `/login`、`/register` 逻辑无需改动。
2. **（已完成）数据导出**：`node tools/export-data.mjs [参考站目录]` 把 json/csv 合并导出为
   TAB 分隔的 UTF-8 文本 `teams.txt`（48 行）、`players.txt`（1254 行），与 `dict.txt` 的做法一致；
   以 `#` 开头的首行是字段说明，C 端导入时跳过；缺失字段为空串。
   - teams.txt 字段：`team_id name_zh name_en group coach fifa_rank market_value played wins draws losses points goals_for goals_against logo_url`，已按小组、FIFA 排名排序；
   - players.txt 字段：`name team_id position club goals assists`；
   - 位置取值：门将 / 后卫 / 中场 / 前锋 / 中场/前锋（日本、瑞典原名单把中场和前锋合写，无法细分），全部球员均已推断出位置；
   - **存在重名**：塔勒比（摩洛哥、突尼斯各一）、阿里亚斯（哥伦比亚两人），所以详情页要按自增 id
     访问（`/player?id=`），按姓名查询时可能命中多人，应走搜索结果列表。
3. **（已完成）C 端建库**：新增 `db.c`（makefile 已加入 `db.o`），仿照 `CreateWordDB`，
   `main` 启动时若 `worldcup.db` 不存在就调用 `CreateWorldCupDB` 读两个 txt 建表导入：
   - 插入用 `sqlite3_prepare_v2` + 参数绑定（不拼 SQL），整个导入放在一个事务里；
   - 建了 `players(team_id)`、`players(name)` 两个索引，供第 4 步查询使用；
   - 导入失败会回滚并删除 `worldcup.db`，修好数据后重启即可重新导入；
   - **数据更新后**：重新运行导出脚本 → 删除 `worldcup.db` → 重启服务器。
4. **（已完成）业务模块替换**：新增 `player.c`、`style.css`，makefile 中 `goods.o` 改为 `player.o`，
   `head.h` 中商品结构体换成 `Team_t` / `TeamList_t` / `Player_t` / `PlayerList_t`：
   - `GET /` 首页：统计横幅 + 射手榜前 10（同进球并列名次）+ 12 个小组积分表；
   - `GET /team?id=fra`：球队资料（小组、主教练、FIFA 排名、身价、战绩）+ 按位置排序的大名单；
   - `GET /player?id=12`：球员资料（球队、位置、俱乐部、进球、助攻）+ 同队名单（本人高亮）；
   - `GET /search?name=`（及 `POST /search`）：姓名完全相同且不重名 → 302 跳详情页；
     否则按姓名 / 俱乐部 / 球队中英文名模糊匹配、球队代码 / 位置精确匹配 → 结果列表（最多 64 条）；都没有 → 提示页；
   - 动态页面不再内嵌 CSS，统一引用静态文件 `/style.css`；队徽为外链，加载失败时只显示球队名；
   - 已删除 `/goods`、`/buy` 路由和 `SendNoContent`，`main` 不再创建 `123.db`；
   - 顺带完成的加固：所有查询参数绑定、搜索词和用户名 HTML 转义、静态文件禁止 `..` 及
     `.db/.c/.h/.o/.mjs/.out`（原来 `mima.db` 可被直接下载）、空请求不再崩溃、注册成功带 `?reg=1` 提示。
5. **（已完成）清理**：移除 `goods.c`、`goods.o`、`goods.html`、`123.db`、`../images/`（223 个商品/支付图片），
   以及旧的生成页 `homepage.html`、旧可执行文件 `a.out` 和用旧源码编译的 `webserver.o`、`user.o`
   （下次 `make` 会全部重新生成）。现在 `work file` 目录只剩新网站需要的文件。
6. **（可选）加固**：密码加盐哈希存储、Cookie 签名（目前 `user=xxx` 可伪造）、
   生成页面改为每个请求独立文件或直接发送（为以后多线程做准备）。

## 四、编译运行（Linux）

```bash
make        # 生成 worldcup_server
make run    # 编译并启动，浏览器访问 http://<IP>:8080/
make clean  # 删除可执行文件和本项目 .o（保留编译很慢的 sqlite3.o）
```
