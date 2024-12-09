#include "Server.h"
#include <arpa/inet.h>  // arpa/inet.h 包含了socket.h，它声明了更多的套接字相关的函数
#include <sys/epoll.h>  // epoll，创建epoll_create
#include <stdio.h>
#include <fcntl.h>  // fcntl函数
#include <errno.h>  // errno == EAGAIN
#include <strings.h>  // strcasecmp字符串比较，不区分大小写
#include <string.h>
#include <sys/stat.h>  // stat
#include <fcntl.h>
#include <assert.h>  // 断言 assert(fd > 0);
#include <sys/sendfile.h>  // sendfile函数
#include <dirent.h>  // scandir遍历目录
#include <unistd.h>  // close()
#include <stdlib.h>  // free
#include <pthread.h>
#include <ctype.h>


struct FdInfo
{
    int fd;         // 文件描述符
    int epfd;       // epoll树根节点
    pthread_t tid;  // 线程ID
};

// 初始化监听的套接字。返回值：用于监听的文件描述符，参数：端口
int initListenFd(unsigned short port)
{
    printf("初始化监听的套接字\n");
    // 1. 创建监听的fd
    // AF_INET: 使用IPv4格式的ip地址; 流式协议；0默认TCP
    int lfd = socket(AF_INET, SOCK_STREAM, 0);  
    if (lfd == -1)  // 创建失败了
    {
        perror("socket");
        return -1;
    }
    // 2. 设置端口复用，这个可写可不写，最好写一下
    int opt = 1; // 第四个参数需要一个整型变量的地址，值指定为1则代表lfd可以复用绑定的端口
    // 设置sock属性,第三个参数写成SO_REUSEPORT也可以
    int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt); // sizeof后面只是一个关键字可以不用括号
    if (ret == -1)  // 端口复用设置失败了
    {
        perror("setsockopt");
        return -1;
    }
    // 3. 绑定
    // bind的第二个参数本来是要使用 sockaddr 这个结构体的，但是我们一般都是使用和这个
    // 结构体占用相同内存大小的另一个结构体 sockaddr_in 来代替
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;    // 地址读协议，设置为IPV4
    addr.sin_port = htons(port);  // 指定端口，主机字节序（小端）转化为网络字节序（大端）
    // 要绑定的本地的IP地址，0地址对应的宏INADDR_ANY，表示绑定本地的任意一个地址
    // 本来也应该指定为大端的，但是0地址大小端都是一样的，就不需要转化了
    addr.sin_addr.s_addr = INADDR_ANY; 
    ret = bind(lfd, (struct sockaddr*)&addr, sizeof addr);
    if (ret == -1)
    {
        perror("bind");
        return -1;
    }
    // 4. 设置监听
    // 即使你设置得再大，内核会将其恢复成128，最多也就128
    ret = listen(lfd, 128); // 一次性最多能接收128个客户端请求
    if (ret == -1)
    {
        perror("listen");
        return -1;
    }
    // 返回fd，返回给函数调用者
    return lfd;
}

// 启动epoll
int epollRun(int lfd)
{
    printf("开始启动epoll");
    // 1. 创建epoll实例，epoll底层是红黑树，实质上就是创建树的根节点
    int epfd = epoll_create(1);  // 这个函数的参数已经被弃用了，只要指定一个大于0的数字即可
    if (epfd == -1)
    {
        perror("epoll_create");
        return -1;
    }
    // 2.将用于监听的文件描述符添加到epoll树上。即lfd上树
    struct epoll_event ev; // epoll事件结构体
    ev.data.fd = lfd;  // ev.data是一个联合体，几个成员共用一块内存，所以只能取一个。一般使用里面的fd
    ev.events = EPOLLIN;  // 要监听的事件，读事件
    // 四个参数：epoll的根节点epfd,要进行的事件（添加），要操作的文件描述符，
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    if (ret == -1)
    {
        perror("epoll_ctl");
        return -1;
    }
    // 3.检测 （持续监测）
    struct epoll_event evs[1024]; // 传入传出参数
    int size = sizeof(evs) / sizeof(struct epoll_event); // 数组的元素个数
    while (1) // 服务器启动起来之后一般是不会停下来的，要停的话手动即可
    {
        // 使用epoll_wait就是委托内核帮助我们检测通过 epoll_ctl 添加到 epoll 模型上的
        // 这些文件描述符对应的事件是不是被激活了
        int num = epoll_wait(epfd, evs, size, -1);  // -1表示一直阻塞直到被激活
        // 遍历被激活的事件
        for (int i = 0; i < num; ++i)
        {
            // 初始化结构体
            // 属性->链接器->输入->库依赖性->加上pthread
            struct FdInfo* info = (struct FdInfo*)malloc(sizeof(struct FdInfo));
            int fd = evs[i].data.fd;  // 取出对应的文件描述符
            info->epfd = epfd;
            info->fd = fd;
            // 判断这个文件描述符是不是用于监听的文件描述符
            if (fd == lfd)  // 是用于监听的文件描述符
            {
                printf("建立新连接 accept,第%d次", i);
                // 建立新连接 accept
                //acceptClient(lfd, epfd); // 检测到新连接的话就会建立新连接，并把新的文件描述符也添加到epoll树上
                pthread_create(&info->tid, NULL, acceptClient, info);
            }
            else  // 不是用于监听的文件描述符，那它就是用于监听的文件描述符
            {
                printf("接收对端的数据，读数据 accept,第%d次", i);
                // 主要是接收对端的数据，读数据
                //recvHttpRequest(fd, epfd);
                pthread_create(&info->tid, NULL, recvHttpRequest, info);
            }
        }
    }

    return 0;
}

// 和客户端建立连接
void* acceptClient(void* arg)
{
    printf("开始和客户端建立连接");

    // 类型转换
    struct FdInfo* info = (struct FdInfo*)arg;
    // 1. 建立连接
    // 第二个参数是一个传出参数，用来保存客户端的IP和端口信息，如果需要就定义一个
    // struct sockaddr*类型来保存即可，没有这个需求的话直接指定为空即可
    // 第三个参数是计算第二个参数所占用的内存大小
    int cfd = accept(info->fd, NULL, NULL);
    if (cfd == -1)
    {
        perror("accept");
        return NULL;
    }
    // 2. 默认是阻塞的，要设置非阻塞。epoll的边沿非阻塞模式效率最高
    int flag = fcntl(cfd, F_GETFL);  // 得到文件描述符的属性
    flag |= O_NONBLOCK;  // 追加非阻塞属性
    fcntl(cfd, F_SETFL, flag);  // 设置新属性

    // 3. cfd添加到epoll中，跟前面epollRun函数中添加lfd是一样的
    struct epoll_event ev;
    ev.data.fd = cfd;
    ev.events = EPOLLIN | EPOLLET; // EPOLLET设置 边沿模式
    int ret = epoll_ctl(info->epfd, EPOLL_CTL_ADD, cfd, &ev);
    if (ret == -1)
    {
        perror("epoll_ctl");
        return NULL;
    }
    printf("acceptClient threadId：%ld\n", info->tid);
    free(info);

    return NULL;
}
//int acceptClient(int lfd, int epfd)
//{
//    printf("开始和客户端建立连接");
//    // 1. 建立连接
//    // 第二个参数是一个传出参数，用来保存客户端的IP和端口信息，如果需要就定义一个
//    // struct sockaddr*类型来保存即可，没有这个需求的话直接指定为空即可
//    // 第三个参数是计算第二个参数所占用的内存大小
//    int cfd = accept(lfd, NULL, NULL);
//    if (cfd == -1)
//    {
//        perror("accept");
//        return -1;
//    }
//    // 2. 默认是阻塞的，要设置非阻塞。epoll的边沿非阻塞模式效率最高
//    int flag = fcntl(cfd, F_GETFL);  // 得到文件描述符的属性
//    flag |= O_NONBLOCK;  // 追加非阻塞属性
//    fcntl(cfd, F_SETFL, flag);  // 设置新属性
//
//    // 3. cfd添加到epoll中，跟前面epollRun函数中添加lfd是一样的
//    struct epoll_event ev;
//    ev.data.fd = cfd;
//    ev.events = EPOLLIN | EPOLLET; // EPOLLET设置 边沿模式
//    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
//    if (ret == -1)
//    {
//        perror("epoll_ctl");
//        return -1;
//    }
//
//    return cfd;
//}

// 接收http请求
// 客户端把数据发送完了。并且已经断开了连接，则需要服务器端将这个用于通信的文件描述符
// 从epoll树上摘除，所以还需要添加一个参数epfd，epoll树的根节点
void* recvHttpRequest(void* arg)
{
    // 类型转换
    struct FdInfo* info = (struct FdInfo*)arg;

    printf("开始接收数据了...\n");
    int len = 0, total = 0;
    char tmp[1024] = { 0 };  // 每次写数据都先写到tmp里
    char buf[4096] = { 0 };  // 使用的是get请求，数据块不会太大，创建一个buf用来存储数据即可
    // 在前面已经将epoll的模式设置为边沿非阻塞模式，所以epoll检测到文件描述符对应的
    // 读事件之后，它之后通知一次，所以在接收通知之后我们需要一次性将数据全部接收完毕
    while ((len = recv(info->fd, tmp, sizeof tmp, 0)) > 0)
    {
        if (total + len < sizeof buf)
        {
            // memcpy每次都是从头部开始拷贝数据的
            // 三个参数：目标地址指针，指向要复制的源文件的指针，要复制的字节数
            memcpy(buf + total, tmp, len);  // 把tmp的数据拷贝到buf内，注意要更新新拷贝的起点
        }
        // 每循环一次就让total加上写入的数据的长度，因为memcpy是从头部写入的
        total += len;
    }
    // 判断数据是否被接收完毕
    if (len == -1 && errno == EAGAIN)  // 接收完毕了  #include <errno.h>  
    {
        // 解析请求行,也只需要解析请求行即可，其他的不需要解析
        char* pt = strstr(buf, "\r\n"); // 在字符串buf里面寻找字符串"\r\n"
        int reqLen = pt - buf;  // 获取请求行的长度（结束地址-起始地址）
        buf[reqLen] = '\0';     // 人为截断
        parseRequestLine(buf, info->fd);  // 解析请求行
    }
    else if (len == 0)  // 客户端断开了连接
    {
        // 客户端断开了连接
        epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL); // 从epoll树上删除文件描述符cfd
        close(info->fd);
    }
    else  // 错误
    {
        perror("recv");
    }

    printf("recvMsg threadId：%ld\n", info->tid);
    free(info);

    return NULL;
}
//int recvHttpRequest(int cfd, int epfd)
//{
//    printf("开始接收数据了...\n");
//    int len = 0, total = 0;
//    char tmp[1024] = { 0 };  // 每次写数据都先写到tmp里
//    char buf[4096] = { 0 };  // 使用的是get请求，数据块不会太大，创建一个buf用来存储数据即可
//    // 在前面已经将epoll的模式设置为边沿非阻塞模式，所以epoll检测到文件描述符对应的
//    // 读事件之后，它之后通知一次，所以在接收通知之后我们需要一次性将数据全部接收完毕
//    while ((len = recv(cfd, tmp, sizeof tmp, 0)) > 0)
//    {
//        if (total + len < sizeof buf)
//        {
//            // memcpy每次都是从头部开始拷贝数据的
//            // 三个参数：目标地址指针，指向要复制的源文件的指针，要复制的字节数
//            memcpy(buf + total, tmp, len);  // 把tmp的数据拷贝到buf内，注意要更新新拷贝的起点
//        }
//        // 每循环一次就让total加上写入的数据的长度，因为memcpy是从头部写入的
//        total += len;
//    }
//    // 判断数据是否被接收完毕
//    if (len == -1 && errno == EAGAIN)  // 接收完毕了  #include <errno.h>  
//    {
//        // 解析请求行,也只需要解析请求行即可，其他的不需要解析
//        char* pt = strstr(buf, "\r\n"); // 在字符串buf里面寻找字符串"\r\n"
//        int reqLen = pt - buf;  // 获取请求行的长度（结束地址-起始地址）
//        buf[reqLen] = '\0';     // 人为截断
//        parseRequestLine(buf, cfd);  // 解析请求行
//    }
//    else if (len == 0)  // 客户端断开了连接
//    {
//        // 客户端断开了连接
//        epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, NULL); // 从epoll树上删除文件描述符cfd
//        close(cfd);
//    }
//    else  // 错误
//    {
//        perror("recv");
//    }
//}

// 解析请求行
// 请求行分为三部分：1.请求方式(get/post); 2.请求的静态资源;3.使用的http协议版本
// line：请求行字符串；cfd：解析出来需要回复给客户端，回复数据需要用于通信的文件描述符
int parseRequestLine(const char* line, int cfd)
{
    printf("解析请求行");
    // 解析请求行  get(或port) /xxx/1.jpg http /1.1(http请求行格式)
    // /xxx/1.jpg 第一个"/"表示的是服务器访问的资源根目录path
    char method[12];  // 要么是get要么是post，不需要太大
    char path[1024];  // 用来储存客户端请求的静态资源
    // sscanf函数：对格式化字符进行拆分
    // [^ ]正则匹配规则，取出非空格的字符。[1-9]:1~9范围,[a-z]a~z范围,
    // [A,b,c,D,e,f]集合匹配，[1-9,f-x]:这两个集合内的字符匹配
    sscanf(line, "%[^ ] %[^ ]", method, path);  // get /xxx/1.jpg http /1.1
    printf("method: %s, path: %s\n", method, path); // method: get, path: /xxx/1.jpg
    if (strcasecmp(method, "get") != 0)  // strcasecmp匹配的时候不区分大小写，#include <strings.h> 
    {
        // 对于get以外的请求，都忽略
        return -1;
    }
    decodeMsg(path, path); // 解码函数，解析非英文字符
    // 处理客户端请求的 静态资源(目录或者文件)
    char* file = NULL;
    if (strcmp(path, "/") == 0)  // 是资源根目录
    {
        file = "./";             // 从绝对路径改为相对路径
    }
    else                         // 不是资源根目录
    {
        file = path + 1;         // 把(/xxx/1.jpg)的第一个 / 去掉
    }
    // 获取文件属性
    struct stat st; // 传出参数
    int ret = stat(file, &st); // #include <sys/stat.h>
    if (ret == -1)
    {
        // 文件不存在 -> 回复404界面
        // 首先是发送http的响应头
        // 参数：cfd：用于通信的文件描述符； 404：状态码； "Not Found"：状态描述；
        // getFileType(".html")：类型； -1：让浏览器自己计算数据块大小
        sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), -1);// -1是让浏览器自己去读大小，我不指定
        sendFile("404.html", cfd);
        return 0;  // 404表示有问题，直接return 不继续了
    }
    // 判断文件类型
    if (S_ISDIR(st.st_mode)) // 是目录
    {
        // 把这个目录中的内容发送给客户端
        sendHeadMsg(cfd, 200, "OK", getFileType(".html"), -1);
        sendDir(file, cfd);  // 发送目录
    }
    else  // 是文件
    {
        // 把文件的内容发送给客户端
        sendHeadMsg(cfd, 200, "OK", getFileType(file), st.st_size);
        sendFile(file, cfd);
    }

    return 0;
}

// 根据文件的后缀或者名字去得到它所对应的content-type
// content-type可参考网站：https://tool.oschina.net/commons
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

// 发送目录
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
    // 先把body前面的拼接出来，把目录名作为title标题
    sprintf(buf, "<html><head><title>%s</title></head><body><table>", dirName);
    struct dirent** namelist;
    // scandir函数参数：1要遍历的目录名字，2传出参数（一个二级指针的地址=三级指针）  
    // 3一个回调函数（指定遍历时的过滤规则），4排序方式（这里使用Linux写好了的函数）
    // 设置alphasort：属性 -> C/C++ -> 语言 -> C++语言标准 -> 设置为：C11(GNU Dialect)(-std=gnu11)
    int num = scandir(dirName, &namelist, NULL, alphasort);  // 只遍历一层目录，#include <dirent.h>
    for (int i = 0; i < num; ++i)
    {
        // 取出文件名, namelist 指向的是一个指针数组 struct dirent* tmp[]
        // struct dirent* tmp[]：tmp是一个数组，这个数组里面的每一个元素都是struct dirent*类型的
        char* name = namelist[i]->d_name;
        struct stat st;
        char subPath[1024] = { 0 };
        sprintf(subPath, "%s/%s", dirName, name);
        stat(subPath, &st);
        if (S_ISDIR(st.st_mode))  // 是目录
        {
            // 一行两列，一列是目录名，一列是文件大小
            // a标签 <a href="">name</a>   :a标签有一个属性href，就可以让网页跳转到""该目录下
            // \"%s/\"："\"是因为要转义，因为前面已经有双引号了，这个双引号需要转义；
            // "%s/"的 "/"是因为需要跳转到目录里面，没有"/"则表示要访问这个文件
            sprintf(buf + strlen(buf), 
                "<tr><td><a href=\"%s/\">%s</a></td><td>%ld</td></tr>", 
                name, name, st.st_size);// 目录名字，长度
        }
        else  // 是文件
        {
            sprintf(buf + strlen(buf),
                "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>",
                name, name, st.st_size);
        }
        // 拼接完成之后，发送出去
        send(cfd, buf, strlen(buf), 0);
        memset(buf, 0, sizeof(buf)); // 清空buf
        free(namelist[i]);  // 释放数组的每一个元素
    }
    // for循环结束，则body标签拼接完成，接下来就是拼接结束标签
    sprintf(buf, "</table></body></html>"); // 拼接结束标签
    send(cfd, buf, strlen(buf), 0);  // 将最终的结果发送出去，给客户端
    free(namelist);  // 释放数组

    return 0;
}

// 发送文件，参数：文件名，用于通信的文件描述符
// 读一部分，发一部分，底层用的是Tcp流式传输协议
int sendFile(const char* fileName, int cfd)
{
    // 1. 打开文件
    int fd = open(fileName, O_RDONLY);  // 只读打开
    assert(fd > 0); // #include <assert.h>  // 断言，打开成功
#if 0 
    while (1)
    {
        char buf[1024];  // 每次从文件中读1k的数据
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
        else  // 异常
        {
            perror("read");
        }
    }
#else
    // 用下面这种方法发送文件，代码简单
    off_t offset = 0;  // 偏移量，一般为空即可，表示是在起始位置开始读的
    // lseek 是一个用于更改文件描述符fd的当前文件位置或获取当前文件位置的函数。
    // SEEK_END：这是一个特殊的值，表示我们希望从文件的末尾开始计算偏移量。
    // 结合偏移量0，这意味着我们要获取文件的当前末尾位置（即文件的大小）
    int size = lseek(fd, 0, SEEK_END); // 到尾部去了，这样的话直接读数据会读出空数据
    lseek(fd, 0, SEEK_SET);  // 把文件指针移动回文件头部
    while (offset < size)  // 对于大文件，需要一直读
    {
        // 关于sendfile的第三个参数offset：
        // 1.发送数据之前，根据偏移量开始读文件数据；
        // 2.发送数据之后 更改偏移量
        int ret = sendfile(cfd, fd, &offset, size - offset); // #include <sys/sendfile.h>
        printf("ret value: %d\n", ret); 
        if (ret == -1 && errno == EAGAIN) // AGAIN宏表示你没有数据，你可以再次尝试
        {
            printf("没数据...\n");
        }
    }
#endif
    close(fd);

    return 0;
}

// 发送响应头(状态行+响应头)。参数：用于通信的fd，状态码，状态描述，content-type，数据长度
// content-length对应的长度设置为-1的话是让浏览器自己去读大小，我不指定
int sendHeadMsg(int cfd, int status, const char* descr, const char* type, int length)
{
    // 状态行
    char buf[4096] = { 0 };
    // sprintf:拼接字符串
    sprintf(buf, "http/1.1 %d %s\r\n", status, descr);  // 换行\r\n
    // 响应头
    // + strlen(buf)是为了让内存地址后移，拼接
    sprintf(buf + strlen(buf), "content-type: %s\r\n", type);
    sprintf(buf + strlen(buf), "content-length: %d\r\n\r\n", length); // 最后还有一个空行

    send(cfd, buf, strlen(buf), 0); // 把数据块发送出去

    return 0;
}

// 将字符转换为整形
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

// 解码。to -> 存储解码之后的数据，传出参数；from -> 被解码的数据，传入参数
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
