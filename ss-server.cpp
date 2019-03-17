#include <getopt.h>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <sys/types.h>
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <pthread.h>
#include <thread>
#include <memory>
#include <signal.h>
#include <memory>
#include <unistd.h>
#include <string.h>
#include <queue>
#include <unordered_map>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include <stdlib.h>
#include <netdb.h>
#include <limits>
#include <fcntl.h>

#include "ss-server.h"



struct message{
    struct ver_method header;
    struct sockaddr_in src_addr;
    struct sockaddr_in dst_addr;
    struct event_base * base;
    char stage;
    char select_method;
    char atyp;
    char rep;
    int src_fd;
    int dst_fd;
    short valid;
    struct bufferevent *bev1;
    struct bufferevent *bev2;
    struct event *ev1;
    struct event *ev2;
    struct event *ev_m;
    struct bufferevent *bev_c;
    char * buff;
    char * buff_s;
    char * buff_c;
    struct timeval tv;
    struct timeval timeout;
    status error;
    message(){
        //应当从内存池中申请一块区域
        valid = 0;
        buff = new char[1460];
        buff_s = new char[1460];
        buff_c = new char[1460];
        bzero(buff, 1460);
        evutil_timerclear(&timeout);
        timeout.tv_sec = 1;
    }
    ~message(){
        delete [] buff;
        delete [] buff_s;
        delete [] buff_c;
    }
    void update_time(){
        evutil_gettimeofday(&tv, NULL);
    }
    bool isExpired(){
        return false;
        struct timeval now, res;
        evutil_timerclear(&res);
        evutil_gettimeofday(&now, NULL);
        evutil_timersub(&now, &tv, &res);
        return evutil_timercmp(&res, &timeout, >);
    }
};



class Event{
    public:
    Event():
        base(event_base_new())
    {

    }
    struct event_base * getBase(){
        return base;
    }

    struct event_base *base;
    bool quiting = false;
};

class EventThread{
    public:
        EventThread(){
            pthread_create(&t, NULL, &loopFunc, (void *)&ev);
        }
        ~EventThread(){
            pthread_join(t, NULL);
        }
        struct event_base* get_base(){
            return ev.base;
        }
        Event ev;
        pthread_t t;
    private:
        static void *loopFunc(void * arg){
            Event *ev = (Event *)arg;
            event_base_loop(ev->base, EVLOOP_NO_EXIT_ON_EMPTY);
        }
};

class EventThreadPool{
    public:
    EventThreadPool(int num){
        for(int i = 0; i < num; ++i){
            EventThread *th = new EventThread();
            vec.push_back(th);
            res.push_back(th->get_base());
        }
    }

    ~EventThreadPool(){

    }

    std::vector<struct event_base*> get_bases(){
        return res;
    }

    std::vector<struct event_base*> res;
    std::vector<EventThread*> vec;
};


//编码和解码的工作留到后面做，这里先假设不加密的情况[TODO]
//class EncryptDecrypt{
//    public:
//    static encrypt(
//};






//一个连接从它开始创建，就要给他创建一个事件，这个事件随着两端的连接是否断开为标志，而
//不以其他情况为转移，只要两端的连接还在，那么就不应该断开这个连接

void close_fd(int fd, struct message *msg){
    shutdown(fd, SHUT_RDWR);
}

void close_events_and_fds(struct message * msg){
        close_fd(msg->src_fd, msg);
        close_fd(msg->dst_fd, msg);
        event_del(msg->ev1);
        event_del(msg->ev2);
}

//msg->ev4
void handleWriteToClient(void * arg){
    printf("handleWriteToClient is invoked!\n");
    struct message *msg = (struct message *)arg;
    msg->valid = send(msg->src_fd, msg->buff, msg->valid, 0);
    if(msg->valid > 0){
        bzero(msg->buff, msg->valid);
    }
    else{
        close_events_and_fds(msg);
    }
}

//msg->ev3
void handleWriteToServer(void * arg){
    printf("handleWriteToServer is invoked!\n");
    struct message *msg = (struct message *)arg;
    msg->valid = send(msg->dst_fd, msg->buff, msg->valid, 0);
    if(msg->valid > 0){
        bzero(msg->buff, msg->valid);
    }
    else{
        close_events_and_fds(msg);
    }
}

//msg->ev1
void handleReadFromClient(int fd, short event, void * arg){
    printf("handleReadFromClient is invoked!\n");
    struct message *msg = (struct message *)arg;

    if(event & EV_READ){
        //MTU最大为1500,除去TCP和IP头部，剩余1460字节
        msg->valid = recv(msg->src_fd, msg->buff, 1460, 0);
        if(msg->valid > 0){
            handleWriteToServer((void *)msg);
            event_add(msg->ev1, NULL);
        }
        else{
            close_events_and_fds(msg);
        }
    }
    else if(event & EV_TIMEOUT){
        close_events_and_fds(msg);
    }
}

//msg->ev2
void handleReadFromServer(int fd, short event, void * arg){
    printf("handleReadFromServer is invoked!\n");
    struct message *msg = (struct message *)arg;

    if(event & EV_READ){
        //MTU最大为1500,除去TCP和IP头部，剩余1460字节
        msg->valid = recv(msg->dst_fd, msg->buff, 1460, 0);

        if(msg->valid > 0){
            handleWriteToClient((void *)msg);
            event_add(msg->ev1, NULL);
        }
        else{
            close_events_and_fds(msg);
        }
    }
    else if(event & EV_TIMEOUT){
        close_events_and_fds(msg);
    }
}




int setnoblocking(int fd){
    int flag,old_flag;

    //这个msg->dst完全可以设定为非阻塞的,因为它已经给分配了fd
    flag = fcntl(fd, F_GETFL, 0);
    flag |= O_NONBLOCK;
    flag = fcntl(fd, F_SETFL, flag); //将连接套接字设置为非阻塞。

    return flag;
}

int setblocking(int fd){
    int flag,old_flag;

    //这个msg->dst完全可以设定为非阻塞的,因为它已经给分配了fd
    flag = fcntl(fd, F_GETFL, 0);
    flag &= ~O_NONBLOCK;
    flag = fcntl(fd, F_SETFL, flag); //将连接套接字设置为非阻塞。

    return flag;
}


void handleConnToServer(struct bufferevent *bev, short event, void * arg){
    if(event & BEV_EVENT_CONNECTED){
        printf("进入连接程序\n");
        struct message * msg = (struct message *)arg;
        msg->dst_fd = bufferevent_getfd(bev);

        bzero(msg->buff, 10);
        msg->buff[0] = '\x05';
        msg->buff[3] = '\x01';

        //返回地址为0.0.0.0和端口0，因为它只是通知信息，要满足报文结构
        msg->valid = send(msg->src_fd, msg->buff, 10, 0);

        //event_base_loop(msg->base, EVLOOP_NONBLOCK);
        if(msg->valid != 10){
            msg->error = ST_VALIDLEN;
            msg->stage = 5;
        }

        //持续监听
        //ev1 负责从客户端读取数据发送到远端服务器
        //ev2 负责从远端服务器读取数据发送到客户端

        msg->ev1 = event_new(msg->base, msg->src_fd, EV_READ|EV_PERSIST,  handleReadFromClient, (void *)msg);
        msg->ev2 = event_new(msg->base, msg->dst_fd, EV_READ|EV_PERSIST,  handleReadFromServer, (void *)msg);

        ////将两事件注册到msg上，在错误或关闭时，可以消去事件

        event_add(msg->ev1, NULL);
        event_add(msg->ev2, NULL);

        msg->stage = 6;
        //此处是正常状态，正常的跳出整个连接即可
    }
    else if(event & BEV_EVENT_TIMEOUT){
        //连接超时

    }
}

//*********************Stage的各种状态***********************//
//
//  成功的状态:
//
//  stage = 0: 建立与到客户端的连接
//
//  stage = 1; 对加密方法进行沟通
//
//  stage = 2; 对目标地址和端口进行沟通
//
//  stage = 3; 建立到目标的connect
//
//  stage = 4; 成功建立到目标的connect
//
//  stage = 5; 建立双向通信后，进行双方数据发送
//
//  失败的状态:
//
//  stage = 6; 未建立到客户端的连接
//
//  stage = 7; 建立了到客户端的连接，但未建立到服务器的连接
//
//  stage = 8; 建立了双向连接，但仍出现了错误
//
//
//*********************Stage的各种状态***********************//


//*********************各种错误的原因************************//
//  0.ERROR_CONNECT_CLIENT            未连接上客户端
//  1.ERROR_PROTOCOL                  传输协议出错
//  2.ERROR_VALID                   传输数据位错误，bf
//
//
//
//
//
//
//
//
//
//*********************各种错误的原因************************//

enum status{
    ERROR_CONNECT_CLIENT,
    ERROR_PROTOCOL
};


void handleConnFromMain(int fd, short event, void * arg){

    msg->stage = 1;

    struct message * msg = (struct message *)arg;

    while(true){
        if(msg->stage == 1){
            //1:接收版本号、加密方法数目和加密方法
            //2:从加密方法中，选择一项加密方法，并告知客户端
            msg->valid = recv(msg->src_fd, (char *)&msg->header, 2, 0);

            if((header.ver != SOCKS_VERSION5) || (msg->valid != 2)){
                //协议错误,根据sock5协议标准，此情况下无需回答
                msg->stage = 7;
                continue;
            }

            printf("len is %d\n", header.method);
            //选择加密方式
            if(header.method == 0){
                //默认不加密
                msg->select_method = 0x00;
            }
            else{
                //在剩下255种加密方法中选择一种,长度也可能不是255，由第二字节决定
                msg->select_method = rand()%header.method;
                char * methods = new char[header.method];
                msg->valid = recv(msg->src_fd, methods, header.method, 0);
                if(msg->valid != header.method){
                    msg->error = ST_VALIDLEN;
                    msg->stage = 4;
                    continue;
                }
                msg->select_method = methods[msg->select_method];
                delete [] methods;
            }

            //将选择出来的加密方式传给客户端
            //[TODO]为了简便，直接用0，后面修改
            //header.method = msg->select_method;
            header.method = '\x00';

            msg->valid = send(msg->src_fd, (char *)&header, 2, 0);

            if(msg->valid != 2){
                msg->error = ST_VALIDLEN;
                msg->stage = 4;
                continue;
            }
            msg->stage = 1;
        }
        else if(msg->stage == 1){
            printf("Change to stage 1!\n");
            //在 stage = 1 双方握手，确定目标服务器的IP、端口和协议信息
            struct connect_request recv_req;
            //从客户端接收信息
            msg->valid = recv(msg->src_fd, (char *)&recv_req, 4, 0);
            if(msg->valid != 4){
                msg->error = ST_VALIDLEN;
                msg->stage = 4;
                continue;
            }
            msg->atyp = recv_req.atyp;

            //这里最长的是域名，可能有128位，而IPV4与IPV6都只有几位，
            //用char足矣,且方便读取域名信息
            char address_len;
            switch(recv_req.atyp){
                case 0x01:
                    //IPV4类型
                    address_len = 4;
                    break;
                case 0x03:
                    msg->valid = recv(msg->src_fd, &address_len, 1, 0);
                    break;
                case 0x04:
                    break;
            }
            printf("address len is %d\n", address_len);
            char * address = new char[address_len];


            msg->valid = recv(msg->src_fd, address, address_len, 0);

            if(msg->valid != address_len){
                msg->error = ST_VALIDLEN;
                msg->stage = 4;
                continue;
            }


            unsigned short port;
            msg->valid = recv(msg->src_fd, (char *)&port, 2, 0);

            if(msg->valid != 2){
                msg->error = ST_VALIDLEN;
                msg->stage = 4;
                continue;
            }

            msg->dst_addr.sin_family = AF_INET;
            msg->dst_addr.sin_port   = port;


            //域名查找较为复杂，稍后完成[TODO]
            if(recv_req.atyp == 0x03){
                //使用域名查询
                struct hostent * h;
                char * tmpaddr = new char[address_len+1];
                memcpy(tmpaddr, address, address_len);
                tmpaddr[address_len-1] = '\0';

                h = gethostbyname(address);
                printf("Address:");
                for(int j = 0; j < address_len; ++j){
                    printf("%c", address[j]);
                }
                printf("\n");
                if(h){
                    int i = 0;
                    while(h->h_addr_list[i] != NULL){
                        //已经是网络字节序
                        //msg->dst_addr.sin_addr.s_addr = h->h_addr_list[i];
                        msg->dst_addr.sin_addr.s_addr = *((uint32_t *)h->h_addr_list[i]);
                        printf(" i  is  %d  ,address is  %s\n", i, h->h_addr_list[i]);
                        ++i;
                    }
                }
                else{
                    //没有查询到目标主机
                }

            }
            else if(recv_req.atyp == 0x01){
                //传递过来的数据都是网络字节序，可以直接转义使用,其他无论是
                //IPV4还是IPV6都可以直接设置传输
                msg->dst_addr.sin_addr.s_addr = *((uint32_t *)address);
            }
            else{
                //IPV6稍后完成[TODO]

            }


            printf("address:%s\n", inet_ntoa(msg->dst_addr.sin_addr));

            //如果没有在这里返回，则认为代理服务器已经获得远端服务器的ip与端口
            msg->stage = 2;
        }
        else if(msg->stage == 2){
            printf("Change to stage 2!\n");

            //连接到远端服务器
            msg->dst_fd = socket(AF_INET, SOCK_STREAM, 0);

            msg->bev_c = bufferevent_socket_new(msg->base, -1, BEV_OPT_CLOSE_ON_FREE);

            bufferevent_setcb(msg->bev_c, NULL, NULL, handleConnToServer, msg);

            //setblocking(msg->dst_fd);

            if (bufferevent_socket_connect(msg->bev_c,(struct sockaddr *)&msg->dst_addr, sizeof(msg->dst_addr)) < 0) {
                //没能建立连接的情况
                bufferevent_free(msg->bev_c);
                return;
            }

            break;
        }
        else if(msg->stage == 4){
            printf("Change to stage 4!\n");
            //没有成功建立起与远端服务器的连接,应该删除主事件，并关闭src_fd
            //远端服务器不可达
            bzero(msg->buff, 10);
            msg->buff[0] = '\x05';
            msg->buff[1] = '\x04';
            msg->buff[3] = '\x01';

            //协议错误
            if(msg->error == ST_PROTOCOL){
                msg->buff[1] = '\x02';
            }

            msg->valid = send(msg->src_fd, msg->buff, 10, 0);

            if(msg->valid != 10){
                msg->error = ST_VALIDLEN;
                msg->stage = 5;
                continue;
            }

            shutdown(msg->src_fd, SHUT_RDWR);
            break;
        }
        else if(msg->stage == 5){
            printf("Change to stage 5!\n");
            //建立起来了与远端服务器的连接，目的是析构客户端和远端服务器的连接,删除主事件,而生成了子事件的可以自己删除自己
            //正常结束

            bzero(msg->buff, 10);
            msg->buff[0] = '\x05';
            msg->buff[1] = '\x01';
            msg->buff[3] = '\x01';

            msg->valid = send(msg->src_fd, msg->buff, 10, 0);

            if(msg->valid != 10){
                msg->error = ST_VALIDLEN;
                msg->stage = 5;
                continue;
            }

            close_fd(msg->src_fd, msg);
            close_fd(msg->dst_fd, msg);
            break;
        }
    }
}

class Server{
    public:
        //初始化服务器信息
        Server(std::string server_ip, int server_port, std::string password, std::string method, int threadNum):
            eventThreadPool(threadNum)
        {
            bases = eventThreadPool.get_bases();

        }

        //创建一个线程池，用于处理来自于主线程的消息
        void start(){
            struct sockaddr_in source_addr;
            int socket_fd = listenPort(source_addr);
            //这里要循环，以在主端口上监听，如果有事件到来，那么
            while(true){
                struct message msg;
                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);

                msg.base = get_low_load_base();
                msg.src_fd = accept(socket_fd, (struct sockaddr *)&client_addr, &len);
                //使用小根堆，获得各个线程的负载情况，找到那些正在处理事件少的线程，分配给新到来的事件

                msg.stage = 0;


                //仅注册一次，分发给子线程处理，它的所有解析、传送、处理和关闭都交给子线程执行

                msg.ev_m = event_new(msg.base, msg.src_fd, EV_READ|EV_TIMEOUT, handleConnFromMain, (void *)&msg);
                event_add(msg.ev_m, NULL);
                //event_base_dispatch(msg.base);
                //event_base_loop(msg.base, EVLOOP_ONCE);

                //setnoblocking(msg.src_fd);
                //setblocking(msg.src_fd);
                //应该在主线程中注册一个signal事件，用于结束所有存在的事件[TODO]
            }
        }

        struct event_base * get_low_load_base(){
            struct event_base * res;
            int t, c = 9999;
            for(int i = 0; i < 4; ++i){
                t = event_base_get_num_events(bases[i], EVENT_BASE_COUNT_ADDED);
                printf("total num is %d, event base %d have %d events!\n", 4, i, t);
                if(t < c){
                    c = t;
                    res = bases[i];
                }
            }
            printf("selected base has %d events\n", c);
            return res;
        }

        int listenPort(struct sockaddr_in &source_addr){
            int socket_fd = socket(AF_INET, SOCK_STREAM, 0);

            struct sockaddr_in serverAddr;

            bzero(&serverAddr, sizeof(serverAddr));

            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(1080);
            serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);

            int flag = 1;
            if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0)
            {
                printf("socket setsockopt error=%d(%s)!!!\n", errno, strerror(errno));
                exit(1);
            }

            if(bind(socket_fd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1){
                perror("bind");
                exit(1);
            }

            if(listen(socket_fd, 2048) == -1){
                perror("listen");
                exit(1);
            }

            return socket_fd;
        }

    private:
        std::string server_ip;
        std::string password;
        std::string method;
        int server_port;
        int threadNum;
        EventThreadPool eventThreadPool;
        std::vector<struct event_base *> events;
        std::vector<struct event_base*> bases;
};


int main(int argc, char ** argv){
    int opt;
    srand(time(0));

    std::string server_ip;
    std::string local_address_ip;
    std::string password;
    std::string method;

    int server_port;
    int local_port;
    int timeout;

    char * configPath = NULL;
    const char * str = "c|s|p|l|k|m|t|h";
    server_ip = "127.0.0.1";
    server_port = 1080;
    password = "123456";
    method = "chacha20";
    std::unordered_map<std::string, char> methods;
    //配置文件，读取所有加密方式

	//while(opt = getopt(argc, argv, str)){/*{{{*/
	//	switch(opt){
	//		case 'c':
    //            break;
	//		case 'h':
	//			std::cout << "" << std::endl
	//				<< "  usage:" << std::endl
	//				<< "" << std::endl
	//				<< "    ss-server" << std::endl
	//				<< "" << std::endl
	//				<< "       -s <server_host>           Host name or IP address of your remote server." << std::endl
	//				<< "       -p <server_port>           Port number of your remote server." << std::endl
	//				<< "       -l <local_port>            Port number of your local server." << std::endl
	//				<< "       -k <password>              Password of your remote server." << std::endl
	//				<< "       -m <encrypt_method>        Encrypt method: rc4-md5, " << std::endl
	//				<< "                                  aes-128-gcm, aes-192-gcm, aes-256-gcm," << std::endl
	//				<< "                                  aes-128-cfb, aes-192-cfb, aes-256-cfb," << std::endl
	//				<< "                                  aes-128-ctr, aes-192-ctr, aes-256-ctr," << std::endl
	//				<< "                                  camellia-128-cfb, camellia-192-cfb," << std::endl
	//				<< "                                  camellia-256-cfb, bf-cfb," << std::endl
	//				<< "                                  chacha20-ietf-poly1305," << std::endl
	//				<< "                                  salsa20, chacha20 and chacha20-ietf." << std::endl
	//				<< "                                  The default cipher is rc4-md5." << std::endl
	//				<< "" << std::endl
	//				<< "       [-a <user>]                Run as another user." << std::endl
	//				<< "       [-f <pid_file>]            The file path to store pid." << std::endl
	//				<< "       [-t <timeout>]             Socket timeout in seconds." << std::endl
	//				<< "       [-c <config_file>]         The path to config file." << std::endl
	//				<< "       [-n <number>]              Max number of open files." << std::endl
	//				<< "       [-i <interface>]           Network interface to bind." << std::endl
	//				<< "       [-b <local_address>]       Local address to bind." << std::endl
	//				<< "" << std::endl
	//				<< "       [-u]                       Enable UDP relay." << std::endl
	//				<< "       [-U]                       Enable UDP relay and disable TCP relay." << std::endl
	//				<< "       [-6]                       Resovle hostname to IPv6 address first." << std::endl
	//				<< "" << std::endl
	//				<< "       [-d <addr>]                Name servers for internal DNS resolver." << std::endl
	//				<< "       [--reuse-port]             Enable port reuse." << std::endl
	//				<< "       [--fast-open]              Enable TCP fast open." << std::endl
	//				<< "                                  with Linux kernel > 3.7.0." << std::endl
	//				<< "       [--acl <acl_file>]         Path to ACL (Access Control List)." << std::endl
	//				<< "       [--manager-address <addr>] UNIX domain socket address." << std::endl
	//				<< "       [--mtu <MTU>]              MTU of your network interface." << std::endl
	//				<< "       [--mptcp]                  Enable Multipath TCP on MPTCP Kernel." << std::endl
	//				<< "       [--no-delay]               Enable TCP_NODELAY." << std::endl
	//				<< "       [--key <key_in_base64>]    Key of your remote server." << std::endl
	//				<< "       [--plugin <name>]          Enable SIP003 plugin. (Experimental)" << std::endl
	//				<< "       [--plugin-opts <options>]  Set SIP003 plugin options. (Experimental)" << std::endl
	//				<< "" << std::endl
	//				<< "       [-v]                       Verbose mode." << std::endl
	//				<< "       [-h, --help]               Print this message." << std::endl;
    //            exit(0);
	//		case 's':
    //            server_ip = optarg;
	//			break;
    //        case 'p':
	//			server_port = atoi(optarg);
    //            break;
	//		case 'l':
	//			local_port = atoi(optarg);
    //            break;
    //        case 'k':
    //            password = optarg;
    //            break;
    //        case 'm':
    //            method = optarg;
    //            break;
    //        default:
    //            break;
    //        //先做这些选项[TODO],剩下的以后做
    //    }
    //}
/*}}}*/

    int threadNum = 4;

    //创建监听服务器，监听事件的到来,服务器的设计
    //要考虑多个使用者的情况

    Server server(server_ip, server_port, password, method, threadNum);
    server.start();


    //使用事件循环，来处理sock5报文,它负责将从端口
    //接收到的报文转给其他线程来处理,这个event
    //为主线程,负责将socket_fd注册到event上
    //  事件的处理用来监听主线程上端口信息的变化情
    //  况，并把信息传递给一个全局对象,这个对象是
    //  epoll的包装


    return 0;
}
