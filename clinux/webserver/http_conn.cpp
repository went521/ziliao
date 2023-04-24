#include "http_conn.h"
#include <iostream>

using namespace std;


// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "/home/went/clinux/webserver/resources";


int setunblcokfd(int fd){
    int old_option=fcntl(fd,F_GETFL);
    int new_option=old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}


//one_shot 防止多个线程同时连接处理同一个fd
void addfd(int epollfd,int fd,bool one_shot){
    epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLRDHUP;
    if(one_shot){
        ev.events|=EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd, &ev);
    int pre_option=setunblcokfd(fd);
}

//修改某个fd
void modfd(int epollfd, int fd,int ev){
    epoll_event event;
    event.data.fd=fd,
    event.events=ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

//删除epoll中的某个fd
void remove(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_address=addr;
    m_sockfd=sockfd;
    //用于定时器
    cldata.address=addr;
    cldata.sockfd=sockfd;
    // 端口复用
    int reuse=2;
    setsockopt(sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd,sockfd,true);
    m_user_count++;
    init();
}

void http_conn::init(){
    m_check_state=CHECK_STATE_REQUESTLINE;
    m_start_line=0;
    m_checked_idx=0;
    m_read_idx=0;

    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接
    m_method=GET;
    m_url = 0;              
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_write_idx = 0;

    bytes_to_send = 0;
    bytes_have_send = 0;

    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

void http_conn::close_conn(){
    if(this->m_sockfd!=-1){
        remove(m_epollfd,m_sockfd);
        m_sockfd=-1;
        m_user_count--;
    }
}

// 非阻塞读
bool http_conn::read(http_conn* users ,sort_timer_lst* timer_lst,int epollfd){
    if(m_read_idx>=READ_BUFFER_SIZE){
        return false;
    }

    int read_btyes=0;
    util_timer* timer = users[m_sockfd].cldata.timer;
    while(1){
        read_btyes=recv(this->m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        if(read_btyes==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK){
                //数据已经读完
                break;
            }
            //如果发生读错误，则关闭连接，并移除其对应的定时器
            if( read_btyes < 0 )
                {
                    // 如果发生读错误，则关闭连接，并移除其对应的定时器
                    if( errno != EAGAIN )
                    {
                        cb_func( &users[m_sockfd].cldata,epollfd);
                        if( timer )
                        {
                            timer_lst->del_timer( timer );
                        }
                    }
                }
            return false;
        }else if(read_btyes==0){//关闭连接
            // 如果对方已经关闭连接，则我们也关闭连接，并移除对应的定时器。
            cb_func( &users[m_sockfd].cldata ,epollfd);
            if( timer )
            {
                timer_lst->del_timer( timer );
            }
            return false;
        }
        //如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间。
        if( timer ) {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;
                        //cout<<m_read_buf<<endl;
                        printf( "adjust timer once\n" );
                        timer_lst->adjust_timer( timer );
                        cout<<"flag"<<endl;
                    }
                    
        m_read_idx+=read_btyes;//更新位置
    }

    cout<<"读数据完毕"<<endl;
    //cout<<m_read_buf<<endl;

    

    
    return true;
}

// 非阻塞写
bool http_conn::write(){
    if(bytes_to_send==0){
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        init();
        return true;
    }

    int tmp=0;
    //cout<<"m_write_buf11111111:  "<<m_write_buf<<endl;
   // cout<<"resource:"<<m_iv[1].iov_base<<endl;
    cout<<"准备写数据"<<endl;
    while(1){
        //cout<<"falg1111"<<endl;
        tmp=writev(m_sockfd,m_iv,m_iv_count);
        //cout<<"tmp="<<tmp<<endl;
        // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
        // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
        if(tmp<=-1){
            if(errno==EAGAIN){
                //cout<<"went"<<endl;
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        //cout<<"flag1"<<endl;
        bytes_have_send+=tmp;
        bytes_to_send-=tmp;

        if(bytes_have_send>=m_iv[0].iov_len){
            //响应报文已经发送完毕，但是响应的资源还没发送完毕
            m_iv[0].iov_len=0;
            m_iv[1].iov_len=bytes_to_send;
            m_iv[1].iov_base=m_file_address+bytes_have_send-m_write_idx;
        }else{//响应报文没用一次发完，更新位置准备发第二次
            m_iv[0].iov_len-=tmp;
            m_iv[0].iov_base=m_write_buf+ bytes_have_send;
        }
        //数据发送完毕
        if(bytes_to_send<=0){
            unmap();
            modfd(m_epollfd,m_sockfd,EPOLLIN);

            if(m_linger){
                init();
                return true;
            }else{
                return false;
            }
        }
    }
    cout<<"写数据完毕"<<endl;
    return true;
}


void http_conn::process(){
    //解析http请求
    //cout<<"flag7"<<endl;
    HTTP_CODE read_ret= process_read();
    if(read_ret==NO_REQUEST){
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return ;
    }
    //cout<<"flag0"<<endl;
    //服务器响应
    bool write_ret=process_write(read_ret);
    if(!write_ret){
        close_conn();
    }
    //cout<<"flag1"<<endl;
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
    cout<<"parse request and reponse"<<endl;
}

//解析一行数据 是否符合格式
http_conn::LINE_STATUS http_conn::parse_line(){
    char tmp;
    //cout<<"m_read_idx="<<m_read_idx<<endl;
    for( ; m_checked_idx < m_read_idx; m_checked_idx++){
        tmp=m_read_buf[m_checked_idx];
        //cout<<"m_read_buf["<<m_checked_idx<<"]=" <<tmp<<endl;
        if(tmp=='\r'){
            if((m_checked_idx+1)==m_read_idx){
                return LINE_OPEN;
            }else if(m_read_buf[m_checked_idx+1] == '\n'){
                m_read_buf[m_checked_idx++]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
        }else if(tmp=='\n'){
            if((m_checked_idx>1) && (m_read_buf[m_checked_idx-1]=='\r')){
                m_read_buf[m_checked_idx-1]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        //cout<<"wewrwe"<<endl;
    }
    return LINE_OPEN;
}


http_conn::HTTP_CODE http_conn::parse_request_line( char* text ){
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    if (! m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符
    char* method = text;
    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }
    /**
     * http://192.168.110.129:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr( m_url, '/' );
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查头
    //cout<<"CHECK_STATE_HEADER="<<CHECK_STATE_HEADER<<endl;
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers( char* text ){
    if(text[0]=='\0'){
        if(m_content_length!=0){
            m_check_state=CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }else if( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
        //cout<<"m_host"<<m_host<<endl;
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content( char* text ){
    if(m_read_idx>=(m_checked_idx+m_content_length)){
        text[m_content_length]='\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_sta=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;
    char* text =0;
    //cout<<parse_line()<<endl;
    while(((line_sta==LINE_OK) && (m_check_state==CHECK_STATE_CONTENT)) 
            || ((line_sta=parse_line()) == LINE_OK)){  

        //cout<<"flag232"<<endl;
        text= getline();
        m_start_line=m_checked_idx;
        //cout<<"get a new line:"<<text<<endl;

        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:{
            ret=parse_request_line(text);
            if(ret==BAD_REQUEST){
                return BAD_REQUEST;
            }
            break; 
        }
        case CHECK_STATE_HEADER:{
            ret=parse_headers(text);
            if(ret==BAD_REQUEST){
                return BAD_REQUEST;
            }else if(ret==GET_REQUEST){
                //cout<<"do_request"<<endl;
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:{
            ret = parse_content( text );
            if ( ret == GET_REQUEST ) {
                return do_request();
            }
            
        }
        default:
            return INTERNAL_ERROR;
        }        
    }
    return NO_REQUEST;
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request(){
    strcpy(m_real_file,doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file+len,m_url,FILENAME_LEN-len-1);
    if(stat(m_real_file,&m_file_stat)<0){
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    int fd = open(m_real_file,O_RDONLY);
    //
    m_file_address=(char*)mmap(NULL,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    //cout<<"m_file_address="<<m_file_address<<endl;
    close(fd);
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

bool http_conn::process_write( HTTP_CODE ret ){
    cout<<"RET:"<<ret<<endl;
    switch(ret){
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:{
            add_status_line(200,ok_200_title);
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_len=m_write_idx;
            m_iv[0].iov_base=m_write_buf;
            m_iv[1].iov_len=m_file_stat.st_size;
            m_iv[1].iov_base=m_file_address;
            m_iv_count=2;
           // cout<<"m_iv[1].iov_base="<<(char* )(m_iv[1].iov_base)<<endl;
            bytes_to_send=m_write_idx+m_file_stat.st_size;
            //cout<<"m_write_idx="<<m_write_idx<<endl;
            //cout<<"m_file_stat.st_size="<<m_file_stat.st_size<<endl;
            //cout<<"bytes_to_send="<<bytes_to_send<<endl;
            return true;
        }
        default:
            return false;
    }
   // cout<<"flag2"<<endl;
    m_iv[0].iov_len=m_write_idx;
    m_iv[0].iov_base=m_write_buf;
    m_iv_count=1;
    bytes_to_send=m_write_idx;
    return true;
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ... ) {
    if(m_write_idx>=WRITE_BUFFER_SIZE){
        return false;
    }
    va_list arg_list;
    va_start(arg_list,format);
    int len = vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE-m_write_idx-1,format,arg_list);
    if(len>WRITE_BUFFER_SIZE-m_write_idx-1)
        return false;
    m_write_idx+=len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}


int http_conn::m_user_count=0;
int http_conn::m_epollfd=-1;