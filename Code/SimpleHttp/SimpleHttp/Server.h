#pragma once

// 初始化监听的套接字。返回值：用于监听的文件描述符，参数：端口
int initListenFd(unsigned short port);

// 启动epoll
int epollRun(int lfd);

// 和客户端建立连接
//int acceptClient(int lfd, int epfd);
void* acceptClient(void* arg);
// 接收http请求
//int recvHttpRequest(int cfd, int epfd);
void* recvHttpRequest(void* arg);

// 解析请求行
// line：请求行字符串；cfd：解析出来需要回复给客户端，回复数据需要用于通信的文件描述符
int parseRequestLine(const char* line, int cfd);

// 发送文件，参数：文件名，用于通信的文件描述符
int sendFile(const char* fileName, int cfd);
// 发送响应头(状态行+响应头)。参数：用于通信的fd，状态码，状态描述，content-type，数据长度
// content-length对应的长度设置为-1的话是让浏览器自己去读大小，我不指定
int sendHeadMsg(int cfd, int status, const char* descr, const char* type, int length);
// 根据文件的后缀或者名字去得到它所对应的content-type
// content-type可参考网站：https://tool.oschina.net/commons
const char* getFileType(const char* name);
// 发送目录
int sendDir(const char* dirName, int cfd);

// 解码函数
// 将字符转换为整形
int hexToDec(char c);
// 解码。to -> 存储解码之后的数据，传出参数；from -> 被解码的数据，传入参数
void decodeMsg(char* to, char* from);