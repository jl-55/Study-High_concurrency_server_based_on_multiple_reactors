#include "Server.h"
#include <arpa/inet.h>  // arpa/inet.h 包含了socket.h
#include <sys/epoll.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>  // 断言
#include <sys/sendfile.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <ctype.h>

struct FdInfo
{
    int fd;
    int epfd;
    pthread_t tid;
};

int initListenFd(unsigned short port)
{
    // 1. 创建监听的fd
    int lfd = socket(AF_INET, SOCK_STREAM, 0);  // AF_INET: 使用IPv4格式的ip地址;流式协议；0默认TCP
    if (lfd == -1)
    {
        perror("socket");
        return -1;
    }
    // 2. 设置端口复用
    int opt = 1; // 第四个参数需要一个整型变量的地址，值指定为1则代表lfd可以复用绑定的端口
    // 设置sock属性,第三个参数写成SO_REUSEPORT也可以
    int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt); // sizeof后面只是一个关键字可以不用括号
    if (ret == -1)
    {
        perror("setsockopt");
        return -1;
    }
    // 3. 绑定
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;  // 地址读协议，设置为IPV4
    addr.sin_port = htons(port);  // 指定端口，主机字节序（小端）转化为网络字节序（大端）
    addr.sin_addr.s_addr = INADDR_ANY; // 要绑定的本地的IP地址，0地址对应的宏INADDR_ANY，表示绑定本地的任意一个地址
    ret = bind(lfd, (struct sockaddr*)&addr, sizeof addr);
    if (ret == -1)
    {
        perror("bind");
        return -1;
    }
    // 4. 设置监听
    ret = listen(lfd, 128); // 一次性最多能接收128个客户端请求
    if (ret == -1)
    {
        perror("listen");
        return -1;
    }
    // 返回fd
    return lfd;
}

int epollRun(int lfd)
{
    // 1. 创建epoll实例，epoll底层是红黑树，实质上就是创建树的根节点
    int epfd = epoll_create(1);  //这个函数的参数已经被弃用了，只要指定一个大于0的数字即可
    if (epfd == -1)
    {
        perror("epoll_create");
        return -1;
    }
    // 2.添加到epoll树上。lfd上树
    struct epoll_event ev;
    ev.data.fd = lfd;  // ev.data是一个联合体，几个成员共用一块内存，所以只能取一个
    ev.events = EPOLLIN;  // 要监听的事件，读事件
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    if (ret == -1)
    {
        perror("epoll_ctl");
        return -1;
    }
    // 3.检测 （持续监测）
    struct epoll_event evs[1024]; // 传入传出参数
    int size = sizeof(evs) / sizeof(struct epoll_event); // 数组的元素个数
    while (1)
    {
        int num = epoll_wait(epfd, evs, size, -1);  // -1表示一直阻塞直到被激活
        // 遍历被激活的事件
        for (int i = 0; i < num; ++i)
        {
            struct FdInfo* info = (struct FdInfo*)malloc(sizeof(struct FdInfo));// 属性->链接器->输入->库依赖性->加上pthread
            int fd = evs[i].data.fd;
            info->epfd = epfd;
            info->fd = fd;
            if (fd == lfd)
            {
                // 建立新连接 accept
                // acceptClient(lfd, epfd);
                pthread_create(&info->tid, NULL, acceptClient, info);
            }
            else
            {
                // 主要是接收对端的数据
                // recvHttpRequest(fd, epfd);
                pthread_create(&info->tid, NULL, recvHttpRequest, info);
            }
        }
    }

    return 0;
}

// int acceptClient(int lfd, int epfd)
void* acceptClient(void* arg)
{
    struct FdInfo* info = (struct FdInfo*)arg;
    // 1. 建立连接
    int cfd = accept(info->fd, NULL, NULL);
    if (cfd == -1)
    {
        perror("accept");
        return NULL;
    }
    // 2. 设置非阻塞。epoll的边沿非阻塞模式效率最高
    int flag = fcntl(cfd, F_GETFL);  // 得到文件描述符的属性
    flag |= O_NONBLOCK;  //追加非阻塞属性
    fcntl(cfd, F_SETFL, flag);

    // 3. cfd添加到epoll中
    struct epoll_event ev;
    ev.data.fd = cfd;
    ev.events = EPOLLIN | EPOLLET; //EPOLLET设置 边沿模式
    int ret = epoll_ctl(info->epfd, EPOLL_CTL_ADD, cfd, &ev);
    if (ret == -1)
    {
        perror("epoll_ctl");
        return NULL;
    }
    printf("acceptcliet threadId: %ld\n", info->tid);
    free(info);

    return NULL;
}

// int recvHttpRequest(int cfd, int epfd)
void* recvHttpRequest(void* arg)
{
    struct FdInfo* info = (struct FdInfo*)arg;
    printf("开始接收数据了...\n");
    int len = 0, total = 0;
    char tmp[1024] = { 0 };  // 每次写数据都先写到tmp里
    char buf[4096] = { 0 };
    while ((len = recv(info->fd, tmp, sizeof tmp, 0)) > 0)
    {
        if (total + len < sizeof buf)
        {
            //三个参数：目标地址指针，指向要复制的源文件的指针，要复制的字节数
            memcpy(buf + total, tmp, len);  // 把tmp的数据拷贝到buf内，注意要更新新拷贝的起点
        }
        total += len; // 每循环一次就让total加上写入的数据的长度，因为memcpy是从头部写入的
    }
    // 判断数据是否被接收完毕
    if (len == -1 && errno == EAGAIN)  // 接收完毕了
    {
        // 解析请求行
        char* pt = strstr(buf, "\r\n"); // 在字符串buf里面寻找字符串"\r\n"
        int reqLen = pt - buf;
        buf[reqLen] = '\0';
        parseRequestLine(buf, info->fd);
    }
    else if (len == 0)  // 客户端断开了连接
    {
        // 客户端断开了连接
        epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL); // 删除文件描述符cfd
        close(info->fd);
    }
    else  // 错误
    {
        perror("recv");
    }
    printf("recvMsg threadId: %ld\n", info->tid);
    free(info);
    return NULL;
}


int parseRequestLine(const char* line, int cfd)
{
    // /xxx/1.jpg 第一个"/"表示的是服务器访问的资源根目录path
    // 解析请求行  get(或port) /xxx/1.jpg http /1.1(http请求行格式)
    char method[12];
    char path[1024];
    // sscanf函数：对格式化字符进行拆分
    // [^ ]正则匹配规则，取出非空格的字符。[1-9]:1~9范围,[a-z]a~z范围,
    // [A,b,c,D,e,f]集合匹配，[1-9,f-x]:这两个集合内的字符匹配
    sscanf(line, "%[^ ] %[^ ]", method, path);  // get /xxx/1.jpg http /1.1
    printf("method: %s, path: %s\n", method, path); // method:get, path:/xxx/1.jpg
    if (strcasecmp(method, "get") != 0)
    {
        return -1;
    }
    decodeMsg(path, path); // 解码函数，解析非英文字符
    // 处理客户端请求的静态资源(目录或者文件)
    char* file = NULL;
    if (strcmp(path, "/") == 0)// 是资源根目录
    {
        file = "./"; // 从绝对路径改为相对路径
    }
    else
    {
        file = path + 1; // (/xxx/1.jpg)去掉/
    }
    // 获取文件属性
    struct stat st;
    int ret = stat(file, &st); // #include <sys/stat.h>
    if (ret == -1)
    {
        // 文件不存在 -> 回复404界面
        sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), -1);// -1是让浏览器自己去读大小，我不指定
        sendFile("404.html", cfd);
        return 0;
    }
    // 判断文件类型
    if (S_ISDIR(st.st_mode)) // 是目录
    {
        // 把这个目录中的内容发送给客户端
        sendHeadMsg(cfd, 200, "OK", getFileType(".html"), -1);
        sendDir(file, cfd);
    }
    else
    {
        // 把文件的内容发送给客户端
        sendHeadMsg(cfd, 200, "OK", getFileType(file), st.st_size);
        sendFile(file, cfd);
    }

    return 0;
}

const char* getFileType(const char* name)
{
    // a.jpg a.mp4 a.html
    // 自右向左查找‘.’字符, 如不存在返回NULL
    const char* dot = strrchr(name, '.');
    if (dot == NULL)
        return "text/plain; charset=utf-8";	// 纯文本
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".ico") == 0)
        return "image/x-icon";
    if (strcmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".au") == 0)
        return "audio/basic";
    if (strcmp(dot, ".wav") == 0)
        return "audio/wav";
    if (strcmp(dot, ".avi") == 0)
        return "video/x-msvideo";
    if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)
        return "video/quicktime";
    if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)
        return "video/mpeg";
    if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)
        return "model/vrml";
    if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)
        return "audio/midi";
    if (strcmp(dot, ".mp3") == 0)
        return "audio/mp3";
    if (strcmp(dot, ".mp4") == 0)
        return "video/mpeg4";
    if (strcmp(dot, ".ogg") == 0)
        return "application/ogg";
    if (strcmp(dot, ".pac") == 0)
        return "application/x-ns-proxy-autoconfig";

    // 如果不是以上的格式，则返回utf-8格式的纯文本格式
    return "text/plain; charset=utf-8";
}

/* 简单的html网页格式          // 标签一般都是成对的
<html>                         // 网页标签的根结点
    <head>                     // 头部标签
        <title>test</title>    // 网页标签的标题，显示在tab页上的，内容test
    </head>
    <body>                     // 身体标签
        <table>                // 在主题内容上使用了列表table(可以使用多行多列)
            <tr>               // <tr>标签表示行，这里是两行
                <td></td>      // <td>标签表示列，内容放置在<td></td>中间
                <td></td>
            </tr>
            <tr>
                <td></td>
                <td></td>
            </tr>
        </table>
    </body>
</html>
*/
int sendDir(const char* dirName, int cfd)
{
    char buf[4096] = { 0 };
    sprintf(buf, "<html><head><title>%s</title></head><body><table>", dirName);
    struct dirent** namelist;
    // scandir函数参数：1要遍历的目录名字，2传出参数（一个二级指针）
    // 3一个回调函数（指定遍历时的过滤规则），4排序方式（这里使用Linux写好了的函数）
    // 设置alphasort：属性 -> C/C++ -> 语言 -> C++语言标准
    int num = scandir(dirName, &namelist, NULL, alphasort);  // 只遍历一层目录
    for (int i = 0; i < num; ++i)
    {
        // 取出文件名 namelist 指向的是一个指针数组 struct dirent* tmp[]
        char* name = namelist[i]->d_name;
        struct stat st;
        char subPath[1024] = { 0 };
        sprintf(subPath, "%s/%s", dirName, name);
        stat(subPath, &st);
        if (S_ISDIR(st.st_mode))
        {
            // a标签 <a href="">name</a>   :a标签有一个属性href，就可以让网页跳转到该目录下
            sprintf(buf + strlen(buf), 
                "<tr><td><a href=\"%s/\">%s</a></td><td>%ld</td></tr>", 
                name, name, st.st_size);// 目录名字，长度
        }
        else
        {
            sprintf(buf + strlen(buf),
                "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>",
                name, name, st.st_size);
        }
        send(cfd, buf, strlen(buf), 0);
        memset(buf, 0, sizeof(buf));
        free(namelist[i]);
    }
    sprintf(buf, "</table></body></html>"); // 拼接结束标签
    send(cfd, buf, strlen(buf), 0);
    free(namelist);

    return 0;
}

int sendFile(const char* fileName, int cfd)
{
    // 1. 打开文件
    int fd = open(fileName, O_RDONLY);  // 只读打开
    assert(fd > 0); // #include <assert.h>  // 断言，打开成功
#if 0 
    while (1)
    {
        char buf[1024];// 每次从文件中读1k的数据
        int len = read(fd, buf, sizeof buf);
        if (len > 0)
        {
            send(cfd, buf, len, 0);  // 把数据发送给客户端
            usleep(10);  // 休眠10微秒，这很重要。让接收方有时间处理接受的数据
        }
        else if (len == 0)  // 文件读完了
        {
            break;
        }
        else
        {
            perror("read");
        }
    }
#else
    //用下面这种方法发送文件，代码简单
    off_t offset = 0;
    // lseek 是一个用于更改文件描述符fd的当前文件位置或获取当前文件位置的函数。
    // SEEK_END：这是一个特殊的值，表示我们希望从文件的末尾开始计算偏移量。
    // 结合偏移量0，这意味着我们要获取文件的当前末尾位置（即文件的大小）
    int size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    while (offset < size)  // 对于大文件，需要一直读
    {
        // 关于sendfile的第三个参数offset：
        // 1.发送数据之前，根据偏移量开始读文件数据；
        // 2.发送数据之后 更改偏移量
        int ret = sendfile(cfd, fd, &offset, size - offset); // #include <sys/sendfile.h>
        printf("ret value: %d\n", ret);
        if (ret == -1 && errno == EAGAIN)
        {
            printf("没数据...\n");
        }
    }
#endif
    close(fd);

    return 0;
}

int sendHeadMsg(int cfd, int status, const char* descr, const char* type, int length)
{
    // 状态行
    char buf[4096] = { 0 };
    // sprintf:拼接字符串
    sprintf(buf, "http/1.1 %d %s\r\n", status, descr);  // 换行\r\n
    // 响应头
    // + strlen(buf)是为了让内存地址后移
    sprintf(buf + strlen(buf), "content-type: %s\r\n", type);
    sprintf(buf + strlen(buf), "content-length: %d\r\n\r\n", length); // 最后还有一个空行

    send(cfd, buf, strlen(buf), 0); //把数据块发送出去

    return 0;
}

// 将十六进制字符转换为十进制整形数
int hexToDec(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return 0;
}

// 解码
// to 存储解码之后的数据, 传出参数, from被解码的数据, 传入参数
void decodeMsg(char* to, char* from)
{
    for (; *from != '\0'; ++to, ++from)
    {
        // isxdigit -> 判断字符是不是16进制格式, 取值在 0-f
        // Linux%E5%86%85%E6%A0%B8.jpg
        if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2]))
        {
            // 将16进制的数 -> 十进制 将这个数值赋值给了字符 int -> char
            // B2 == 178
            // 将3个字符, 变成了一个字符, 这个字符就是原始数据
            *to = hexToDec(from[1]) * 16 + hexToDec(from[2]);

            // 跳过 from[1] 和 from[2] 因此在当前循环中已经处理过了
            from += 2;
        }
        else
        {
            // 字符拷贝, 赋值
            *to = *from;
        }

    }
    *to = '\0';
}
