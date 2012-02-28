#include <arpa/inet.h>
#include <errno.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/thread.h>
#include <fcntl.h>
#include <iostream>
#include <program_options.hpp>
#include <signal.h>
#include <stdio.h>
#include <string>
#include "badbaseexception.hpp"
#include "eventbase.hpp"
#include "network.hpp"
#include "tpool.h"
namespace po = boost::program_options;
using namespace dm;

#define DFLT_THREADS    16
#define DFLT_QUEUE      4096
#define DFLT_PORT       32000
#define LISTEN_BACKLOG  65535

EventBase* initLibEvent(const char* method);
evutil_socket_t listenSock(const int port);
void runServer(EventBase* eb, const int port, const int numWorkerThreads, 
        const int maxQueueSize);
void runServerTh(const int port, const int numWorkerThreads, 
        const int maxQueueSize);

struct readArgs
{
    struct bufferevent* bev;
    tPool* pool;  
};

/**
 * A server intended to test the differences in efficiency between the various
 * event handling methods.
 * @author Dean Morin
 */
int main(int argc, char** argv)
{
    int opt = 0;
    int port = 0;
    int threads = 0;
    int queue = 0;
    EventBase* eb = NULL;
    std::string method = "";

    po::options_description desc("Allowed options");
    desc.add_options()
        ("kqueue,k", "use kqueue()")
        ("epoll,e", "use epoll()")
        ("select,s", "use select()")
        ("poll,p", "use poll()")
        ("threads,t", "use threads")
        ("port,P", po::value<int>(&opt)->default_value(DFLT_PORT),
                "port to listen on")
        ("thread-pool,T", po::value<int>(&opt)->default_value(DFLT_THREADS),
                "number of threads in the thread pool")
        ("max-queue,M", po::value<int>(&opt)->default_value(DFLT_QUEUE),
                "max number of jobs in the pool queue")
        ("help", "show this message")
    ;

    po::variables_map vm;
    try 
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } 
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        std::cerr << "\tuse --help to see program options\n";
        return 1;
    }

    port = vm["port"].as<int>();
    threads = vm["thread-pool"].as<int>();
    queue = vm["max-queue"].as<int>();
    
    if (vm.count("help"))
    {
        std::cout << desc << "\n";
    }
    else if (vm.count("kqueue"))
    {
        method = "kqueue";
    }
    else if (vm.count("epoll"))
    {
        method = "epoll";
    }

    else if (vm.count("select"))
    {
        method = "select";
    }
    else if (vm.count("poll"))
    {
        method = "poll";
    }
    else if (vm.count("threads"))
    {
        runServerTh(port, threads, queue);
    }
    else
    {
        std::cerr << "Please specify which event base to use.\n";
        std::cerr << "\tuse --help to see program options\n";
    }

    if (method.compare(""))
    {
        eb = initLibEvent(method.c_str());
        runServer(eb, port, threads, queue);
    }
    return 0;
}

/**
 * Perform the initialization required to use the libevent library.
 * @param method The desired event method to use.
 * @return The initialized event base. This is heap allocated and so the caller
 * must call delete on it later.
 * @author Dean Morin
 */
EventBase* initLibEvent(const char* method)
{
    try
    {
        EventBase* eb = new EventBase(method);
#ifdef DEBUG
        std::cerr << "Using: " << eb->getMethod() << "\n";
#endif
        return eb;
    }
    catch (const BadBaseException& e)
    {
        int i = 0;
        const char** methods = EventBase::getAvailableMethods();

        std::cerr << "Error: " << e.what() << "\n";
        std::cerr << "\tThe available event bases are:\n";

        for (i = 0; methods[i] != NULL; i++)
        {
            std::cerr << "\t - " << methods[i] << "\n";
        }
        exit(1);
    }
    catch (...)
    {
        std::cerr << "Error: pthreads are not available on this machine\n";
        exit(1);
    }
}

void handleSigint(evutil_socket_t, short, void* arg)
{
    evconnlistener_free((struct evconnlistener*) arg);
	exit(0);
}

static void sockEvent(struct bufferevent* bev, short events, void*)
{
    if (events & BEV_EVENT_ERROR)
    {
        perror("Error from bufferevent");
    }
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) 
    {
        bufferevent_free(bev);
    }
}

void handleRequest(void* args)
{
    struct bufferevent* bev = (struct bufferevent*) args;
    struct evbuffer *input = bufferevent_get_input(bev);
    struct evbuffer *output = bufferevent_get_output(bev);
    uint32_t msgSize;
    if (evbuffer_copyout(input, &msgSize, sizeof(uint32_t)) == -1)
    {
        std::cerr << "Error: evbuffer_copyout\n";
    }
#ifdef DEBUG
    std::cerr << "msgsize: " << msgSize << "\n";
#endif
    char* buf = new char[msgSize];

    // fill the packet with random characters
    for (size_t i = 0; i < msgSize; i++)
    {
        buf[i] = rand() % 93 + 33;
    }

    evbuffer_add(output, buf, msgSize);
    delete[] buf;
}

static void readSock(struct bufferevent* bev, void* arg)
{
    if (tPoolAddJob((tPool*) arg, handleRequest, bev))
    {
        std::cerr << "Error adding new job to thread pool\n";
        exit(1);
    }
}

static void acceptErr(struct evconnlistener* listener, void*)
{
    struct event_base *base = evconnlistener_get_base(listener);
    int err = EVUTIL_SOCKET_ERROR();
    std::cerr << "Error " << err << "(" << evutil_socket_error_to_string(err)
              << ") on listening socket. Shutting down.\n";
    event_base_loopexit(base, NULL);
}

static void acceptClient(struct evconnlistener* listener, evutil_socket_t fd,
        struct sockaddr*, int, void* arg)
{
    // new connection
    struct event_base* base = evconnlistener_get_base(listener);
    struct bufferevent* bev = bufferevent_socket_new(base, fd, 
            BEV_OPT_CLOSE_ON_FREE);

    bufferevent_setcb(bev, readSock, NULL, sockEvent, arg);
    bufferevent_enable(bev, EV_READ | EV_WRITE); 
}

void runServer(EventBase* eb, const int port, const int numWorkerThreads,
        const int maxQueueSize) 
{
	struct sockaddr_in addr;
    struct evconnlistener* listener;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    tPool* pool = NULL;
    int blockWhenQueueFull = 1;

    if (tPoolInit(&pool, numWorkerThreads, maxQueueSize, blockWhenQueueFull))
    {
        std::cerr << "Error initializing thread pool\n";
        exit(1);
    }

    if (!(listener = evconnlistener_new_bind(eb->getBase(), acceptClient, pool, 
            LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, LISTEN_BACKLOG, 
            (struct sockaddr*) &addr, sizeof(addr))))
    {
        perror("evconnlistener_new_bind()");
        exit(1);
    }
    evconnlistener_set_error_cb(listener, acceptErr);

    struct event* sigint;
    sigint = evsignal_new(eb->getBase(), SIGINT, handleSigint, listener);
    evsignal_add(sigint, NULL);

    event_base_dispatch(eb->getBase());
    event_del(sigint);
}

/**
 * Read message from fd, the return a packet of random characters.
 * @param args The socket to read from / write to.
 * @author Dean Morin
 */
void readSockTh(void* args)
{
    evutil_socket_t fd = *(evutil_socket_t*) args;
    char readBuf[REQUEST_SIZE];
    uint32_t msgSize;

    clearSocket(fd, readBuf, REQUEST_SIZE);

    std::cerr << readBuf << std::endl;
    std::cerr << readBuf[0] << readBuf[1] << readBuf[2] << readBuf[3] << std::endl;

    msgSize = (readBuf[3] << 24) & 0xFF000000
            + (readBuf[2] << 16) & 0x00FF0000
            + (readBuf[1] <<  8) & 0x0000FF00
            +  readBuf[0]        & 0x000000FF;
#ifdef DEBUG
    std::cerr << "msgsize: " << msgSize << "\n";
#endif
    char* writeBuf = new char[msgSize];

    // fill the packet with random characters
    for (size_t i = 0; i < msgSize; i++)
    {
        writeBuf[i] = rand() % 93 + 33;
    }

    send(fd, writeBuf, msgSize, 0);

    delete writeBuf;
}

/**
 * Set up a new socket so that the port it's bound to can be immediately reused 
 * after exiting the program.
 * @param fd The socket to perform these operations on.
 * @author Dean Morin
 */
void setUpSocket(evutil_socket_t fd)
{
    int arg = 1;
    // set so port can be resused imemediately after ctrl-c
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &arg, sizeof(arg)) == -1) 
    {
        perror("setsockopt()");
        exit(1);
    }
}

evutil_socket_t acceptClientTh(evutil_socket_t fd)
{
    evutil_socket_t fdNew;
	struct sockaddr_in addr;
	socklen_t addrSize = sizeof(struct sockaddr_in);

    fdNew = accept(fd, (struct sockaddr*) &addr, &addrSize);
    if (fdNew == -1) 
    {
        perror("accept");
    }
    setUpSocket(fdNew);

    return fdNew;
}

void runServerTh(const int port, const int numWorkerThreads,
        const int maxQueueSize)
{
	struct sockaddr_in addr;
    evutil_socket_t fd;
    evutil_socket_t fdNew;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    tPool* pool = NULL;
    int blockWhenQueueFull = 1;

    if (tPoolInit(&pool, numWorkerThreads, maxQueueSize, blockWhenQueueFull))
    {
        std::cerr << "Error initializing thread pool\n";
        exit(1);
    }

    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{
		perror("socket()");
		exit(1);
	}
	if (bind(fd, (struct sockaddr*) &addr, sizeof(addr)) == -1)
	{
		perror("bind()");
		exit(1);
	}
    if (listen(fd, LISTEN_BACKLOG))
    {
        perror("listen()");
        exit(1);
    }

    while (true)
    {
        fdNew = acceptClientTh(fd);
        
        if (tPoolAddJob(pool, readSockTh, &fdNew))
        {
            std::cerr << "Error adding new job to thread pool\n";
            exit(1);
        }
    }
}
