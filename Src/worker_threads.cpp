/*
 * CThread.cc
 *
 *  Created on: Mar 4, 2013
 *      Author: yaowei
 */

#include "worker_threads.h"
#include "global_settings.h"
#include "utils.h"
#include "socket_wrapper.h"
#include <stdio.h>
#include <iostream>
using namespace std;

std::vector<LIBEVENT_THREAD*> CWorkerThread::vec_libevent_thread_;
int CWorkerThread::init_count_ = 0;
pthread_mutex_t	CWorkerThread::init_lock_ = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  CWorkerThread::init_cond_ = PTHREAD_COND_INITIALIZER;

int CWorkerThread::freetotal_ = 0;
int CWorkerThread::freecurr_  = 0;
boost::mutex CWorkerThread::mutex_;
std::vector<CONN*> CWorkerThread::vec_freeconn_;
std::vector<CONN*> CWorkerThread::vec_using_conn_;


CWorkerThread::CWorkerThread()
{
	last_thread_ = -1;
}

CWorkerThread::~CWorkerThread()
{
}

/* 初始化worker线程池 */
bool CWorkerThread::InitThreads(struct event_base* main_base)
{

	InitFreeConns();

	LOG4CXX_INFO(g_logger, "Initializes worker threads...");

	for(unsigned int i=0; i<utils::G<CGlobalSettings>().thread_num_; ++i)
	{
		LIBEVENT_THREAD* libevent_thread_ptr = new LIBEVENT_THREAD;
		/* 建立每个worker线程和主监听线程通信的管道 */
		int fds[2];
		if (pipe(fds) != 0)
		{
			LOG4CXX_ERROR(g_logger, "CThread::InitThreads:Can't create notify pipe");
			return false;
		}
		libevent_thread_ptr->notify_receive_fd = fds[0];
		libevent_thread_ptr->notify_send_fd	   = fds[1];

		if(!SetupThread(libevent_thread_ptr))
		{
			utils::SafeDelete(libevent_thread_ptr);
			LOG4CXX_ERROR(g_logger, "CThread::InitThreads:SetupThread failed.");
			return false;
		}

		vec_libevent_thread_.push_back(libevent_thread_ptr);
	}

	for (unsigned int i = 0; i < utils::G<CGlobalSettings>().thread_num_; i++)
	{
		CreateWorker(WorkerLibevent, vec_libevent_thread_.at(i));
	}

	 /* 等待所有线程都已经启动完毕. */
	WaitForThreadRegistration(utils::G<CGlobalSettings>().thread_num_);

	LOG4CXX_INFO(g_logger, "Create threads success. we hava done all the libevent setup.");

	return true;
}

void CWorkerThread::CreateWorker(void *(*func)(void *), void *arg)
{
	pthread_t thread;
	pthread_attr_t attr;
	int ret;

	pthread_attr_init(&attr);

	if ((ret = pthread_create(&thread, &attr, func, arg)) != 0)
	{
		LOG4CXX_FATAL(g_logger, "CWorkerThread::CreateWorker:Can't create thread:" << strerror(ret));
		exit(1);
	}
}


void *CWorkerThread::WorkerLibevent(void *arg)
{
	LIBEVENT_THREAD *me = static_cast<LIBEVENT_THREAD *>(arg);

	me->thread_id = pthread_self();

	RegisterThreadInitialized();

	event_base_dispatch(me->base);

	return NULL;
}

bool CWorkerThread::SetupThread(LIBEVENT_THREAD* me)
{
	me->base = event_base_new();
	assert(me != NULL);

	/* 通过每个worker线程的读管道监听来自master的通知 */
	me->notify_event = *event_new(me->base, me->notify_receive_fd, EV_READ|EV_PERSIST, ReadPipeCb, (void*)me);
	assert(&me->notify_event != NULL);

	if (event_add(&me->notify_event, NULL) == -1)
	{
		int error_code = EVUTIL_SOCKET_ERROR();
		LOG4CXX_ERROR(g_logger, "CWorkerThread::SetupThread:event_add errorCode = " << error_code
								<< ", description = " << evutil_socket_error_to_string(error_code));
		return false;
	}

	return true;
}

void CWorkerThread::ReadPipeCb(int fd, short event, void* arg)
{

	LIBEVENT_THREAD *libevent_thread_ptr = static_cast<LIBEVENT_THREAD*>(arg);
	assert(libevent_thread_ptr != NULL);

	/* read from master-thread had write, a byte 代表一个客户端连接 */
	char buf[1];
	if (read(fd, buf, 1) != 1)
	{
		LOG4CXX_ERROR(g_logger, "CWorkerThread::ThreadLibeventProcess:Can't read from libevent pipe.");
		return;
	}

    if(buf[0] == 'c')
    {
        /* 将主线程塞到队列中的连接pop出来 */
        CONN_INFO connInfo;
        if(!libevent_thread_ptr->list_conn.pop_front(connInfo))
        {
            LOG4CXX_ERROR(g_logger, "CWorkerThread::ThreadLibeventProcess:list_conn.pop_front NULL.");
            return;
        }

        /*初始化新连接，将连接事件注册入libevent */
        if(connInfo.sfd != 0)
        {
            CONN* conn = InitNewConn(connInfo, libevent_thread_ptr);
            if(NULL == conn)
            {
                LOG4CXX_ERROR(g_logger, "CWorkerThread::ReadPipeCb:Can't listen for events on sfd = " << connInfo.sfd);
                close(connInfo.sfd);
            }
            LOG4CXX_TRACE(g_logger, "CWorkerThread::ReadPipeCb thread id = " << conn->thread->thread_id);
        }
    }
    else if(buf[0] == 't')
    {
        dprintf("ReadPipeCb(): get the trans signal.\n");
        TRANS_MSG tm;
        libevent_thread_ptr->list_trans.pop_front(tm);
        cout << "ReadPipeCb: get the trans data: "<< tm.content <<endl;
        string send_msg = "||" + utils::TransToByteStr(tm.content.size(), 4) + string("03") + utils::TransToByteStr(tm.send_id, 8) + utils::TransToByteStr(tm.recv_id, 8) + tm.content;
        if(!SocketOperate::WriteSfd(tm.send_fd, send_msg.c_str(), send_msg.size()))
        {
            LOG4CXX_ERROR(g_logger, "CWorkerThread::ClientTcpReadCb:send sfd .error = " << strerror(errno));
        }
    }
}

CONN* CWorkerThread::InitNewConn(const CONN_INFO& conn_info, LIBEVENT_THREAD* libevent_thread_ptr)
{
	CONN* conn = GetConnFromFreelist();
	if (NULL == conn)
	{
		conn = new CONN;
		if (NULL == conn)
		{
			LOG4CXX_ERROR(g_logger, "CWorkerThread::InitNewConn:new conn error.");
			return NULL;
		}

		try
		{
			conn->rBuf = new char[DATA_BUFFER_SIZE];
			conn->wBuf = new char[DATA_BUFFER_SIZE];
		} catch (std::bad_alloc &)
		{
			FreeConn(conn);
			LOG4CXX_ERROR(g_logger, "CWorkerThread::InitNewConn:new buf error.");
			return NULL;
		}
	}

	conn->sfd = conn_info.sfd;
    conn->id = 0;
	conn->rlen = 0;
	conn->wlen = 0;
	conn->thread = libevent_thread_ptr;

	/* 将新连接加入此线程libevent事件循环 */
	int flag = EV_READ | EV_PERSIST;
	struct bufferevent *client_tcp_event = bufferevent_socket_new(libevent_thread_ptr->base, conn->sfd, BEV_OPT_CLOSE_ON_FREE);
	if (NULL == client_tcp_event)
	{
		if(!AddConnToFreelist(conn))
		{
			FreeConn(conn);
		}
		int error_code = EVUTIL_SOCKET_ERROR();
		LOG4CXX_ERROR(g_logger,
				"CWorkerThread::conn_new:bufferevent_socket_new errorCode = " << error_code << ", description = " << evutil_socket_error_to_string(error_code));

		return NULL;
	}
	bufferevent_setcb(client_tcp_event, ClientTcpReadCb, NULL, ClientTcpErrorCb, (void*) conn);

	/* 利用客户端心跳超时机制处理半开连接 */
	struct timeval heartbeat_sec;
	heartbeat_sec.tv_sec = utils::G<CGlobalSettings>().client_heartbeat_timeout_;
	heartbeat_sec.tv_usec= 0;
	bufferevent_set_timeouts(client_tcp_event, &heartbeat_sec, NULL);

	bufferevent_enable(client_tcp_event, flag);

	return conn;
}


void CWorkerThread::ClientTcpReadCb(struct bufferevent *bev, void *arg)
{
    CONN* conn = static_cast<CONN*>(arg);
    assert(conn != NULL);

    int recv_size = 0;
    recv_size = bufferevent_read(bev, conn->rBuf + conn->rlen, DATA_BUFFER_SIZE - conn->rlen);
    conn->rlen = conn->rlen + recv_size;

    std::string str_recv(conn->rBuf);	//将数据转换成string,效率降低.
    std::cout << "Get the data: " << str_recv<<endl;

    int flag_pos = 0;
    while(DATA_BUFFER_SIZE - flag_pos > 6)
    {
//------------------------Parse data-------------------------------
        printf("start to parse data...\n");
        flag_pos = str_recv.find("||", flag_pos);
        if(flag_pos == string::npos)	// data is wrong, clear it and break.
        {
//            conn->rlen = 0;
//            char wrong_msg[25] = "||0000050000000000000000";
//            if(!SocketOperate::WriteSfd(conn->sfd, wrong_msg, sizeof(wrong_msg)) )
//            {
//                LOG4CXX_ERROR(g_logger, "CWorkerThread::ClientTcpReadCb:send sfd .error = " << strerror(errno));
//            }
            bzero(conn->rBuf, sizeof(conn->rBuf));
            conn->rlen = 0;
            break;
        }
        flag_pos += 2;	//jump to the len of data.

        int data_len = utils::GetNumFromStr(str_recv, flag_pos, 4);
        if(data_len + 24 > (DATA_BUFFER_SIZE - flag_pos + 2))
        {	//...move to head and break;
            memmove(conn->rBuf, conn->rBuf + flag_pos - 2, DATA_BUFFER_SIZE - flag_pos + 2);
            conn->rlen = DATA_BUFFER_SIZE - flag_pos + 2;
            break;
        }
        flag_pos += 4;

        int data_type = utils::GetNumFromStr(str_recv, flag_pos, 2);
        flag_pos += 2;

        int send_id = utils::GetNumFromStr(str_recv, flag_pos, 8);
        flag_pos += 8;

        int recv_id = utils::GetNumFromStr(str_recv, flag_pos, 8);
        flag_pos += 8;

        string content(str_recv, flag_pos);
        flag_pos += data_len;

//--------------------------Handle data-----------------------
        switch (data_type)
        {
            case 1:		// login.done.
                printf("login...........\n");
                conn->id = send_id;
                UsingConn(conn);
                break;

                case 2:		// logout.done.
                UnusingConn(conn);
                CloseConn(conn, bev);
                break;

            case 3:		// normal msg.
            {
                printf("normal msg...........\n");
                CONN* co = FindThreadFromId(recv_id);
                if (NULL == co)
                {
                    LOG4CXX_WARN(g_logger, "CWorkerThread::ClientTcpReadCb: Can't find the thread of the id" << recv_id);
                }
                else if (co->thread->thread_id == pthread_self())
                {	// write msg!
                    string send_msg = "||" + utils::TransToByteStr(content.size(), 4) + string("03") + utils::TransToByteStr(send_id, 8) + utils::TransToByteStr(recv_id, 8) + content;
                    if(!SocketOperate::WriteSfd(co->sfd, send_msg.c_str(), send_msg.size()))
                    {
                        LOG4CXX_ERROR(g_logger, "CWorkerThread::ClientTcpReadCb:send sfd .error = " << strerror(errno));
                    }
                }
                else
                {	// trans to others!
                    TRANS_MSG tm;
                    tm.send_fd = co->sfd;
                    cout << "Before trans data: "<<content << endl;
                    tm.content = content;
                    tm.send_id = send_id;
                    tm.recv_id = recv_id;
                    TransMsg(co, tm);
                }
                break;
            }
            default:	// wrong type!
                LOG4CXX_ERROR(g_logger, "CWorkerThread::ClientTcpReadCb:data_type wrong : " << data_type);
        }
    }
}

void CWorkerThread::ClientTcpErrorCb(struct bufferevent *bev, short event, void *arg)
{
	CONN* conn = static_cast<CONN*>(arg);

	if (event & BEV_EVENT_TIMEOUT)
	{
		LOG4CXX_WARN(g_logger, "CWorkerThread::ClientTcpErrorCb:TimeOut.");
	}
	else if (event & BEV_EVENT_EOF)
	{
	}
	else if (event & BEV_EVENT_ERROR)
	{
		int error_code = EVUTIL_SOCKET_ERROR();
		LOG4CXX_WARN(g_logger,
				"CWorkerThread::ClientTcpErrorCb:some other errorCode = " << error_code << ", description = " << evutil_socket_error_to_string(error_code));
	}

	CloseConn(conn, bev);
}

void CWorkerThread::DispatchSfdToWorker(int sfd)
{
	/* Round Robin*/
	int tid = (last_thread_ + 1) % utils::G<CGlobalSettings>().thread_num_;
	LIBEVENT_THREAD *libevent_thread_ptr = vec_libevent_thread_.at(tid);
	last_thread_ = tid;

	/* 将新连接的加入此worker线程连接队列 */
	CONN_INFO connInfo;
	connInfo.sfd = sfd;
	libevent_thread_ptr->list_conn.push_back(connInfo);

	/* 通知此worker线程有新连接到来，可以读取了 */
	char buf[1];
	buf[0] = 'c';
	if (write(libevent_thread_ptr->notify_send_fd, buf, 1) != 1)
	{
		LOG4CXX_WARN(g_logger, "CWorkerThread::DispatchSfdToWorker:Writing to thread notify pipe");
	}
}

void CWorkerThread::RegisterThreadInitialized(void)
{
    pthread_mutex_lock(&init_lock_);
    init_count_++;
    if(init_count_ == int(utils::G<CGlobalSettings>().thread_num_))
    {
    	pthread_cond_signal(&init_cond_);
    }
    pthread_mutex_unlock(&init_lock_);
}

void CWorkerThread::WaitForThreadRegistration(int nthreads)
{
	pthread_mutex_lock(&init_lock_);
    pthread_cond_wait(&init_cond_, &init_lock_);
    pthread_mutex_unlock(&init_lock_);
}

void CWorkerThread::InitFreeConns()
{
	freetotal_ 	= 200;
	freecurr_	= 0;

	vec_freeconn_.resize(freetotal_);
}

CONN* CWorkerThread::GetConnFromFreelist()
{
	CONN *conn = NULL;

	boost::mutex::scoped_lock Lock(mutex_);
	if(freecurr_ > 0)
	{
		conn = vec_freeconn_.at(--freecurr_);
	}

	return conn;
}

bool CWorkerThread::AddConnToFreelist(CONN* conn)
{
	bool ret = false;
	boost::mutex::scoped_lock Lock(mutex_);
	if (freecurr_ < freetotal_)
	{
		vec_freeconn_.at(freecurr_++) = conn;
		ret = true;
	}
	else
	{
		/* 增大连接内存池队列 */
		size_t newsize = freetotal_ * 2;
		vec_freeconn_.resize(newsize);
		freetotal_ = newsize;
		vec_freeconn_.at(freecurr_++) = conn;
		ret = true;
	}

	return ret;
}

void CWorkerThread::FreeConn(CONN* conn)
{
	if (conn)
	{
		utils::SafeDeleteArray(conn->rBuf);
		utils::SafeDeleteArray(conn->wBuf);
		utils::SafeDelete (conn);
	}
}

void CWorkerThread::CloseConn(CONN* conn, struct bufferevent* bev)
{
	assert(conn != NULL);

	/* 清理资源：the event, the socket and the conn */
	bufferevent_free(bev);

	LOG4CXX_TRACE(g_logger, "CWorkerThread::conn_close sfd = " << conn->sfd);

	/* if the connection has big buffers, just free it */
	if (!AddConnToFreelist (conn))
	{
		FreeConn(conn);
	}

	return;
}

CONN* CWorkerThread::FindThreadFromId(int id)
{
    boost::mutex::scoped_lock Lock(mutex_);
    for(auto t_it : vec_using_conn_)
    {
        if(t_it->id == id)
            return t_it;
    }
    return NULL;
}

void CWorkerThread::TransMsg(CONN* conn, TRANS_MSG tm)
{
    conn->thread->list_trans.push_back(tm);

    char buf[1];
    buf[0] = 't';
    if(write(conn->thread->notify_send_fd, buf, 1) != 1)
    {
        LOG4CXX_WARN(g_logger, "CWorkerThread::TransMsg:Writing to thread notify pipe");
    }
}

void CWorkerThread::UsingConn(CONN *conn)
{
    boost::mutex::scoped_lock Lock(mutex_);
    vec_using_conn_.push_back(conn);
}

void CWorkerThread::UnusingConn(CONN *conn)
{
    boost::mutex::scoped_lock Lock(mutex_);
    auto pos = std::find(vec_using_conn_.begin(), vec_using_conn_.end(), conn);
    vec_using_conn_.erase(pos);
}
