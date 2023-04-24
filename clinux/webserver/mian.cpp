#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"


#define MAX_FD 65536   // 最大的文件描述符个数
//#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量


using namespace std;

static int pipefd[2];

//static int epollfd = 0;

sort_timer_lst timer_lst;


void addfd( int epollfd, int fd )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setunblcokfd( fd );
}

void addsig(int signal, void (handler)(int)){

    struct sigaction sa ;
    memset(&sa,0,sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    //注册信号集  
    sigaction(signal,&sa,NULL);

}

//闹钟信号 向管道中写数据
void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

void addsig( int sig )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = sig_handler;
    //即在处理完消息后，继续执行原来中断的函数，像什么也没发生一样)
    sa.sa_flags |= SA_RESTART;
    //全部临时阻塞
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

void timer_handler(int epollfd)
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick(epollfd);
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func( client_data* user_data,int  epollfd)
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0 );
    assert( user_data );
    close( user_data->sockfd );
    printf( "close fd %d\n", user_data->sockfd );
}

int main(int argc, char* argv[]){
    
    if(argc<=1){
        cout<<"usage:"<<basename(argv[0])<<endl; 
        return 1;
    }

    
    //字符串转换成整型数的一个函数
    int port= atoi(argv[1]);
    //忽略client进程杀死后的信号，让server继续运行
    addsig(SIGPIPE,SIG_IGN);

    ThreadPool<http_conn>*pool =nullptr;
    try
    {
        pool=new ThreadPool<http_conn>;
    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }

    

    int listenfd=socket(PF_INET,SOCK_STREAM,0);

    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_addr.s_addr=INADDR_ANY;
    address.sin_port=htons(port);
    address.sin_family=AF_INET;


    int epollfd=epoll_create(1);

    //端口复用
    int reuse=2;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    
    int ret=0;
    ret=bind(listenfd,(sockaddr*)&address,sizeof(address));
    //5是监听的数量（未连接+已连接（两个队列）），已连接的马上会被accep（）取出 
    ret=listen(listenfd,5);

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setunblcokfd( pipefd[1] );
    addfd( epollfd, pipefd[0] );

    
    epoll_event events[MAX_EVENT_NUMBER];
    //1的参数没用意义 
    
    addfd(epollfd,listenfd,false);
    http_conn::m_epollfd=epollfd;

     // 设置信号处理函数
    addsig( SIGALRM );
    addsig( SIGTERM );
    bool stop_server = false;

    http_conn* users=new http_conn[MAX_FD];
    bool timeout = false;
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号

    while(!stop_server){
        //timeout : 阻塞时间
        // - 0 : 不阻塞
        // - -1 : 阻塞，直到检测到fd数据发生变化，解除阻塞
        // - > 0 : 阻塞的时长（毫秒）
        int cnt=epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);

        if(cnt<0 && errno!=EINTR){
            cout<<"epoll failure"<<endl;
            break;
        }

        for(int i=0; i<cnt; i++){
            int sockfd=events[i].data.fd;
            //监听信号  建立新连接
            if(sockfd==listenfd){
                struct sockaddr_in client_addr;
                socklen_t client_addr_len=sizeof(client_addr);
                //获取建立的新连接
                int connfd=accept(sockfd,(sockaddr*)&client_addr,&client_addr_len);
                
                if(connfd<0){
                    cout<<"errno is : "<<errno<<endl;
                    continue;
                }else if(http_conn::m_user_count>=MAX_FD){
                    cout<<"server is in busy"<<endl;
                    close(connfd);
                }
                users[connfd].init(connfd,client_addr);

                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd].cldata;
                timer->cb_func = cb_func;
                time_t cur = time( NULL );
                timer->expire = cur + 3 * TIMESLOT;
                users[connfd].cldata.timer = timer;
                timer_lst.add_timer( timer );

            }else if(events[i].events&(EPOLLRDHUP | EPOLLHUP | EPOLLERR)){//连接错误则关闭连接          
                users[sockfd].close_conn();
            }else if(( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN )){
                // 处理信号
                //cout<<"闹钟"<<endl;
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 ) {
                    continue;
                } else if( ret == 0 ) {
                    continue;
                } else  {
                    for( int i = 0; i < ret; ++i ) {
                        switch( signals[i] )  {
                            case SIGALRM:
                            {
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
                modfd(epollfd,sockfd,EPOLLIN);
            }else if(events[i].events & EPOLLIN){//读数据
                if(users[sockfd].read(users,&timer_lst,epollfd)){
                    pool->append(users+sockfd);
                }else {
                    users[sockfd].close_conn();
                }
            }else if(events[i].events & EPOLLOUT){// 写数据
                if(!users[sockfd].write()){
                    users[sockfd].close_conn();
                }
            }
        }
        // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if( timeout ) {
            timer_handler(epollfd);
            timeout = false;
        }
    }
    

    close(epollfd);
    close(listenfd);
    close( pipefd[1] );
    close( pipefd[0] );
    delete [] users;
    delete pool;
    
    return 0;
}