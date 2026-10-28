/*
 * db.c —— 世界杯数据库建库与数据导入
 *
 *   - CreateWorldCupDB：创建 worldcup.db（teams / players 两张表），
 *     并从 teams.txt、players.txt 导入数据（仿照 webserver_wordmean 的 CreateWordDB）；
 *   - ImportTxtFile：逐行读取 TAB 分隔的文本，用预编译语句批量插入；
 *   - SplitLine：按 TAB 拆分一行文本。
 *
 * 两个 txt 由 tools/export-data.mjs 从参考站数据导出：UTF-8 编码，
 * 每行一条记录，字段之间用 TAB 分隔，以 '#' 开头的行为字段说明（跳过）。
 */
#include "head.h"
#include "sqlite3.h"

#define MAX_FIELD_NUM 16        /* 每行最多字段数（teams.txt 为 15 列） */

/*
 * 按 TAB 拆分一行文本
 * 功能：去掉行尾的 "\r\n"，再把每个 '\t' 改写为 '\0'，
 *       pfields[i] 依次指向各字段的起始位置（直接指向 pline 内部，不另外分配）
 * 参数：pline - 待拆分的一行（会被修改）；pfields - 保存各字段指针；
 *       maxfields - pfields 的容量
 * 返回：拆分出的字段个数
 */
int SplitLine(char *pline, char **pfields, int maxfields)
{
    int count = 0;
    char *ptmp = NULL;

    //去掉行尾换行（Windows 下生成的文件可能带 '\r'）
    ptmp = pline + strlen(pline);
    while (ptmp > pline && ('\n' == *(ptmp - 1) || '\r' == *(ptmp - 1)))
    {
        ptmp--;
        *ptmp = '\0';
    }

    ptmp = pline;
    pfields[count++] = ptmp;
    while (*ptmp != '\0' && count < maxfields)
    {
        if ('\t' == *ptmp)
        {
            *ptmp = '\0';
            pfields[count++] = ptmp + 1;
        }
        ptmp++;
    }

    return count;
}

/*
 * 从 TAB 分隔的文本文件导入数据
 * 功能：用 sqlite3_prepare_v2 预编译 psql（形如 "insert into ... values(?,?,...)"），
 *       逐行读取文件、拆分字段后用 sqlite3_bind_text 绑定参数再执行；
 *       空字段绑定为 NULL，数字列由 sqlite 按列类型自动转换。
 *       参数绑定不需要拼接 SQL，球员名、俱乐部名中出现引号也不会出错
 * 参数：pdb - 已打开的数据库；pFileName - 文本文件名；
 *       psql - 插入语句；nfields - 每行应有的字段数（与 ? 个数一致）
 * 返回：成功返回导入的行数，失败返回 -1
 */
int ImportTxtFile(sqlite3 *pdb, const char *pFileName, const char *psql, int nfields)
{
    FILE *fp = NULL;
    sqlite3_stmt *pstmt = NULL;
    char tmpbuff[4096] = {0};
    char *pfields[MAX_FIELD_NUM] = {NULL};
    int count = 0;
    int lineno = 0;
    int ret = 0;
    int i = 0;

    fp = fopen(pFileName, "r");
    if (NULL == fp)
    {
        perror("fail to fopen");
        fprintf(stderr, "缺少 %s，请先运行：node tools/export-data.mjs\n", pFileName);
        return -1;
    }

    ret = sqlite3_prepare_v2(pdb, psql, -1, &pstmt, NULL);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "fail to sqlite3_prepare_v2:%s\n", sqlite3_errmsg(pdb));
        fclose(fp);
        return -1;
    }

    while (NULL != fgets(tmpbuff, sizeof(tmpbuff), fp))
    {
        lineno++;

        //跳过字段说明行和空行
        if ('#' == tmpbuff[0] || '\n' == tmpbuff[0] || '\r' == tmpbuff[0])
        {
            continue;
        }

        if (SplitLine(tmpbuff, pfields, MAX_FIELD_NUM) != nfields)
        {
            fprintf(stderr, "%s 第 %d 行字段数不是 %d，已跳过\n", pFileName, lineno, nfields);
            continue;
        }

        //绑定参数（下标从 1 开始），空串按 NULL 处理
        for (i = 0; i < nfields; i++)
        {
            if ('\0' == pfields[i][0])
            {
                sqlite3_bind_null(pstmt, i + 1);
            }
            else
            {
                sqlite3_bind_text(pstmt, i + 1, pfields[i], -1, SQLITE_TRANSIENT);
            }
        }

        ret = sqlite3_step(pstmt);
        if (ret != SQLITE_DONE)
        {
            fprintf(stderr, "%s 第 %d 行插入失败:%s\n", pFileName, lineno, sqlite3_errmsg(pdb));
            sqlite3_finalize(pstmt);
            fclose(fp);
            return -1;
        }

        sqlite3_reset(pstmt);
        sqlite3_clear_bindings(pstmt);
        count++;
    }

    sqlite3_finalize(pstmt);
    fclose(fp);

    return count;
}

/*
 * 创建世界杯数据库并导入数据
 * 功能：1) 打开（不存在则创建）数据库，创建 teams、players 两张表及索引；
 *       2) 在一个事务中导入 teams.txt、players.txt（事务能大幅提高插入速度）；
 *       3) 任一步失败则回滚并删除数据库文件，避免下次启动时
 *          因为文件已存在而跳过建库，留下一个不完整的库
 * 参数：pDbName - 数据库文件名（"worldcup.db"）；
 *       pTeamFile - 球队数据文件（"teams.txt"）；pPlayerFile - 球员数据文件（"players.txt"）
 * 返回：成功返回 0，失败返回 -1
 */
int CreateWorldCupDB(const char *pDbName, const char *pTeamFile, const char *pPlayerFile)
{
    sqlite3 *pdb = NULL;
    char *perrmsg = NULL;
    int ret = 0;
    int nteams = 0;
    int nplayers = 0;

    const char *pcreate =
        "create table if not exists teams("
        "team_id text primary key,"         /* 球队代码，如 fra */
        "name_zh text,"                     /* 中文名 */
        "name_en text,"                     /* 英文名 */
        "group_name text,"                  /* 小组 A ~ L */
        "coach text,"                       /* 主教练 */
        "fifa_rank integer,"                /* FIFA 排名 */
        "market_value real,"                /* 总身价（亿） */
        "played integer,"                   /* 场次 */
        "wins integer,"
        "draws integer,"
        "losses integer,"
        "points integer,"                   /* 积分 */
        "goals_for integer,"                /* 进球 */
        "goals_against integer,"            /* 失球 */
        "logo_url text);"                   /* 队徽链接 */
        "create table if not exists players("
        "id integer primary key autoincrement,"
        "name text not null,"               /* 姓名 */
        "team_id text,"                     /* 所属球队，关联 teams.team_id */
        "position text,"                    /* 门将/后卫/中场/前锋/中场/前锋 */
        "club text,"                        /* 俱乐部 */
        "goals integer default 0,"          /* 进球 */
        "assists integer default 0);"       /* 助攻 */
        "create index if not exists idx_players_team on players(team_id);"
        "create index if not exists idx_players_name on players(name);";

    printf("正在创建数据库 %s ...\n", pDbName);

    ret = sqlite3_open(pDbName, &pdb);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "sqlite3_open failed:%s\n", sqlite3_errmsg(pdb));
        sqlite3_close(pdb);
        return -1;
    }

    ret = sqlite3_exec(pdb, pcreate, NULL, NULL, &perrmsg);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "fail to sqlite3_exec:%s\n", perrmsg);
        sqlite3_free(perrmsg);
        goto fail;
    }

    ret = sqlite3_exec(pdb, "BEGIN TRANSACTION;", NULL, NULL, &perrmsg);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "fail to sqlite3_exec:%s\n", perrmsg);
        sqlite3_free(perrmsg);
        goto fail;
    }

    nteams = ImportTxtFile(pdb, pTeamFile,
                           "insert into teams values(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);", 15);
    if (nteams < 0)
    {
        goto fail;
    }

    nplayers = ImportTxtFile(pdb, pPlayerFile,
                             "insert into players(name,team_id,position,club,goals,assists) "
                             "values(?,?,?,?,?,?);", 6);
    if (nplayers < 0)
    {
        goto fail;
    }

    ret = sqlite3_exec(pdb, "COMMIT;", NULL, NULL, &perrmsg);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "fail to sqlite3_exec:%s\n", perrmsg);
        sqlite3_free(perrmsg);
        goto fail;
    }

    sqlite3_close(pdb);

    printf("加载完成：%d 支球队，%d 名球员\n", nteams, nplayers);

    return 0;

fail:
    sqlite3_exec(pdb, "ROLLBACK;", NULL, NULL, NULL);
    sqlite3_close(pdb);
    remove(pDbName);
    fprintf(stderr, "创建数据库 %s 失败\n", pDbName);
    return -1;
}
