#ifndef __HEAD_H__
#define __HEAD_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>

#define MAX_LIST_NUM 64         /* 搜索结果 / 球队大名单最多展示的球员条数 */
#define MAX_TEAM_NUM 64         /* 最多球队数（实际 48 支） */

/* HTTP 请求报文解析结构体：保存解析出的请求方法、URL 和请求体 */
typedef struct HttpRequest
{
    char *pmethod;              /* 请求方法，如 "GET" / "POST" */
    char *purl;                 /* 请求路径，如 "/" / "/player?id=12" */
    char *pcontent;             /* 请求体（POST 表单数据），GET 请求一般为空 */
}HttpRequest_t;

/* 球队信息结构体：保存 teams 表的查询结果 */
typedef struct Team
{
    char id[8];                 /* team_id       球队代码，如 fra */
    char name_zh[64];           /* name_zh       中文名 */
    char name_en[64];           /* name_en       英文名 */
    char group[8];              /* group_name    小组 A ~ L */
    char coach[64];             /* coach         主教练 */
    int fifa_rank;              /* fifa_rank     FIFA 排名（无数据为 0） */
    double market_value;        /* market_value  总身价，单位亿（无数据为 0） */
    int played;                 /* played        场次 */
    int wins;                   /* wins          胜 */
    int draws;                  /* draws         平 */
    int losses;                 /* losses        负 */
    int points;                 /* points        积分 */
    int goals_for;              /* goals_for     进球 */
    int goals_against;          /* goals_against 失球 */
    char logo[256];             /* logo_url      队徽链接 */
}Team_t;

/* 球队列表 */
typedef struct TeamList
{
    Team_t items[MAX_TEAM_NUM];
    int count;                  /* 实际条数 */
}TeamList_t;

/* 球员信息结构体：保存 players 表（左连接 teams 表）的查询结果 */
typedef struct Player
{
    int id;                     /* id        球员编号 */
    char name[96];              /* name      姓名 */
    char team_id[8];            /* team_id   所属球队代码 */
    char team_name[64];         /* name_zh   所属球队中文名 */
    char team_logo[256];        /* logo_url  所属球队队徽 */
    char position[32];          /* position  位置 */
    char club[128];             /* club      俱乐部 */
    int goals;                  /* goals     进球 */
    int assists;                /* assists   助攻 */
}Player_t;

/* 球员列表：items 最多保存 MAX_LIST_NUM 条，total 为实际匹配总数 */
typedef struct PlayerList
{
    Player_t items[MAX_LIST_NUM];
    int count;                  /* 保存的条数 */
    int total;                  /* 匹配的总条数 */
}PlayerList_t;

/* webserver.c 中的公共工具函数（供 user.c / player.c 调用） */
extern int HexVal(char ch);
extern void UrlDecode(char *pdst, const char *psrc, int maxlen);

/* db.c：世界杯数据库建库与数据导入（ImportTxtFile 用到 sqlite3 类型，仅在 db.c 内部使用） */
extern int SplitLine(char *pline, char **pfields, int maxfields);
extern int CreateWorldCupDB(const char *pDbName, const char *pTeamFile, const char *pPlayerFile);

/* user.c：注册、登录相关函数 */
extern int CreateUserTable(const char *pDbName);
extern int UserCallback(void *arg, int colnum, char **pcontent, char **ptitle);
extern int CheckUser(const char *pDbName, const char *pusername, const char *ppassword);
extern int AddUser(const char *pDbName, const char *pusername, const char *ppassword);
extern int GetFormValue(const char *pcontent, const char *pkey, char *pdst, int maxlen);
extern int GetCookieUser(const char *prawbuf, char *puser, int maxlen);

/* player.c：球员 / 球队查询与页面生成（查询函数用到 sqlite3 类型，仅在 player.c 内部使用） */
extern int FindExactPlayer(const char *pDbName, const char *pname);
extern void FputsHtml(FILE *fp, const char *psrc);
extern int CreateHomepageHtml(const char *pfilename, const char *puser);
extern int CreateTeamHtml(const char *pid, const char *pfilename, const char *puser);
extern int CreatePlayerHtml(const char *pid, const char *pfilename, const char *puser);
extern int CreateSearchHtml(const char *pkeyword, const char *pfilename, const char *puser);

#endif
