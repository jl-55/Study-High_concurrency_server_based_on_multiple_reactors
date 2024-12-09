#include <stdio.h>
#include <unistd.h>  // chdir(argv[2]);
#include <stdlib.h>  // atoi    // 字符型->整型
#include "Server.h"

// 给main指定命令行参数
int main(int argc, char* argv[])
{
    // argv[0] 就是可执行程序的名字
    // 要传入两个参数：端口port 和 被读取的资源文件path
    if (argc < 3)
    {
        printf("./a.out port path\n");
        printf("启动的时候需要添加参数：端口和访问的资源根目录路径\n");
        return -1;
    }
    printf("服务器开始启动\n");
    unsigned short port = atoi(argv[1]);  // 字符型->整型,#include <stdlib.h> 
    // 需要把当前服务器的进程切换到用户指定的这个path目录下
    // 切换到服务器的工作路径
    chdir(argv[2]);  // #include <unistd.h>

    // 首先，初始化用于监听的套接字
    int lfd = initListenFd(port);
    printf("初始化用于监听的套接字成功了,文件描述符为：%d\n", lfd);
    // 然后，启动服务器程序
    printf("开始启动服务器程序\n");
    epollRun(lfd);
    printf("服务器启动了\n");

    return 0;
}