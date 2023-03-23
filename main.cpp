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
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量

// 添加文件描述符
extern void addfd( int epollfd, int fd, bool one_shot );
extern void removefd( int epollfd, int fd );

void addsig(int sig, void( handler )(int)){
    // 信号处理需要告诉这个信号是什么sig,以及一个信号处理函数handler
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );//清空一下
    sa.sa_handler = handler;//设置handler
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );//使用sigaction注册信号
}

int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    //获取端口号
    int port = atoi( argv[1] );
    //对SIGPIPI信号进行处理
    addsig( SIGPIPE, SIG_IGN );

    //创建线程池，线程池有一个模板任务类http_conn
    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        return 1;
    }

    //这是一个保存所有客户端信息的一个users数组（65535），最大的文件描述符个数
    http_conn* users = new http_conn[ MAX_FD ];

    // 1.创建一个用于监听listen的文件描述符
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );

    int ret = 0;


    // 端口复用（通过setsockopt设置端口listenfd复用-->SO_REUSERADDR）
    // 要在绑定之前设置
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    // 通过Bind函数绑定端口
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );//转为网络字节序
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    ret = listen( listenfd, 5 );

    // 2.创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );//调用epoll_create来创建一个epoll文件描述符
    // 添加文件描述符到到epoll对象中
    addfd( epollfd, listenfd, false );
    /*
    // 向epoll中添加需要监听的文件描述符
        void addfd( int epollfd, int fd, bool one_shot ) {
            epoll_event event;
            event.data.fd = fd;
            event.events = EPOLLIN | EPOLLRDHUP;
            if(one_shot) 
            {
                // 防止同一个通信被不同的线程处理
                event.events |= EPOLLONESHOT;
            }
            epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
            // 设置文件描述符非阻塞
            setnonblocking(fd);  
        }
    */

    // http_conn::m_epollfd这是一个静态的变量，表示所有socket的事件都注册到同一个epoll_fd上
    // 所有的http_conn都共享这个epollfd
    http_conn::m_epollfd = epollfd;

    // 主线程的while死循环
    while(true) {
        
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            // epoll出错了
            printf( "epoll failure\n" );
            break;
        }

        // 循环遍历数组事件
        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;
            
            if( sockfd == listenfd ) {
                // 是listenfd说明有客户端连接建立成功了
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                // 调用accept获取连接的文件描述符connfd
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength ); 
                
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) {
                    // 说明文件描述符满了，连接太多了
                    close(connfd);//关闭客户端（应该回写一个信息给客户端）
                    continue;
                }
                // 将新的user进行初始化，将connfd作为索引
                users[connfd].init( connfd, client_address);

                /*
                    // 初始化连接,外部调用初始化套接字地址
                    void http_conn::init(int sockfd, const sockaddr_in& addr){
                        m_sockfd = sockfd;
                        m_address = addr;
                        
                        // 端口复用
                        int reuse = 1;
                        setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
                        addfd( m_epollfd, sockfd, true );
                        m_user_count++;
                        init();
                    }
                */

            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {
                // 这三种事件是异常断开或者说错误的事件
                users[sockfd].close_conn();
                /*
                    // 关闭连接
                    void http_conn::close_conn() {
                        if(m_sockfd != -1) {
                            removefd(m_epollfd, m_sockfd);
                            m_sockfd = -1;
                            m_user_count--; // 关闭一个连接，将客户总数量-1
                        }
                    }
                */

            } else if(events[i].events & EPOLLIN) {
                // 判断是不是读事件发生
                if(users[sockfd].read()) {// 一次性把所有数据都读完
                    pool->append(users + sockfd);// 交给工作线程去处理
                } else {
                    // 读失败，对方关闭了链接，就关闭
                    users[sockfd].close_conn();
                }
                /*
                    // 循环读取客户数据，直到无数据可读或者对方关闭连接
                    bool http_conn::read() {
                        if( m_read_idx >= READ_BUFFER_SIZE ) {
                            return false;
                        }
                        int bytes_read = 0;
                        while(true) {
                            // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
                            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, 
                            READ_BUFFER_SIZE - m_read_idx, 0 );
                            if (bytes_read == -1) {
                                if( errno == EAGAIN || errno == EWOULDBLOCK ) {
                                    // 没有数据可读了，都读完了
                                    break;
                                }
                                return false;// 其他错误   
                            } else if (bytes_read == 0) {   // 对方关闭连接
                                return false;
                            }
                            m_read_idx += bytes_read;
                        }
                        return true;
                    }
                */

            }  else if( events[i].events & EPOLLOUT ) {
                // 是写事件
                if( !users[sockfd].write() ) {// 一次性写完
                    users[sockfd].close_conn();
                }

                /*
                    // 写HTTP响应
                    bool http_conn::write()
                    {
                        int temp = 0;
                        int bytes_have_send = 0;    // 已经发送的字节
                        int bytes_to_send = m_write_idx;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数
                        
                        if ( bytes_to_send == 0 ) {
                            // 将要发送的字节为0，这一次响应结束。
                            modfd( m_epollfd, m_sockfd, EPOLLIN ); 
                            init();
                            return true;
                        }

                        while(1) {
                            // 分散写
                            temp = writev(m_sockfd, m_iv, m_iv_count);
                            if ( temp <= -1 ) {
                                // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
                                // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
                                if( errno == EAGAIN ) {
                                    modfd( m_epollfd, m_sockfd, EPOLLOUT );
                                    return true;
                                }
                                unmap();
                                return false;
                            }
                            bytes_to_send -= temp;
                            bytes_have_send += temp;
                            if ( bytes_to_send <= bytes_have_send ) {
                                // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
                                unmap();
                                if(m_linger) {
                                    init();
                                    modfd( m_epollfd, m_sockfd, EPOLLIN );
                                    return true;
                                } else {
                                    modfd( m_epollfd, m_sockfd, EPOLLIN );
                                    return false;
                                } 
                            }
                        }
                    }
                */
            }
        }
    }
    
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}