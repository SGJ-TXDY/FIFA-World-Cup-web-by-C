#include "head.h"
#include "sqlite3.h"

/*
 * webserver.c —— 基于 TCP socket 的简易 HTTP 词典服务器
 *
 * 功能：监听 8080 端口，接收浏览器 HTTP 请求；
 *       GET 请求返回静态页面（index.html 等）；
 *       POST /searchword 请求解析表单中的单词，查询 sqlite3 词典数据库，
 *       动态生成 mean.html 并返回查询结果。
 *       若 dict.db 不存在，先从 dict.txt 文本词典导入生成数据库。
 */

/* HTTP 请求结构体：保存解析出的请求方法、URL 和请求体 */
typedef struct HttpRequest
{
    char *pmethod;              /* 请求方法，如 "GET" / "POST" */
    char *purl;                 /* 请求路径，如 "/" / "/searchword" */
    char *pcontent;             /* 请求体（POST 表单数据，如 "word=water"） */
}HttpRequest_t;

/* sqlite3 查询回调用的数据传递结构体 */
typedef struct data 
{
    char *pmean;                /* 指向存放查询结果（单词含义）的缓冲区 */
    int flag;                   /* 是否查到记录的标志：0 未查到，1 已查到 */
}data_t;

/*
 * 创建监听套接字
 * 功能：socket() 创建 TCP 套接字 -> bind() 绑定本机任意地址和指定端口
 *       -> listen() 转为被动监听（等待队列长度为 10）
 * 参数：Port - 要监听的端口号
 * 返回：成功返回监听套接字描述符，失败返回 -1
 */
int CreateListenSocket(int Port)
{
    int ret = 0;
    int sockfd = 0;
    struct sockaddr_in seraddr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == sockfd)
    {
        perror("fail to socket");
        return -1;
    }
    
    seraddr.sin_family = AF_INET;
    seraddr.sin_port = htons(Port);
    seraddr.sin_addr.s_addr = INADDR_ANY;
    ret = bind(sockfd, (struct sockaddr *)&seraddr, sizeof(seraddr));
    if (-1 == ret)
    {
        perror("fail to bind");
        return -1;
    }

    ret = listen(sockfd, 10);
    if (-1 == ret)
    {
        perror("fail to listen");
        return -1;
    }

    return sockfd;
}

/*
 * 接收 HTTP 请求
 * 功能：从已建立连接的套接字 confd 中读取浏览器发来的 HTTP 请求报文，
 *       存入 precvbuf 缓冲区（最多 maxlen 字节）
 * 返回：成功返回实际接收的字节数，失败返回 -1
 */
int RecvHttpRequest(int confd, char *precvbuf, int maxlen)
{
    ssize_t nret = 0;

    nret = recv(confd, precvbuf, maxlen, 0);
    if (-1 == nret)
    {
        perror("fail to recv");
        return -1;
    }

    return nret;
}

/*
 * sqlite3_exec 的查询结果回调函数
 * 功能：每查到一行记录就会被调用一次；这里把第 0 列（含义）
 *       拷贝到 pmean 指向的缓冲区，并置 flag=1 表示"单词存在"
 * 参数：arg - 实际传入的是 data_t 结构体指针
 *       colnum - 查询结果的列数；pcontent - 各列的值；ptitle - 各列的列名
 */
int callbackfun(void *arg, int colnum, char **pcontent, char **ptitle)
{
    data_t *pdata = arg;

    strcpy(pdata->pmean, pcontent[0]);
    pdata->flag = 1;

    return 0;
}

/*
 * 在词典数据库中查询单词含义
 * 功能：打开 sqlite3 数据库，执行 "select 含义 from 单词列表 where 单词=..."
 *       查询语句；若查到，含义会通过 callbackfun 写入 pmean 缓冲区
 * 参数：pDbName - 数据库文件名；pword - 要查询的单词；
 *       pmean - 存放查询结果（含义）的缓冲区
 * 返回：查到返回 1，未查到返回 0，出错返回 -1
 */
int FindWordMean(const char *pDbName, char *pword, char *pmean)
{
    int ret = 0;
    sqlite3 *pdb = NULL;
    char cmd[1024] = {0};
    char *perrmsg = NULL;
    int IsExist = 0;
    data_t TmpData;

    //打开数据库
    ret = sqlite3_open(pDbName, &pdb);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "sqlite3_open failed:%s\n", sqlite3_errmsg(pdb));
        return -1;
    }

    TmpData.pmean = pmean;
    TmpData.flag = 0;
    sprintf(cmd, "select 含义 from 单词列表 where 单词=\"%s\";", pword);
    ret = sqlite3_exec(pdb, cmd, callbackfun, &TmpData, &perrmsg);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "fail to sqlite3_exec:%s\n", perrmsg);
        sqlite3_free(perrmsg);
        return -1;
    }
    IsExist = TmpData.flag;

    sqlite3_close(pdb);

    return IsExist;
}

/*
 * 生成查询结果页面 mean.html
 * 功能：从 POST 请求体（形如 "word=water"）中解析出要查询的单词，
 *       调用 FindWordMean 查询词典数据库，把查询结果（含义或"单词不存在"）
 *       写入一个动态生成的 HTML 文件
 * 参数：pTmpRequest - 已解析的 HTTP 请求；pfilename - 要生成的 HTML 文件名
 * 返回：成功返回 0，失败返回 -1
 */
int CreateMeanHtml(HttpRequest_t *pTmpRequest, const char *pfilename)
{
    char *pword = NULL;
    char tmpbuff[4096] = {0};
    FILE *fp = NULL;
    int IsExist = 0;

    //word=water
    strtok(pTmpRequest->pcontent, "=");
    pword = strtok(NULL, "\r");
    if (pword == NULL)
    {
        return -1;
    }
    else 
    {
        fp = fopen(pfilename, "w");
        if (NULL == fp)
        {
            perror("fail to fopen");
            return -1;
        }
        fprintf(fp, "<!DOCTYPE html>\
                        <html>\
                        <head>\
                        <meta charset=\"utf-8\">\
                    <title>中文测试。。。。</title>\
                    </head>\
                    <body>");


        memset(tmpbuff, 0, sizeof(tmpbuff));
        IsExist = FindWordMean("dict.db", pword, tmpbuff);
        if (IsExist)
        {
            fprintf(fp, "含义:%s", tmpbuff);
        }
        else 
        {
            fprintf(fp, "单词不存在");
        }

        fprintf(fp, "</body></html>");

        fclose(fp);
    }

    return 0;
}

/*
 * 发送 HTTP 响应头
 * 功能：先用 access() 检测要发送的文件是否存在，
 *       存在则组包并发送 "200 OK" 响应头，
 *       不存在则发送 "404 NotFound" 响应头
 * 参数：confd - 与客户端的连接套接字；pfilename - 待发送的文件名
 * 返回：成功返回 0，发送失败返回 -1
 */
int SendHttpResponeHeader(int confd, char *pfilename)
{
    char tmpbuff[4096] = {0};
    ssize_t nret = 0;

    if (0 == access(pfilename, F_OK))
    {
        memset(tmpbuff, 0, sizeof(tmpbuff));
        sprintf(tmpbuff, "HTTP/1.1 200 OK\r\n");
        sprintf(tmpbuff, "%sServer: TinyServer\r\n", tmpbuff);
        sprintf(tmpbuff, "%sContent-Type: text/html; charset=utf-8;\r\n", tmpbuff);
        sprintf(tmpbuff, "%sConnection: keep-alive\r\n", tmpbuff);
        sprintf(tmpbuff, "%s\r\n", tmpbuff);
    }
    else
    {
        memset(tmpbuff, 0, sizeof(tmpbuff));
        sprintf(tmpbuff, "HTTP/1.1 404 NotFound\r\n");
        sprintf(tmpbuff, "%sServer: TinyServer\r\n", tmpbuff);
        sprintf(tmpbuff, "%sContent-Type: text/html; charset=utf-8;\r\n", tmpbuff);
        sprintf(tmpbuff, "%sConnection: keep-close\r\n", tmpbuff);
        sprintf(tmpbuff, "%s\r\n", tmpbuff);
    }

    nret = send(confd, tmpbuff, strlen(tmpbuff), 0);
    if (-1 == nret)
    {
        perror("fail to send");
        return -1;
    }

    return 0;
}

/*
 * 发送 HTTP 响应体（文件内容）
 * 功能：以只读方式打开文件，循环 read() 读取文件内容并 send() 发送给客户端，
 *       直到读到文件末尾（read 返回 0）为止
 * 参数：confd - 与客户端的连接套接字；pfilename - 待发送的文件名
 * 返回：成功返回 0，失败返回 -1
 */
int SendHttpResponeContent(int confd, char *pfilename)
{
    int fd = 0;
    char tmpbuff[4096] = {0};
    ssize_t nret = 0;

    fd = open(pfilename, O_RDONLY);
    if (-1 == fd)
    {
        perror("fail to open");
        return -1;
    }

    while (1)
    {
        memset(tmpbuff, 0, sizeof(tmpbuff));
        nret = read(fd, tmpbuff, sizeof(tmpbuff));
        if (-1 == nret)
        {
            perror("fail to read");
            return -1;
        }
        else if (0 == nret)
        {
            break;
        }

        nret = send(confd, tmpbuff, nret, 0);
        if (-1 == nret)
        {
            perror("fail to send");
            return -1;
        }
    }

    close(fd);

    return 0;
}

/*
 * 发送完整 HTTP 响应（路由分发）
 * 功能：根据请求方法和 URL 决定要返回给客户端的文件：
 *       - GET "/"          -> 返回首页 index.html
 *       - GET "/favicon.ico"-> 直接忽略（浏览器图标请求）
 *       - GET 其他路径      -> 返回对应的静态文件
 *       - POST "/searchword"-> 调用 CreateMeanHtml 动态生成 mean.html 后返回
 *       然后依次调用 SendHttpResponeHeader / SendHttpResponeContent
 *       发送响应头和响应体
 * 返回：成功返回 0
 */
int SendHttpRespone(int confd, HttpRequest_t *pTmpRequest)
{
    char tmpbuff[4096] = {0};
    char filename[256] = {0};
    int fd = 0;
    ssize_t nret = 0;
    int ret = 0;

    if (0 == strcmp(pTmpRequest->pmethod, "GET"))
    {
        if (0 == strcmp(pTmpRequest->purl, "/"))
        {
            sprintf(filename, "./index.html");
        }
        else if (0 == strcmp(pTmpRequest->purl, "/favicon.ico"))
        {
            return 0;
        }
        else 
        {
            sprintf(filename, ".%s", pTmpRequest->purl);
        }
    }
    else if (0 == strcmp(pTmpRequest->pmethod, "POST"))
    {
        if (0 == strcmp(pTmpRequest->purl, "/searchword"))
        {
            ret = CreateMeanHtml(pTmpRequest, "./mean.html");
            if (0 == ret)
            {
                sprintf(filename, "./mean.html");
            }
            else 
            {
                sprintf(filename, "./index.html");
            }
        } 
    }

    SendHttpResponeHeader(confd, filename);
    SendHttpResponeContent(confd, filename);

    return 0;
}

/*
 * 解析 HTTP 请求报文
 * 功能：从接收缓冲区中拆解出请求的三个要素：
 *       - pmethod：请求行第一个空格前的单词（GET/POST）
 *       - purl：请求行第二个空格前的单词（请求路径）
 *       - pcontent："\r\n\r\n" 之后的请求体（POST 表单数据）
 * 注意：strtok 会修改原缓冲区（在分隔符处写入 '\0'）
 * 返回：成功返回 0
 */
int PraseHttpRequest(char *precvbuf, HttpRequest_t *pTmpRequest)
{
    pTmpRequest->pcontent = strstr(precvbuf, "\r\n\r\n");
    pTmpRequest->pmethod = strtok(precvbuf, " ");
    pTmpRequest->purl = strtok(NULL, " ");

    return 0;
}

/*
 * 从文本文件导入生成词典数据库
 * 功能：1) 打开（不存在则创建）sqlite3 数据库，并创建"单词列表"表
 *          （字段：ID 主键、单词、含义）；
 *       2) 逐行读取 dict.txt（每行格式：单词 + 空格 + 含义），
 *          拆分出单词和含义后逐条 insert 进数据库；
 *       3) 所有插入放在一个事务（BEGIN/END TRANSACTION）中执行以提高速度，
 *          并实时打印加载进度百分比
 * 参数：pDbName - 数据库文件名；pFileName - 文本词典文件名
 * 返回：成功返回 0，失败返回 -1
 */
int CreateWordDB(const char *pDbName, const char *pFileName)
{
    sqlite3 *pdb = NULL;
    FILE *fp = NULL;
    char tmpbuff[4096] = {0};
    char *pret = NULL;
    char *ptmp = NULL;
    char cmd[1024] = {0};
    char *perrmsg = NULL;
    int ret = 0;
    long tlen = 0;
    long clen = 0;

    //打开数据库
    ret = sqlite3_open(pDbName, &pdb);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "sqlite3_open failed:%s\n", sqlite3_errmsg(pdb));
        return -1;
    }

    //创建表
    sprintf(cmd, "create table if not exists 单词列表 (ID integer primary key asc, 单词 text, 含义 text);");
    ret = sqlite3_exec(pdb, cmd, NULL, NULL, &perrmsg);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "fail to sqlite3_exec:%s\n", perrmsg);
        sqlite3_free(perrmsg);
        return -1;
    }

    //打开文件
    fp = fopen(pFileName, "r");
    if (NULL == fp)
    {
        perror("fail to fopen");
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    tlen = ftell(fp);
    rewind(fp);

    sprintf(cmd, "BEGIN TRANSACTION;");
    ret = sqlite3_exec(pdb, cmd, NULL, NULL, &perrmsg);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "fail to sqlite3_exec:%s\n", perrmsg);
        sqlite3_free(perrmsg);
        return -1;
    }

    while (1)
    {
        //读取一行
        pret = fgets(tmpbuff, sizeof(tmpbuff), fp);
        if (NULL == pret)
        {
            break;
        }

        //拆分获得单词
        ptmp = tmpbuff;
        while (*ptmp != '\0' && *ptmp != ' ')
        {
            ptmp++;
        }
        *ptmp = '\0';

        //拆分获得含义
        ptmp++;
        while (*ptmp != '\0' && ' ' == *ptmp)
        {
            ptmp++;
        }

        //将改行单词和含义插入数据库中
        sprintf(cmd, "insert into 单词列表 values (NULL, \"%s\", \"%s\");", tmpbuff, ptmp);
        ret = sqlite3_exec(pdb, cmd, NULL, NULL, &perrmsg);
        if (ret != SQLITE_OK)
        {
            fprintf(stderr, "fail to sqlite3_exec:%s\n", perrmsg);
            sqlite3_free(perrmsg);
            return -1;
        }

        clen = ftell(fp);
        printf("已加载:%.2lf%%\r", ((double)clen / (double)tlen) * 100.0);
        fflush(stdout);
    }
    
    sprintf(cmd, "END TRANSACTION;");
    ret = sqlite3_exec(pdb, cmd, NULL, NULL, &perrmsg);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "fail to sqlite3_exec:%s\n", perrmsg);
        sqlite3_free(perrmsg);
        return -1;
    }

    fclose(fp);
    sqlite3_close(pdb);

    printf("加载完成         \n");

    return 0;
}

/*
 * 主函数：服务器主流程
 * 1) 若 dict.db 不存在，先调用 CreateWordDB 从 dict.txt 导入建库；
 * 2) 创建监听套接字（8080 端口）；
 * 3) 进入服务循环：accept 接受一个客户端连接 -> 接收 HTTP 请求并打印
 *    -> 解析请求 -> 发送响应 -> 关闭连接，然后继续等待下一个连接
 *    （单进程串行处理，一次只服务一个客户端）
 */
int main(void)
{
    int sockfd = 0;
    int confd = 0;
    char recvbuf[4096] = {0};
    HttpRequest_t TmpRequest;

    if (access("dict.db", F_OK))
    {   
        CreateWordDB("dict.db", "dict.txt");
    }

    sockfd = CreateListenSocket(8080);

    while (1)
    {
        confd = accept(sockfd, NULL, NULL);
        if (-1 == confd)
        {
            perror("fail to accept");
            return -1;
        }

        RecvHttpRequest(confd, recvbuf, sizeof(recvbuf));

        printf("============================== RECV ==============================\n");
        printf("%s\n", recvbuf);
        printf("==================================================================\n");

        PraseHttpRequest(recvbuf, &TmpRequest);

        SendHttpRespone(confd, &TmpRequest);

        close(confd);
    }
    
    close(sockfd);

    return 0;
}