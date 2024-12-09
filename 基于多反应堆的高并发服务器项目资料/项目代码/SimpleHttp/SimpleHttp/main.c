#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "Server.h"

int main(int argc, char* argv[])
{
    // 要传入两个参数：端口port 和 被读取的目录文件path
    if (argc < 3)
    {
        printf("./a.out port path\n");
        return -1;
    }
    unsigned short port = atoi(argv[1]);  // 字符型->整型
    // 切换到服务器的工作路径
    chdir(argv[2]);  // #include <unistd.h>
    // 初始化用于监听的套接字
    int lfd = initListenFd(port);
    // 启动服务器程序
    epollRun(lfd);

    return 0;
}