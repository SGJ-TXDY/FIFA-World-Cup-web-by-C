/*
 * player.c —— 球员 / 球队查询与页面生成（替代原来的 goods.c）
 *
 *   查询（全部使用 sqlite3_prepare_v2 + 参数绑定，用户输入不拼进 SQL）：
 *   - QueryPlayers / QueryTeams：执行查询，把结果读入列表；
 *   - FindPlayer / FindTeam：按 id 查询单个球员 / 球队；
 *   - FindExactPlayer：按姓名精确查询，唯一命中时返回球员 id；
 *   - QueryInt：执行只返回一个整数的统计查询；
 *   页面（生成到文件，再由 webserver.c 作为静态文件发送，样式统一引用 /style.css）：
 *   - CreateHomepageHtml：首页（射手榜 + 12 个小组积分表）；
 *   - CreateTeamHtml：球队页（球队资料 + 大名单）；
 *   - CreatePlayerHtml：球员页（球员资料 + 同队队员）；
 *   - CreateSearchHtml：搜索结果页（按姓名/俱乐部/球队/位置模糊匹配）。
 */
#include "head.h"
#include "sqlite3.h"

#define DB_NAME "worldcup.db"

/* 查询球员时统一使用的列（顺序与 ReadPlayerRow 一一对应） */
#define PLAYER_SELECT "select p.id,p.name,p.team_id,ifnull(t.name_zh,p.team_id)," \
                      "ifnull(p.position,''),ifnull(p.club,''),p.goals,p.assists," \
                      "ifnull(t.logo_url,'') " \
                      "from players p left join teams t on p.team_id=t.team_id "

/* 查询球队时统一使用的列（顺序与 ReadTeamRow 一一对应） */
#define TEAM_SELECT "select team_id,name_zh,name_en,group_name,coach,fifa_rank,market_value," \
                    "played,wins,draws,losses,points,goals_for,goals_against,logo_url from teams "

/* 大名单按 门将 -> 后卫 -> 中场 -> 中场/前锋 -> 前锋 排序 */
#define POSITION_ORDER "case p.position when '门将' then 1 when '后卫' then 2 " \
                       "when '中场' then 3 when '中场/前锋' then 4 when '前锋' then 5 else 6 end"

/* ======================== 数据查询 ======================== */

/* 读取查询结果中的文本列：NULL 视为空串，超长截断 */
void ColText(sqlite3_stmt *pstmt, int col, char *pdst, int maxlen)
{
    const unsigned char *psrc = sqlite3_column_text(pstmt, col);

    if (NULL == psrc)
    {
        pdst[0] = '\0';
        return;
    }
    strncpy(pdst, (const char *)psrc, maxlen - 1);
    pdst[maxlen - 1] = '\0';
}

/* 按 PLAYER_SELECT 的列顺序读取一行球员记录 */
void ReadPlayerRow(sqlite3_stmt *pstmt, Player_t *pplayer)
{
    pplayer->id = sqlite3_column_int(pstmt, 0);
    ColText(pstmt, 1, pplayer->name, sizeof(pplayer->name));
    ColText(pstmt, 2, pplayer->team_id, sizeof(pplayer->team_id));
    ColText(pstmt, 3, pplayer->team_name, sizeof(pplayer->team_name));
    ColText(pstmt, 4, pplayer->position, sizeof(pplayer->position));
    ColText(pstmt, 5, pplayer->club, sizeof(pplayer->club));
    pplayer->goals = sqlite3_column_int(pstmt, 6);
    pplayer->assists = sqlite3_column_int(pstmt, 7);
    ColText(pstmt, 8, pplayer->team_logo, sizeof(pplayer->team_logo));
}

/* 按 TEAM_SELECT 的列顺序读取一行球队记录（NULL 数字列读出为 0） */
void ReadTeamRow(sqlite3_stmt *pstmt, Team_t *pteam)
{
    ColText(pstmt, 0, pteam->id, sizeof(pteam->id));
    ColText(pstmt, 1, pteam->name_zh, sizeof(pteam->name_zh));
    ColText(pstmt, 2, pteam->name_en, sizeof(pteam->name_en));
    ColText(pstmt, 3, pteam->group, sizeof(pteam->group));
    ColText(pstmt, 4, pteam->coach, sizeof(pteam->coach));
    pteam->fifa_rank = sqlite3_column_int(pstmt, 5);
    pteam->market_value = sqlite3_column_double(pstmt, 6);
    pteam->played = sqlite3_column_int(pstmt, 7);
    pteam->wins = sqlite3_column_int(pstmt, 8);
    pteam->draws = sqlite3_column_int(pstmt, 9);
    pteam->losses = sqlite3_column_int(pstmt, 10);
    pteam->points = sqlite3_column_int(pstmt, 11);
    pteam->goals_for = sqlite3_column_int(pstmt, 12);
    pteam->goals_against = sqlite3_column_int(pstmt, 13);
    ColText(pstmt, 14, pteam->logo, sizeof(pteam->logo));
}

/*
 * 执行 SQL 并准备好语句
 * 功能：打开数据库、预编译 psql；parg 非空时绑定为 ?1（SQL 中可多次引用 ?1）
 * 返回：成功返回 0（*ppdb / *ppstmt 由调用者负责释放），失败返回 -1
 */
int PrepareQuery(const char *pDbName, const char *psql, const char *parg,
                 sqlite3 **ppdb, sqlite3_stmt **ppstmt)
{
    int ret = 0;

    ret = sqlite3_open_v2(pDbName, ppdb, SQLITE_OPEN_READONLY, NULL);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "sqlite3_open failed:%s\n", sqlite3_errmsg(*ppdb));
        sqlite3_close(*ppdb);
        return -1;
    }

    ret = sqlite3_prepare_v2(*ppdb, psql, -1, ppstmt, NULL);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "fail to sqlite3_prepare_v2:%s\n", sqlite3_errmsg(*ppdb));
        sqlite3_close(*ppdb);
        return -1;
    }

    if (NULL != parg)
    {
        sqlite3_bind_text(*ppstmt, 1, parg, -1, SQLITE_TRANSIENT);
    }

    return 0;
}

/*
 * 查询球员列表
 * 功能：psql 必须以 PLAYER_SELECT 开头；最多保存 MAX_LIST_NUM 条到 plist->items，
 *       plist->total 记录实际匹配的总条数（可能大于保存的条数）
 * 返回：保存的条数（>=0），出错返回 -1
 */
int QueryPlayers(const char *pDbName, const char *psql, const char *parg, PlayerList_t *plist)
{
    sqlite3 *pdb = NULL;
    sqlite3_stmt *pstmt = NULL;

    plist->count = 0;
    plist->total = 0;

    if (PrepareQuery(pDbName, psql, parg, &pdb, &pstmt) < 0)
    {
        return -1;
    }

    while (SQLITE_ROW == sqlite3_step(pstmt))
    {
        if (plist->count < MAX_LIST_NUM)
        {
            ReadPlayerRow(pstmt, &plist->items[plist->count]);
            plist->count++;
        }
        plist->total++;
    }

    sqlite3_finalize(pstmt);
    sqlite3_close(pdb);

    return plist->count;
}

/*
 * 查询球队列表
 * 功能：psql 必须以 TEAM_SELECT 开头；最多保存 MAX_TEAM_NUM 条
 * 返回：保存的条数（>=0），出错返回 -1
 */
int QueryTeams(const char *pDbName, const char *psql, const char *parg, TeamList_t *plist)
{
    sqlite3 *pdb = NULL;
    sqlite3_stmt *pstmt = NULL;

    plist->count = 0;

    if (PrepareQuery(pDbName, psql, parg, &pdb, &pstmt) < 0)
    {
        return -1;
    }

    while (SQLITE_ROW == sqlite3_step(pstmt) && plist->count < MAX_TEAM_NUM)
    {
        ReadTeamRow(pstmt, &plist->items[plist->count]);
        plist->count++;
    }

    sqlite3_finalize(pstmt);
    sqlite3_close(pdb);

    return plist->count;
}

/* 执行只返回一个整数的查询（如 count(*)），出错返回 0 */
int QueryInt(const char *pDbName, const char *psql)
{
    sqlite3 *pdb = NULL;
    sqlite3_stmt *pstmt = NULL;
    int value = 0;

    if (PrepareQuery(pDbName, psql, NULL, &pdb, &pstmt) < 0)
    {
        return 0;
    }

    if (SQLITE_ROW == sqlite3_step(pstmt))
    {
        value = sqlite3_column_int(pstmt, 0);
    }

    sqlite3_finalize(pstmt);
    sqlite3_close(pdb);

    return value;
}

/*
 * 按 id 查询单个球员
 * 参数：pid - 球员 id（来自 URL，字符串形式）；pplayer - 保存查询结果
 * 返回：查到返回 1，未查到返回 0，出错返回 -1
 */
int FindPlayer(const char *pDbName, const char *pid, Player_t *pplayer)
{
    PlayerList_t list;
    int ret = 0;

    ret = QueryPlayers(pDbName, PLAYER_SELECT "where p.id=cast(?1 as integer);", pid, &list);
    if (ret <= 0)
    {
        return ret;
    }

    memcpy(pplayer, &list.items[0], sizeof(Player_t));

    return 1;
}

/*
 * 按球队代码查询单个球队
 * 返回：查到返回 1，未查到返回 0，出错返回 -1
 */
int FindTeam(const char *pDbName, const char *pid, Team_t *pteam)
{
    TeamList_t list;
    int ret = 0;

    ret = QueryTeams(pDbName, TEAM_SELECT "where team_id=lower(?1);", pid, &list);
    if (ret <= 0)
    {
        return ret;
    }

    memcpy(pteam, &list.items[0], sizeof(Team_t));

    return 1;
}

/*
 * 按姓名精确查询球员
 * 功能：数据中存在重名（如两名"阿里亚斯"），只有唯一命中时才返回其 id，
 *       用于搜索时直接跳转详情页；重名或未命中则交给搜索结果页处理
 * 返回：唯一命中返回球员 id（>0），否则返回 0
 */
int FindExactPlayer(const char *pDbName, const char *pname)
{
    PlayerList_t list;

    if (QueryPlayers(pDbName, PLAYER_SELECT "where p.name=?1;", pname, &list) < 0)
    {
        return 0;
    }

    return (1 == list.total) ? list.items[0].id : 0;
}

/* ======================== 页面输出工具 ======================== */

/* 输出 HTML 转义后的文本：用户输入（搜索词、用户名）原样输出会被当成 HTML 执行 */
void FputsHtml(FILE *fp, const char *psrc)
{
    while ('\0' != *psrc)
    {
        switch (*psrc)
        {
            case '<':  fputs("&lt;", fp);   break;
            case '>':  fputs("&gt;", fp);   break;
            case '&':  fputs("&amp;", fp);  break;
            case '"':  fputs("&quot;", fp); break;
            case '\'': fputs("&#39;", fp);  break;
            default:   fputc(*psrc, fp);    break;
        }
        psrc++;
    }
}

/* 位置对应的标签样式 */
const char *PositionClass(const char *pposition)
{
    if (0 == strcmp(pposition, "门将")) return "pos pos-gk";
    if (0 == strcmp(pposition, "后卫")) return "pos pos-df";
    if (0 == strcmp(pposition, "中场")) return "pos pos-mf";
    if (0 == strcmp(pposition, "前锋")) return "pos pos-fw";
    return "pos";
}

/* 取 UTF-8 字符串的第一个字符（球员头像占位用） */
void FirstChar(char *pdst, const char *psrc, int maxlen)
{
    int i = 0;

    if ('\0' != psrc[0])
    {
        pdst[i++] = psrc[0];
        //UTF-8 后续字节形如 10xxxxxx
        while (i < maxlen - 1 && (psrc[i] & 0xC0) == 0x80)
        {
            pdst[i] = psrc[i];
            i++;
        }
    }
    pdst[i] = '\0';
}

/* 输出"队徽 + 球队名"链接；队徽是外链图片，加载失败时自动移除，只留文字 */
void WriteTeamLink(FILE *fp, const char *pid, const char *pname, const char *plogo)
{
    fprintf(fp, "<a class=\"team-cell\" href=\"/team?id=%s\">", pid);
    if ('\0' != plogo[0])
    {
        fprintf(fp, "<img class=\"crest\" src=\"%s\" alt=\"\" onerror=\"this.remove()\">", plogo);
    }
    fprintf(fp, "%s</a>", pname);
}

/*
 * 输出页面公共头部：<head>、顶栏（logo、搜索框、导航、登录状态）
 * 参数：ptitle - 页面标题（会转义）；puser - 当前登录用户（未登录为空串）；
 *       pkeyword - 回填到搜索框的关键词，可为 NULL
 */
void WritePageBegin(FILE *fp, const char *ptitle, const char *puser, const char *pkeyword)
{
    fprintf(fp, "<!DOCTYPE html>"
                "<html lang=\"zh-CN\">"
                "<head><meta charset=\"utf-8\">"
                "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
                "<title>");
    FputsHtml(fp, ptitle);
    fprintf(fp, " - 世界杯球员库</title>"
                "<link rel=\"stylesheet\" href=\"/style.css\">"
                "</head><body>");

    fprintf(fp, "<header class=\"topbar\"><div class=\"topbar-inner\">"
                "<a class=\"brand\" href=\"/\">"
                "<svg viewBox=\"0 0 36 36\" fill=\"none\" aria-hidden=\"true\">"
                "<circle cx=\"18\" cy=\"18\" r=\"17\" fill=\"#008a60\"/>"
                "<path d=\"M18 7L25 12L22 21H14L11 12L18 7Z\" fill=\"#fff\"/>"
                "<path d=\"M14 21L9 27M22 21L27 27M11 12L5 14M25 12L31 14\" "
                "stroke=\"#fff\" stroke-width=\"2\" stroke-linecap=\"round\"/></svg>"
                "世界杯球员库</a>"
                "<form class=\"search\" action=\"/search\" method=\"get\">"
                "<input type=\"text\" name=\"name\" value=\"");
    if (NULL != pkeyword)
    {
        FputsHtml(fp, pkeyword);
    }
    fprintf(fp, "\" placeholder=\"搜索球员 / 球队 / 俱乐部 / 位置\">"
                "<button type=\"submit\">搜索</button></form>"
                "<nav class=\"nav\">"
                "<a href=\"/#scorers\">射手榜</a>"
                "<a href=\"/#groups\">小组积分</a>");

    if (NULL != puser && '\0' != puser[0])
    {
        fprintf(fp, "<span class=\"user\">你好，");
        FputsHtml(fp, puser);
        fprintf(fp, "</span><a href=\"/logout\">退出</a>");
    }
    else
    {
        fprintf(fp, "<a class=\"login\" href=\"/index.html\">登录</a>");
    }

    fprintf(fp, "</nav></div></header><main>");
}

/* 输出页面公共尾部 */
void WritePageEnd(FILE *fp)
{
    fprintf(fp, "</main>"
                "<footer>数据来源：football-data.org 及手工整理的参赛名单 · "
                "世界杯球员库（C 语言 WebServer）</footer>"
                "</body></html>");
}

/* 输出"未找到"提示（ptip 不含用户输入，不转义） */
void WriteEmpty(FILE *fp, const char *ptitle, const char *ptip)
{
    fprintf(fp, "<div class=\"container\"><div class=\"empty\">"
                "<div class=\"icon\">⚽</div>"
                "<h1>%s</h1><p>%s</p>"
                "<a class=\"btn\" href=\"/\">返回首页</a>"
                "</div></div>", ptitle, ptip);
}

/*
 * 输出球员表格
 * 参数：showteam - 是否显示"球队"列（球队页的大名单不需要）；
 *       currentid - 需要高亮的球员 id（球员页中的本人），不需要时传 0
 */
void WritePlayerTable(FILE *fp, PlayerList_t *plist, int showteam, int currentid)
{
    int i = 0;
    Player_t *pp = NULL;

    fprintf(fp, "<div class=\"card table-wrap\"><table><thead><tr>"
                "<th>球员</th>%s<th>位置</th><th>俱乐部</th>"
                "<th class=\"num\">进球</th><th class=\"num\">助攻</th>"
                "</tr></thead><tbody>",
                showteam ? "<th>球队</th>" : "");

    for (i = 0; i < plist->count; i++)
    {
        pp = &plist->items[i];

        fprintf(fp, "<tr%s><td class=\"strong\"><a href=\"/player?id=%d\">%s</a></td>",
                    (pp->id == currentid) ? " class=\"current\"" : "", pp->id, pp->name);
        if (showteam)
        {
            fprintf(fp, "<td>");
            WriteTeamLink(fp, pp->team_id, pp->team_name, pp->team_logo);
            fprintf(fp, "</td>");
        }
        fprintf(fp, "<td><span class=\"%s\">%s</span></td>"
                    "<td class=\"muted\">%s</td>"
                    "<td class=\"num\">%d</td><td class=\"num\">%d</td></tr>",
                    PositionClass(pp->position),
                    ('\0' != pp->position[0]) ? pp->position : "未知",
                    pp->club, pp->goals, pp->assists);
    }

    fprintf(fp, "</tbody></table></div>");
}

/* ======================== 页面生成 ======================== */

/*
 * 生成首页
 * 功能：横幅（球队/球员/小组/进球统计）、射手榜前 10、12 个小组积分表
 *       （同组按 积分 -> 净胜球 -> 进球 排序）
 * 参数：pfilename - 要生成的 HTML 文件名；puser - 当前登录用户
 * 返回：成功返回 0，失败（如数据库不存在）返回 -1
 */
int CreateHomepageHtml(const char *pfilename, const char *puser)
{
    TeamList_t teams;
    PlayerList_t scorers;
    Team_t *pt = NULL;
    Player_t *pp = NULL;
    FILE *fp = NULL;
    char group[8] = {0};
    int rank = 0;
    int i = 0;

    if (QueryTeams(DB_NAME, TEAM_SELECT "order by group_name,points desc,"
                   "goals_for-goals_against desc,goals_for desc,fifa_rank;", NULL, &teams) <= 0)
    {
        return -1;
    }
    if (QueryPlayers(DB_NAME, PLAYER_SELECT "where p.goals>0 "
                     "order by p.goals desc,p.assists desc,p.id limit 10;", NULL, &scorers) < 0)
    {
        return -1;
    }

    fp = fopen(pfilename, "w");
    if (NULL == fp)
    {
        perror("fail to fopen");
        return -1;
    }

    WritePageBegin(fp, "首页", puser, NULL);

    //横幅
    fprintf(fp, "<section class=\"hero\"><div class=\"hero-inner\">"
                "<p class=\"eyebrow\">2026 FIFA WORLD CUP</p>"
                "<h1>世界杯球员基础信息查询</h1>"
                "<p>输入球员、球队、俱乐部或位置，快速查看参赛球员的基础资料</p>"
                "<div class=\"hero-stats\">"
                "<div><b>%d</b><span>参赛球队</span></div>"
                "<div><b>%d</b><span>参赛球员</span></div>"
                "<div><b>%d</b><span>小组</span></div>"
                "<div><b>%d</b><span>球员进球</span></div>"
                "</div></div></section>",
                teams.count,
                QueryInt(DB_NAME, "select count(*) from players;"),
                QueryInt(DB_NAME, "select count(distinct group_name) from teams;"),
                QueryInt(DB_NAME, "select ifnull(sum(goals),0) from players;"));

    fprintf(fp, "<div class=\"container\">");

    //射手榜：进球数相同的球员名次并列
    fprintf(fp, "<section class=\"section\" id=\"scorers\">"
                "<div class=\"section-head\"><p class=\"eyebrow\">TOP SCORERS</p>"
                "<h2>射手榜</h2><p>按进球数排序，进球相同按助攻排序</p></div>"
                "<div class=\"card table-wrap\"><table><thead><tr>"
                "<th>名次</th><th>球员</th><th>球队</th><th>位置</th>"
                "<th class=\"num\">进球</th><th class=\"num\">助攻</th>"
                "</tr></thead><tbody>");
    for (i = 0; i < scorers.count; i++)
    {
        pp = &scorers.items[i];
        if (0 == i || pp->goals != scorers.items[i - 1].goals)
        {
            rank = i + 1;
        }

        fprintf(fp, "<tr><td><span class=\"rank%s\">%d</span></td>"
                    "<td class=\"strong\"><a href=\"/player?id=%d\">%s</a></td><td>",
                    (rank <= 3) ? " top" : "", rank, pp->id, pp->name);
        WriteTeamLink(fp, pp->team_id, pp->team_name, pp->team_logo);
        fprintf(fp, "</td><td><span class=\"%s\">%s</span></td>"
                    "<td class=\"num strong\">%d</td><td class=\"num\">%d</td></tr>",
                    PositionClass(pp->position), pp->position, pp->goals, pp->assists);
    }
    fprintf(fp, "</tbody></table></div></section>");

    //小组积分：球队已按小组排序，小组变化时开始一张新表
    fprintf(fp, "<section class=\"section\" id=\"groups\">"
                "<div class=\"section-head\"><p class=\"eyebrow\">GROUP STAGE</p>"
                "<h2>小组积分</h2><p>点击球队查看大名单</p></div>"
                "<div class=\"groups\">");
    for (i = 0; i < teams.count; i++)
    {
        pt = &teams.items[i];

        if (0 != strcmp(group, pt->group))
        {
            if ('\0' != group[0])
            {
                fprintf(fp, "</tbody></table></div>");
            }
            strncpy(group, pt->group, sizeof(group) - 1);

            fprintf(fp, "<div class=\"card\">"
                        "<div class=\"group-title\"><span>%s</span>组</div>"
                        "<div class=\"table-wrap\"><table><thead><tr>"
                        "<th>球队</th><th class=\"num\">赛</th><th class=\"num\">胜</th>"
                        "<th class=\"num\">平</th><th class=\"num\">负</th>"
                        "<th class=\"num\">净胜</th><th class=\"num\">积分</th>"
                        "</tr></thead><tbody>", pt->group);
        }

        fprintf(fp, "<tr><td>");
        WriteTeamLink(fp, pt->id, pt->name_zh, pt->logo);
        fprintf(fp, "</td><td class=\"num\">%d</td><td class=\"num\">%d</td>"
                    "<td class=\"num\">%d</td><td class=\"num\">%d</td>"
                    "<td class=\"num\">%+d</td><td class=\"num strong\">%d</td></tr>",
                    pt->played, pt->wins, pt->draws, pt->losses,
                    pt->goals_for - pt->goals_against, pt->points);
    }
    fprintf(fp, "</tbody></table></div></div></section>");

    fprintf(fp, "</div>");

    WritePageEnd(fp);
    fclose(fp);

    return 0;
}

/*
 * 生成球队页
 * 功能：球队资料（小组、主教练、FIFA 排名、身价、战绩）和按位置排序的大名单
 * 参数：pid - 球队代码（来自 URL，如 "fra"）；pfilename - 要生成的 HTML 文件名；
 *       puser - 当前登录用户
 * 返回：成功返回 0（球队不存在时生成提示页），失败返回 -1
 */
int CreateTeamHtml(const char *pid, const char *pfilename, const char *puser)
{
    Team_t team;
    PlayerList_t roster;
    FILE *fp = NULL;
    int ret = 0;

    memset(&team, 0, sizeof(team));

    ret = FindTeam(DB_NAME, pid, &team);
    if (ret < 0)
    {
        return -1;
    }

    fp = fopen(pfilename, "w");
    if (NULL == fp)
    {
        perror("fail to fopen");
        return -1;
    }

    if (0 == ret)
    {
        WritePageBegin(fp, "未找到球队", puser, NULL);
        WriteEmpty(fp, "未找到该球队", "球队代码有误，可以在首页的小组积分中选择球队");
        WritePageEnd(fp);
        fclose(fp);
        return 0;
    }

    QueryPlayers(DB_NAME, PLAYER_SELECT "where p.team_id=?1 "
                 "order by " POSITION_ORDER ",p.goals desc,p.assists desc,p.id;",
                 team.id, &roster);

    WritePageBegin(fp, team.name_zh, puser, NULL);

    fprintf(fp, "<div class=\"container\">"
                "<p class=\"crumbs\"><a href=\"/\">首页</a> / <a href=\"/#groups\">%s 组</a> / %s</p>",
                team.group, team.name_zh);

    //资料头
    fprintf(fp, "<section class=\"profile\"><div class=\"avatar light\">");
    if ('\0' != team.logo[0])
    {
        fprintf(fp, "<img src=\"%s\" alt=\"%s\" onerror=\"this.remove()\">", team.logo, team.name_zh);
    }
    fprintf(fp, "</div><div class=\"profile-main\">"
                "<h1>%s</h1>"
                "<p class=\"sub\">%s · %s 组 · 主教练 %s</p>"
                "</div></section>",
                team.name_zh, team.name_en, team.group,
                ('\0' != team.coach[0]) ? team.coach : "暂无数据");

    //数据卡片
    fprintf(fp, "<div class=\"tiles\">");
    if (team.fifa_rank > 0)
    {
        fprintf(fp, "<div class=\"tile\"><span>FIFA 排名</span><b>%d</b></div>", team.fifa_rank);
    }
    if (team.market_value > 0)
    {
        fprintf(fp, "<div class=\"tile\"><span>总身价</span><b>%.2f 亿</b></div>", team.market_value);
    }
    fprintf(fp, "<div class=\"tile\"><span>小组赛积分</span><b>%d</b></div>"
                "<div class=\"tile\"><span>胜 / 平 / 负</span><b>%d / %d / %d</b></div>"
                "<div class=\"tile\"><span>进球 / 失球</span><b>%d / %d</b></div>"
                "<div class=\"tile\"><span>大名单人数</span><b>%d</b></div>"
                "</div>",
                team.points, team.wins, team.draws, team.losses,
                team.goals_for, team.goals_against, roster.total);

    //大名单
    fprintf(fp, "<section class=\"section\">"
                "<div class=\"section-head\"><p class=\"eyebrow\">SQUAD</p>"
                "<h2>大名单</h2><p>按 门将 / 后卫 / 中场 / 前锋 排列</p></div>");
    WritePlayerTable(fp, &roster, 0, 0);
    fprintf(fp, "</section></div>");

    WritePageEnd(fp);
    fclose(fp);

    return 0;
}

/*
 * 生成球员页
 * 功能：球员资料（球队、位置、俱乐部、进球、助攻）和同队队员名单（本人高亮）
 * 参数：pid - 球员 id（来自 URL）；pfilename - 要生成的 HTML 文件名；
 *       puser - 当前登录用户
 * 返回：成功返回 0（球员不存在时生成提示页），失败返回 -1
 */
int CreatePlayerHtml(const char *pid, const char *pfilename, const char *puser)
{
    Player_t player;
    Team_t team;
    PlayerList_t roster;
    FILE *fp = NULL;
    char initial[8] = {0};
    int ret = 0;

    memset(&player, 0, sizeof(player));
    memset(&team, 0, sizeof(team));

    ret = FindPlayer(DB_NAME, pid, &player);
    if (ret < 0)
    {
        return -1;
    }

    fp = fopen(pfilename, "w");
    if (NULL == fp)
    {
        perror("fail to fopen");
        return -1;
    }

    if (0 == ret)
    {
        WritePageBegin(fp, "未找到球员", puser, NULL);
        WriteEmpty(fp, "未找到该球员", "球员编号有误，可以通过顶部搜索框查找球员");
        WritePageEnd(fp);
        fclose(fp);
        return 0;
    }

    FindTeam(DB_NAME, player.team_id, &team);
    QueryPlayers(DB_NAME, PLAYER_SELECT "where p.team_id=?1 "
                 "order by " POSITION_ORDER ",p.goals desc,p.assists desc,p.id;",
                 player.team_id, &roster);
    FirstChar(initial, player.name, sizeof(initial));

    WritePageBegin(fp, player.name, puser, NULL);

    fprintf(fp, "<div class=\"container\">"
                "<p class=\"crumbs\"><a href=\"/\">首页</a> / "
                "<a href=\"/team?id=%s\">%s</a> / %s</p>",
                player.team_id, player.team_name, player.name);

    //资料头
    fprintf(fp, "<section class=\"profile\">"
                "<div class=\"avatar\">%s</div>"
                "<div class=\"profile-main\">"
                "<h1>%s</h1><p class=\"sub\">",
                initial, player.name);
    WriteTeamLink(fp, player.team_id, player.team_name, player.team_logo);
    fprintf(fp, " · %s</p>"
                "<div class=\"tags\"><span class=\"%s\">%s</span></div>"
                "</div></section>",
                player.club, PositionClass(player.position),
                ('\0' != player.position[0]) ? player.position : "位置未知");

    //数据卡片
    fprintf(fp, "<div class=\"tiles\">"
                "<div class=\"tile\"><span>世界杯进球</span><b>%d</b></div>"
                "<div class=\"tile\"><span>助攻</span><b>%d</b></div>"
                "<div class=\"tile\"><span>效力俱乐部</span><b class=\"small\">%s</b></div>",
                player.goals, player.assists, player.club);
    if ('\0' != team.id[0])
    {
        fprintf(fp, "<div class=\"tile\"><span>所在小组</span><b>%s 组</b></div>", team.group);
        if (team.fifa_rank > 0)
        {
            fprintf(fp, "<div class=\"tile\"><span>球队 FIFA 排名</span><b>%d</b></div>",
                        team.fifa_rank);
        }
    }
    fprintf(fp, "</div>");

    //同队队员
    fprintf(fp, "<section class=\"section\">"
                "<div class=\"section-head\"><p class=\"eyebrow\">TEAMMATES</p>"
                "<h2>%s 大名单</h2><p>共 %d 人，当前球员已高亮</p></div>",
                player.team_name, roster.total);
    WritePlayerTable(fp, &roster, 0, player.id);
    fprintf(fp, "</section></div>");

    WritePageEnd(fp);
    fclose(fp);

    return 0;
}

/*
 * 生成搜索结果页
 * 功能：按 姓名 / 俱乐部 / 球队中英文名 模糊匹配，或按 球队代码 / 位置 精确匹配；
 *       姓名完全相同的排在最前，其余按进球、助攻排序；最多展示 MAX_LIST_NUM 条
 *       （唯一同名直接跳详情页的逻辑在 webserver.c 中，调用 FindExactPlayer）
 * 参数：pkeyword - 搜索关键词（已 URL 解码）；pfilename - 要生成的 HTML 文件名；
 *       puser - 当前登录用户
 * 返回：成功返回 0，失败返回 -1
 */
int CreateSearchHtml(const char *pkeyword, const char *pfilename, const char *puser)
{
    PlayerList_t list;
    FILE *fp = NULL;

    if (QueryPlayers(DB_NAME, PLAYER_SELECT
                     "where p.name like '%'||?1||'%' or p.club like '%'||?1||'%' "
                     "or t.name_zh like '%'||?1||'%' or t.name_en like '%'||?1||'%' "
                     "or p.team_id=lower(?1) or p.position=?1 "
                     "order by (p.name=?1) desc,p.goals desc,p.assists desc,p.id;",
                     pkeyword, &list) < 0)
    {
        return -1;
    }

    fp = fopen(pfilename, "w");
    if (NULL == fp)
    {
        perror("fail to fopen");
        return -1;
    }

    WritePageBegin(fp, pkeyword, puser, pkeyword);

    if (0 == list.total)
    {
        //提示中含有用户输入，不能直接用 WriteEmpty
        fprintf(fp, "<div class=\"container\"><div class=\"empty\">"
                    "<div class=\"icon\">🔍</div>"
                    "<h1>没有找到相关球员</h1><p>没有与「");
        FputsHtml(fp, pkeyword);
        fprintf(fp, "」相关的球员，<br>可以试试球员名、球队名（如 法国）、俱乐部或位置（如 前锋）</p>"
                    "<a class=\"btn\" href=\"/\">返回首页</a>"
                    "</div></div>");
    }
    else
    {
        fprintf(fp, "<div class=\"container\"><p class=\"result-tip\">共找到 <b>%d</b> 名与「",
                    list.total);
        FputsHtml(fp, pkeyword);
        fprintf(fp, "」相关的球员");
        if (list.total > list.count)
        {
            fprintf(fp, "，以下显示前 %d 名", list.count);
        }
        fprintf(fp, "</p>");
        WritePlayerTable(fp, &list, 1, 0);
        fprintf(fp, "</div>");
    }

    WritePageEnd(fp);
    fclose(fp);

    return 0;
}
