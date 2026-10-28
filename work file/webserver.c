/*
 * webserver.c —— 基于 TCP socket 的简易 HTTP 服务器（世界杯球员信息查询版）
 *
 * 文件组织：
 *   - head.h：宏定义、结构体声明以及各模块函数的 extern 声明；
 *   - user.c：注册、登录相关函数（users 表维护、表单/URL 参数提取、
 *             Cookie 登录用户解析）；
 *   - db.c：世界杯数据库 worldcup.db 建库，从 teams.txt / players.txt 导入数据；
 *   - player.c：球员 / 球队查询与页面生成（首页、球员页、球队页、搜索页）；
 *   - style.css：上述动态页面共用的样式（作为静态文件发送）；
 *   - webserver.c：服务器框架（监听套接字、接收/解析 HTTP 请求、
 *             路由分发、发送响应、main 主循环）。
 *
 * 仿照 webserver_wordmean 实现：
 *   1) 启动时若 worldcup.db 不存在则从 teams.txt、players.txt 导入建库；
 *      若 mima.db 不存在则创建账号密码数据库，
 *      并在其中创建 users 表（id/username/password）保存注册的账号密码；
 *   2) 监听 8080 端口，接收并解析浏览器 HTTP 请求报文；
 *   3) 根据请求方法（GET/POST）和请求路径进行路由分发：
 *      - POST /register：注册，账号密码写入 mima.db（已存在则注册失败）；
 *      - POST /login：登录，校验成功则 Set-Cookie 记录登录用户并跳转首页，
 *        失败则跳回登录页（带错误提示参数）；
 *      - GET /logout：退出登录，清除 Cookie 后跳回登录页；
 *      - GET / 或 /homepage.html：首页（射手榜、12 个小组积分表）；
 *      - GET /team?id=球队代码：球队资料和大名单；
 *      - GET /player?id=球员编号：球员资料和同队队员；
 *      - GET /search?name=关键词（或 POST /search）：
 *        姓名完全相同且不重名 -> 直接跳转球员详情页；
 *        否则按姓名/俱乐部/球队/位置匹配 -> 搜索结果页（无结果时为提示页）；
 *      - 其余 GET 请求返回对应静态文件（禁止 ".." 和 .db 等文件）。
 */
#include "head.h"
#include "sqlite3.h"

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

    //设置地址可重用，避免服务器重启时因 TIME_WAIT 状态导致 bind 失败
    int on = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    
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
 * 创建数据库
 * 功能：sqlite3_open 打开数据库文件，文件不存在时会自动创建空库
 * 参数：pDbName - 数据库文件名（"mima.db"）
 * 返回：成功返回 0，失败返回 -1
 */
int CreateDB(const char *pDbName)
{
    int ret = 0;
    sqlite3 *pdb = NULL;

    //打开数据库（不存在则自动创建）
    ret = sqlite3_open(pDbName, &pdb);
    if (ret != SQLITE_OK)
    {
        fprintf(stderr, "sqlite3_open failed:%s\n", sqlite3_errmsg(pdb));
        return -1;
    }

    sqlite3_close(pdb);

    return 0;
}

/*
 * 接收 HTTP 请求
 * 功能：循环 recv() 接收浏览器发来的 HTTP 请求报文，直到收齐为止：
 *       收到完整的请求头（出现 "\r\n\r\n"）后，若头部带有 Content-Length
 *       （POST 表单），继续接收到请求体长度达标为止；缓冲区始终保留一个
 *       字节写 '\0'，保证后续 strstr/strtok 等字符串操作不会越界
 * 参数：confd - 与客户端的连接套接字；precvbuf - 接收缓冲区；
 *       maxlen - 缓冲区大小（报文超过 maxlen-1 字节时只能截断接收）
 * 返回：成功返回实际接收的字节数，失败返回 -1
 */
int RecvHttpRequest(int confd, char *precvbuf, int maxlen)
{
    ssize_t nret = 0;
    ssize_t ntotal = 0;
    long bodylen = 0;        /* Content-Length 声明的请求体长度 */
    char *pbody = NULL;      /* 请求头结束位置（"\r\n\r\n"） */
    char *pvalue = NULL;

    while (ntotal < maxlen - 1)
    {
        nret = recv(confd, precvbuf + ntotal, maxlen - 1 - ntotal, 0);
        if (-1 == nret)
        {
            perror("fail to recv");
            return -1;
        }
        else if (0 == nret)
        {
            break;  /* 对端关闭连接 */
        }
        ntotal += nret;
        precvbuf[ntotal] = '\0';

        //请求头收齐后，从中取出 Content-Length（必须在头部范围内匹配，
        //避免把请求体里的同名串误认为头部）
        if (NULL == pbody)
        {
            pbody = strstr(precvbuf, "\r\n\r\n");
            if (NULL != pbody)
            {
                pvalue = strstr(precvbuf, "Content-Length:");
                if (NULL != pvalue && pvalue < pbody)
                {
                    bodylen = atol(pvalue + strlen("Content-Length:"));
                }
            }
        }

        //请求头和请求体都收齐即返回（无 Content-Length 时收齐头部即可）
        if (NULL != pbody && ntotal >= (pbody + 4 - precvbuf) + bodylen)
        {
            break;
        }
    }

    return ntotal;
}

/* 十六进制字符转数值（用于 URL 解码） */
int HexVal(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/*
 * URL 解码
 * 功能：浏览器会把中文等字符编码为 %XX 形式（空格编码为 +），
 *       本函数将其还原为原始 UTF-8 字符串
 * 参数：pdst - 解码结果缓冲区；psrc - 待解码字符串；maxlen - 缓冲区大小
 */
void UrlDecode(char *pdst, const char *psrc, int maxlen)
{
    int i = 0;
    int hi = 0, lo = 0;

    while (*psrc != '\0' && i < maxlen - 1)
    {
        if ('%' == *psrc && *(psrc + 1) != '\0' && *(psrc + 2) != '\0')
        {
            hi = HexVal(*(psrc + 1));
            lo = HexVal(*(psrc + 2));
            if (hi >= 0 && lo >= 0)
            {
                pdst[i++] = (char)(hi * 16 + lo);
                psrc += 3;
                continue;
            }
        }
        if ('+' == *psrc)
        {
            pdst[i++] = ' ';
            psrc++;
            continue;
        }
        pdst[i++] = *psrc++;
    }
    pdst[i] = '\0';
}

/*
 * 完整发送数据
 * 功能：send() 一次调用可能只发送部分数据（尤其是大文件），
 *       本函数循环发送直到 len 字节全部发出
 * 参数：confd - 与客户端的连接套接字；pbuf - 待发送数据；len - 数据长度
 * 返回：成功返回 0，发送失败返回 -1
 */
int SendAll(int confd, const char *pbuf, ssize_t len)
{
    ssize_t nret = 0;
    ssize_t nsend = 0;

    while (nsend < len)
    {
        nret = send(confd, pbuf + nsend, len - nsend, 0);
        if (-1 == nret)
        {
            perror("fail to send");
            return -1;
        }
        nsend += nret;
    }

    return 0;
}

/*
 * 发送 HTTP 响应头
 * 功能：用 stat() 检测要发送的文件，只有存在且为普通文件时发送
 *       "200 OK"（目录等一律按 "404 NotFound" 处理）；
 *       Content-Type 根据文件扩展名确定（html/jpg/png/gif/css/js 等），
 *       保证浏览器能正确解析商品图片等静态资源；
 *       同时携带 Content-Length（响应体字节数，404 时为 0）和
 *       "Connection: close"——服务器每个连接只处理一个请求就关闭，
 *       如实告知浏览器，不再谎称 keep-alive
 * 参数：confd - 与客户端的连接套接字；pfilename - 待发送的文件名
 * 返回：200 返回 1，404 返回 0，发送失败返回 -1
 */
int SendHttpResponeHeader(int confd, char *pfilename)
{
    char tmpbuff[4096] = {0};
    const char *ptype = "text/html; charset=utf-8;";
    char *pext = NULL;
    struct stat st;
    int exists = 0;

    //根据扩展名确定 Content-Type
    pext = strrchr(pfilename, '.');
    if (NULL != pext)
    {
        if (0 == strcmp(pext, ".jpg") || 0 == strcmp(pext, ".jpeg")
            || 0 == strcmp(pext, ".JPG") || 0 == strcmp(pext, ".JPEG"))
        {
            ptype = "image/jpeg";
        }
        else if (0 == strcmp(pext, ".png"))
        {
            ptype = "image/png";
        }
        else if (0 == strcmp(pext, ".gif"))
        {
            ptype = "image/gif";
        }
        else if (0 == strcmp(pext, ".ico"))
        {
            ptype = "image/x-icon";
        }
        else if (0 == strcmp(pext, ".css"))
        {
            ptype = "text/css; charset=utf-8";
        }
        else if (0 == strcmp(pext, ".js"))
        {
            ptype = "application/javascript";
        }
    }

    //只有普通文件才认为存在（access 对目录也会返回存在）
    if (0 == stat(pfilename, &st) && S_ISREG(st.st_mode))
    {
        exists = 1;
    }

    if (exists)
    {
        snprintf(tmpbuff, sizeof(tmpbuff),
                 "HTTP/1.1 200 OK\r\n"
                 "Server: TinyServer\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %ld\r\n"
                 "Connection: close\r\n"
                 "\r\n", ptype, (long)st.st_size);
    }
    else
    {
        snprintf(tmpbuff, sizeof(tmpbuff),
                 "HTTP/1.1 404 NotFound\r\n"
                 "Server: TinyServer\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: 0\r\n"
                 "Connection: close\r\n"
                 "\r\n", ptype);
    }

    if (0 != SendAll(confd, tmpbuff, strlen(tmpbuff)))
    {
        return -1;
    }

    return exists;
}

/*
 * 发送 HTTP 响应体（文件内容）
 * 功能：以只读方式打开文件，循环 read() 读取文件内容，并用 SendAll()
 *       完整发送给客户端（send 一次可能只发出部分数据），
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
        nret = read(fd, tmpbuff, sizeof(tmpbuff));
        if (-1 == nret)
        {
            perror("fail to read");
            close(fd);
            return -1;
        }
        else if (0 == nret)
        {
            break;
        }

        if (0 != SendAll(confd, tmpbuff, nret))
        {
            close(fd);
            return -1;
        }
    }

    close(fd);

    return 0;
}

/*
 * 发送 302 重定向响应
 * 功能：让浏览器跳转到 plocation 指定的页面；pcookie 非空时同时通过
 *       Set-Cookie 写入/清除登录 Cookie（如登录成功写 user=xxx，
 *       退出登录时让 Cookie 立即过期）
 * 参数：confd - 与客户端的连接套接字；plocation - 跳转目标；
 *       pcookie - 要设置的 Cookie 内容（如 "user=zhangsan; Path=/"），可为 NULL
 * 返回：成功返回 0，发送失败返回 -1
 */
int SendRedirect(int confd, const char *plocation, const char *pcookie)
{
    char tmpbuff[1024] = {0};

    if (NULL != pcookie)
    {
        snprintf(tmpbuff, sizeof(tmpbuff),
                 "HTTP/1.1 302 Found\r\n"
                 "Server: TinyServer\r\n"
                 "Location: %s\r\n"
                 "Set-Cookie: %s\r\n"
                 "Content-Length: 0\r\n"
                 "Connection: close\r\n"
                 "\r\n", plocation, pcookie);
    }
    else
    {
        snprintf(tmpbuff, sizeof(tmpbuff),
                 "HTTP/1.1 302 Found\r\n"
                 "Server: TinyServer\r\n"
                 "Location: %s\r\n"
                 "Content-Length: 0\r\n"
                 "Connection: close\r\n"
                 "\r\n", plocation);
    }

    if (0 != SendAll(confd, tmpbuff, strlen(tmpbuff)))
    {
        return -1;
    }

    return 0;
}

/*
 * 静态文件访问检查
 * 功能：禁止 URL 中出现 ".."（防止读取网站目录以外的文件），
 *       禁止下载数据库（含账号密码的 mima.db）、源码和编译产物
 * 参数：pfilename - 由 URL 拼出的文件名（如 "./style.css"）
 * 返回：禁止访问返回 1，允许返回 0
 */
int IsForbiddenFile(const char *pfilename)
{
    const char *pext = NULL;

    if (NULL != strstr(pfilename, ".."))
    {
        return 1;
    }

    pext = strrchr(pfilename, '.');
    if (NULL != pext && (0 == strcmp(pext, ".db") || 0 == strcmp(pext, ".c")
                         || 0 == strcmp(pext, ".h") || 0 == strcmp(pext, ".o")
                         || 0 == strcmp(pext, ".mjs") || 0 == strcmp(pext, ".out")))
    {
        return 1;
    }

    return 0;
}

/*
 * 处理球员搜索
 * 功能：关键词为空 -> 跳回首页；
 *       与某个球员姓名完全相同且不重名 -> 302 跳转该球员详情页；
 *       其余情况 -> 生成搜索结果页 search.html（无结果时为提示页）
 * 参数：confd - 与客户端的连接套接字；pkeyword - 已 URL 解码的关键词；
 *       puser - 当前登录用户；pfilename - 需要发送搜索结果页时写入文件名
 * 返回：已直接发送跳转响应返回 1，需要继续发送 pfilename 返回 0
 */
int HandleSearch(int confd, const char *pkeyword, const char *puser, char *pfilename)
{
    char location[64] = {0};
    int id = 0;

    if ('\0' == pkeyword[0])
    {
        SendRedirect(confd, "/", NULL);
        return 1;
    }

    id = FindExactPlayer("worldcup.db", pkeyword);
    if (id > 0)
    {
        sprintf(location, "/player?id=%d", id);
        SendRedirect(confd, location, NULL);
        return 1;
    }

    if (0 == CreateSearchHtml(pkeyword, "./search.html", puser))
    {
        sprintf(pfilename, "./search.html");
    }
    else
    {
        sprintf(pfilename, "./index.html");
    }

    return 0;
}

/*
 * 发送完整 HTTP 响应（路由分发：判断请求报文信息）
 * 功能：根据请求方法和 URL 决定要返回给客户端的内容：
 *       - GET "/" 或 "/homepage.html" -> 从 worldcup.db 动态生成首页
 *       - GET "/favicon.ico"     -> 直接忽略（浏览器图标请求）
 *       - GET "/player?id=12"    -> 生成球员详情页 player.html
 *       - GET "/team?id=fra"     -> 生成球队页 team.html
 *       - GET "/search?name=xx"  -> 搜索（见 HandleSearch）
 *       - GET "/logout"          -> 清除登录 Cookie，跳转登录界面
 *       - GET 其他路径            -> 返回对应的静态文件（禁止 ".." 和数据库等文件）
 *       - POST "/search"         -> 从请求体解析关键词，同 GET 搜索
 *       - POST "/register"       -> 注册：账号密码写入 mima.db，
 *                                   成功跳登录页，账号已存在跳回注册页
 *       - POST "/login"          -> 登录：校验 mima.db 中的账号密码，
 *                                   成功写 Cookie 并跳首页，失败跳回登录页
 *       - POST 其他路径           -> 打印请求信息，统一返回登录页
 *       页面生成失败（如 worldcup.db 不存在）时退回登录页 index.html；
 *       然后调用 SendHttpResponeHeader 发送响应头，返回 200 时
 *       再调用 SendHttpResponeContent 发送响应体（404 无响应体）
 * 参数：confd - 与客户端的连接套接字；pTmpRequest - 已解析的 HTTP 请求；
 *       prawbuf - 原始请求报文备份（用于解析 Cookie 登录状态）
 * 返回：成功返回 0
 */
int SendHttpRespone(int confd, HttpRequest_t *pTmpRequest, const char *prawbuf)
{
    char filename[256] = {0};
    char user[64] = {0};
    char username[64] = {0};
    char password[64] = {0};
    char cookie[160] = {0};
    char value[128] = {0};      /* URL / 表单中取出的参数值（球员 id、球队代码、搜索词） */
    char *pquery = NULL;
    int ret = 0;

    //空请求（浏览器预连接后直接关闭）解析不出方法和路径，直接忽略
    if (NULL == pTmpRequest->pmethod || NULL == pTmpRequest->purl)
    {
        return 0;
    }

    //从原始报文的 Cookie 头中解析当前登录用户（未登录则 user 为空串）
    GetCookieUser(prawbuf, user, sizeof(user));

    if (0 == strcmp(pTmpRequest->pmethod, "GET"))
    {
        if (0 == strcmp(pTmpRequest->purl, "/")
            || 0 == strcmp(pTmpRequest->purl, "/homepage.html"))
        {
            //首页：射手榜 + 小组积分
            ret = CreateHomepageHtml("./homepage.html", user);
            sprintf(filename, (0 == ret) ? "./homepage.html" : "./index.html");
        }
        else if (0 == strcmp(pTmpRequest->purl, "/favicon.ico"))
        {
            return 0;
        }
        else if (0 == strcmp(pTmpRequest->purl, "/logout"))
        {
            //退出登录：清除登录 Cookie 后跳回登录界面
            SendRedirect(confd, "/index.html", "user=; Path=/; Max-Age=0");
            return 0;
        }
        else if (0 == strncmp(pTmpRequest->purl, "/player?", 8))
        {
            //球员详情页
            GetFormValue(pTmpRequest->purl, "id", value, sizeof(value));
            ret = CreatePlayerHtml(value, "./player.html", user);
            sprintf(filename, (0 == ret) ? "./player.html" : "./index.html");
        }
        else if (0 == strncmp(pTmpRequest->purl, "/team?", 6))
        {
            //球队页
            GetFormValue(pTmpRequest->purl, "id", value, sizeof(value));
            ret = CreateTeamHtml(value, "./team.html", user);
            sprintf(filename, (0 == ret) ? "./team.html" : "./index.html");
        }
        else if (0 == strcmp(pTmpRequest->purl, "/search")
                 || 0 == strncmp(pTmpRequest->purl, "/search?", 8))
        {
            //搜索：唯一同名直接跳详情页，否则生成结果页
            GetFormValue(pTmpRequest->purl, "name", value, sizeof(value));
            if (1 == HandleSearch(confd, value, user, filename))
            {
                return 0;
            }
        }
        else
        {
            //静态文件路径：去掉 URL 中 "?" 后的查询串，
            //避免 "/register.html?err=1" 被当成不存在的文件而 404
            snprintf(filename, sizeof(filename), ".%s", pTmpRequest->purl);
            pquery = strchr(filename, '?');
            if (NULL != pquery)
            {
                *pquery = '\0';
            }

            //禁止访问的文件按不存在处理（stat 失败 -> 404）
            if (IsForbiddenFile(filename))
            {
                filename[0] = '\0';
            }
        }
    }
    else if (0 == strcmp(pTmpRequest->pmethod, "POST"))
    {
        if (0 == strcmp(pTmpRequest->purl, "/register"))
        {
            //注册：解析表单中的账号密码，写入 mima.db
            GetFormValue(pTmpRequest->pcontent, "username", username, sizeof(username));
            GetFormValue(pTmpRequest->pcontent, "password", password, sizeof(password));

            if ('\0' != username[0] && '\0' != password[0]
                && 1 == AddUser("mima.db", username, password))
            {
                //注册成功：跳转登录界面（reg=1 提示"注册成功，请登录"）
                SendRedirect(confd, "/index.html?reg=1", NULL);
            }
            else
            {
                //账号已存在或输入为空：跳回注册页（err=1 提示）
                SendRedirect(confd, "/register.html?err=1", NULL);
            }
            return 0;
        }
        else if (0 == strcmp(pTmpRequest->purl, "/login"))
        {
            //登录：解析表单中的账号密码，与 mima.db 中的记录比对
            GetFormValue(pTmpRequest->pcontent, "username", username, sizeof(username));
            GetFormValue(pTmpRequest->pcontent, "password", password, sizeof(password));

            if ('\0' != username[0] && 1 == CheckUser("mima.db", username, password))
            {
                //登录成功：写入登录 Cookie，跳转首页
                sprintf(cookie, "user=%s; Path=/", username);
                SendRedirect(confd, "/homepage.html", cookie);
            }
            else
            {
                //账号或密码错误：跳回登录界面（err=1 提示）
                SendRedirect(confd, "/index.html?err=1", NULL);
            }
            return 0;
        }
        else if (0 == strcmp(pTmpRequest->purl, "/search"))
        {
            GetFormValue(pTmpRequest->pcontent, "name", value, sizeof(value));
            if (1 == HandleSearch(confd, value, user, filename))
            {
                return 0;
            }
        }
        else
        {
            //打印 POST 请求信息，后续在此根据路径分别处理业务
            printf("POST url:%s\n", pTmpRequest->purl);
            printf("POST content:%s\n", pTmpRequest->pcontent);

            sprintf(filename, "./index.html");
        }
    }

    //发送响应头；只有 200（文件确实存在）才继续发送响应体，
    //404 响应没有响应体（Content-Length 已为 0）
    ret = SendHttpResponeHeader(confd, filename);
    if (1 == ret)
    {
        SendHttpResponeContent(confd, filename);
    }

    return 0;
}

/*
 * 解析 HTTP 请求报文
 * 功能：从接收缓冲区中拆解出请求的三个要素：
 *       - pmethod：请求行第一个空格前的单词（GET/POST）
 *       - purl：请求行第二个空格前的单词（请求路径，含查询串）
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
 * 主函数：服务器主流程
 * 1) 若 worldcup.db 不存在，调用 CreateWorldCupDB 从 txt 导入建库；
 *    若 mima.db 不存在，同样创建，并确保其中 users 表存在；
 * 2) 创建监听套接字（8080 端口）；
 * 3) 进入服务循环：accept 接受一个客户端连接 -> 接收 HTTP 请求并打印
 *    -> 备份原始报文（解析时 strtok 会破坏缓冲区，Cookie 等信息
 *    需要从备份中提取）-> 解析请求 -> 发送响应 -> 关闭连接，
 *    然后继续等待下一个连接
 */
int main(void)
{
    int sockfd = 0;
    int confd = 0;
    char recvbuf[4096] = {0};
    char rawbuf[4096] = {0};
    HttpRequest_t TmpRequest;

    //世界杯数据库不存在则从 teams.txt、players.txt 导入建库
    //（数据更新后删除 worldcup.db 再启动即可重新导入）
    if (access("worldcup.db", F_OK))
    {
        CreateWorldCupDB("worldcup.db", "teams.txt", "players.txt");
    }

    //账号密码数据库不存在则创建，并确保 users 表存在
    if (access("mima.db", F_OK))
    {
        CreateDB("mima.db");
    }
    CreateUserTable("mima.db");

    sockfd = CreateListenSocket(8080);
    if (-1 == sockfd)
    {
        return -1;
    }

    while (1)
    {
        confd = accept(sockfd, NULL, NULL);
        if (-1 == confd)
        {
            perror("fail to accept");
            return -1;
        }

        memset(recvbuf, 0, sizeof(recvbuf));
        RecvHttpRequest(confd, recvbuf, sizeof(recvbuf));

        printf("============================== RECV ==============================\n");
        printf("%s\n", recvbuf);
        printf("==================================================================\n");

        //备份原始报文，解析会破坏 recvbuf 的内容
        memcpy(rawbuf, recvbuf, sizeof(rawbuf));

        PraseHttpRequest(recvbuf, &TmpRequest);

        SendHttpRespone(confd, &TmpRequest, rawbuf);

        close(confd);
    }
    
    close(sockfd);

    return 0;
}
