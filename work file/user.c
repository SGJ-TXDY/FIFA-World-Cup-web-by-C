/*
 * user.c —— 注册、登录相关函数
 *
 *   - CreateUserTable：在 mima.db 中创建 users 表（id/账号/密码）；
 *   - UserCallback：sqlite3_exec 的用户查询回调（查到记录即置标志位）；
 *   - CheckUser：校验登录账号密码；
 *   - AddUser：注册新用户（账号已存在则注册失败）；
 *   - GetFormValue：从 POST 表单数据中提取指定字段的值（URL 解码）；
 *   - GetCookieUser：从原始请求报文的 Cookie 头中提取登录用户。
 */
#include "head.h"
#include "sqlite3.h"

/*
 * 创建用户表
 * 功能：在 mima.db 中创建 users 表（id/账号/密码），表已存在则不重复创建
 * 参数：pDbName - 数据库文件名（"mima.db"）
 * 返回：成功返回 0，失败返回 -1
 */
int CreateUserTable(const char *pDbName)
{
    int ret = 0;
    sqlite3 *pdb = NULL;
    char cmd[512] = {0};
    char *perrmsg = NULL;

    ret = sqlite3_open(pDbName, &pdb);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "sqlite3_open failed:%s\n", sqlite3_errmsg(pdb));
        return -1;
    }

    sprintf(cmd, "create table if not exists users("
                 "id integer primary key autoincrement,"
                 "username text unique not null,"
                 "password text not null);");
    ret = sqlite3_exec(pdb, cmd, NULL, NULL, &perrmsg);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "fail to sqlite3_exec:%s\n", perrmsg);
        sqlite3_free(perrmsg);
        sqlite3_close(pdb);
        return -1;
    }

    sqlite3_close(pdb);

    return 0;
}

/*
 * sqlite3_exec 的用户查询回调函数
 * 功能：只要查到一条记录，就把标志位置 1（用于"用户是否存在/密码是否正确"）
 */
int UserCallback(void *arg, int colnum, char **pcontent, char **ptitle)
{
    int *pflag = arg;

    *pflag = 1;

    return 0;
}

/*
 * 校验登录账号密码
 * 功能：在 users 表中查询 "username=账号 and password=密码" 的记录
 * 参数：pDbName - 数据库文件名；pusername - 账号；ppassword - 密码
 * 返回：匹配成功返回 1，账号或密码错误返回 0，出错返回 -1
 */
int CheckUser(const char *pDbName, const char *pusername, const char *ppassword)
{
    int ret = 0;
    int flag = 0;
    sqlite3 *pdb = NULL;
    char cmd[512] = {0};
    char *perrmsg = NULL;

    ret = sqlite3_open(pDbName, &pdb);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "sqlite3_open failed:%s\n", sqlite3_errmsg(pdb));
        return -1;
    }

    sprintf(cmd, "select id from users where username=\"%s\" and password=\"%s\" "
                 "limit 1;", pusername, ppassword);
    ret = sqlite3_exec(pdb, cmd, UserCallback, &flag, &perrmsg);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "fail to sqlite3_exec:%s\n", perrmsg);
        sqlite3_free(perrmsg);
        sqlite3_close(pdb);
        return -1;
    }

    sqlite3_close(pdb);

    return flag;
}

/*
 * 注册新用户
 * 功能：先查询账号是否已存在，不存在则把账号密码插入 users 表
 * 参数：pDbName - 数据库文件名；pusername - 账号；ppassword - 密码
 * 返回：注册成功返回 1，账号已存在返回 0，出错返回 -1
 */
int AddUser(const char *pDbName, const char *pusername, const char *ppassword)
{
    int ret = 0;
    int flag = 0;
    sqlite3 *pdb = NULL;
    char cmd[512] = {0};
    char *perrmsg = NULL;

    ret = sqlite3_open(pDbName, &pdb);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "sqlite3_open failed:%s\n", sqlite3_errmsg(pdb));
        return -1;
    }

    //检查账号是否已被注册
    sprintf(cmd, "select id from users where username=\"%s\" limit 1;", pusername);
    ret = sqlite3_exec(pdb, cmd, UserCallback, &flag, &perrmsg);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "fail to sqlite3_exec:%s\n", perrmsg);
        sqlite3_free(perrmsg);
        sqlite3_close(pdb);
        return -1;
    }
    if (1 == flag)
    {
        sqlite3_close(pdb);
        return 0;
    }

    //插入新账号
    sprintf(cmd, "insert into users(username,password) values(\"%s\",\"%s\");",
            pusername, ppassword);
    ret = sqlite3_exec(pdb, cmd, NULL, NULL, &perrmsg);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "fail to sqlite3_exec:%s\n", perrmsg);
        sqlite3_free(perrmsg);
        sqlite3_close(pdb);
        return -1;
    }

    sqlite3_close(pdb);

    return 1;
}

/*
 * 从 POST 表单数据或 URL 查询串中提取指定字段的值
 * 功能：在请求体（形如 "username=xx&password=yy"）或 URL（形如
 *       "/player?id=12"）中查找 key 对应的值，并做 URL 解码；
 *       要求 key 前面是报文开头、'?'、'&' 或换行，避免
 *       "name=" 误匹配到 "username=" 内部
 * 参数：pcontent - POST 请求体或 URL；pkey - 字段名；
 *       pdst - 存放结果的缓冲区；maxlen - 缓冲区大小
 * 返回：成功返回 0，未找到返回 -1
 */
int GetFormValue(const char *pcontent, const char *pkey, char *pdst, int maxlen)
{
    const char *pcur = NULL;
    const char *pstart = NULL;
    char raw[256] = {0};
    int keylen = strlen(pkey);
    int i = 0;

    if (NULL == pcontent)
    {
        return -1;
    }

    //逐个匹配 key，直到找到边界正确且紧跟 '=' 的那个
    pcur = pcontent;
    while (NULL != (pcur = strstr(pcur, pkey)))
    {
        if ((pcur == pcontent || '&' == *(pcur - 1) || '?' == *(pcur - 1)
             || '\r' == *(pcur - 1) || '\n' == *(pcur - 1))
            && '=' == *(pcur + keylen))
        {
            pstart = pcur + keylen + 1;
            break;
        }
        pcur += keylen;
    }

    if (NULL == pstart)
    {
        return -1;
    }

    //取出 = 之后的值（到 & 或报文结束为止）
    while (*pstart != '\0' && *pstart != '&' && *pstart != ' '
           && *pstart != '\r' && *pstart != '\n' && i < (int)sizeof(raw) - 1)
    {
        raw[i++] = *pstart++;
    }
    raw[i] = '\0';

    UrlDecode(pdst, raw, maxlen);

    return 0;
}

/*
 * 从原始请求报文中提取登录用户
 * 功能：查找请求头 "Cookie:" 中的 "user=xxx"，xxx 即登录成功的账号
 * 参数：prawbuf - 原始 HTTP 请求报文（未被 strtok 破坏的备份）；
 *       puser - 存放用户名的缓冲区；maxlen - 缓冲区大小
 * 返回：已登录返回 0，未登录返回 -1
 */
int GetCookieUser(const char *prawbuf, char *puser, int maxlen)
{
    const char *pcookie = NULL;
    const char *pstart = NULL;
    int i = 0;

    pcookie = strstr(prawbuf, "Cookie:");
    if (NULL == pcookie)
    {
        return -1;
    }

    pstart = strstr(pcookie, "user=");
    if (NULL == pstart)
    {
        return -1;
    }

    pstart += strlen("user=");
    while (*pstart != '\0' && *pstart != ';' && *pstart != ' '
           && *pstart != '\r' && *pstart != '\n' && i < maxlen - 1)
    {
        puser[i++] = *pstart++;
    }
    puser[i] = '\0';

    return (i > 0) ? 0 : -1;
}
