

#include <iostream>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <csignal>
#include "proxy_server.h"

proxy_server::proxy_server(int port) : main_socket(::socket(AF_INET, SOCK_STREAM, 0)),
                                       rslvr(sysconf(_SC_NPROCESSORS_ONLN)) {
    std::cerr<<"Proxy started at port: "<<port<<"\n";
    //std::cerr << epoll.get_fd() << '\n';
    int one = 1;
    setsockopt(main_socket.get_fd().get_fd(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));
    main_socket.bind(AF_INET, port, htons(INADDR_ANY));
    main_socket.listen();
    main_socket.get_fd().make_nonblocking();
    std::cerr << "Listening started\n";
    main_socket.set_flags(main_socket.get_flags() | EPOLLIN);
    int fd[2];
    pipe(fd);
    pipe_fd = file_descriptor(fd[0]);
    file_descriptor resolver_fd(fd[1]);

    pipe_fd.make_nonblocking();
    resolver_fd.make_nonblocking();
    rslvr.set_fd(std::move(resolver_fd));
    // std::cerr << "Resolver fds: " << rslvr.get_fd().get_fd() << " " << pipe_fd.get_fd() << "\n";
    resolver_event = std::unique_ptr<io_event>(new io_event(epoll, pipe_fd, EPOLLIN,
                                                [this](uint32_t events)mutable throw(std::runtime_error) {
                                                    std::cerr << "Resolver handler\n";
                                                    this->resolver_handler();
                                                }));
    listen_event = std::unique_ptr<io_event>(new io_event(epoll, main_socket.get_fd(), EPOLLIN,
                                                          [this](uint32_t events)mutable throw(std::runtime_error) {
                                                              connect_client();
                                                          }));
    std::cerr << "Main listener added to epoll!\n";
}

proxy_server::~proxy_server() {
    std::cerr << "Server stopped.\n";
    rslvr.stop();
}

void proxy_server::run() {
    std::cerr << "Server started\n";
    epoll.run();
}


void proxy_server::connect_client() {
    auto client_fd = this->main_socket.accept();
    std::cerr << "Client socket assigned to: "<<client_fd<<"\n";
    clients[client_fd] = std::unique_ptr<client>(new client(client_fd, *this));
    std::cerr << clients.size() << " clients now\n";
}


epoll_io &proxy_server::get_epoll() {
    return epoll;
}

void proxy_server::resolver_handler() {
    char tmp;
    if (read(pipe_fd.get_fd(), &tmp, sizeof(tmp)) == -1) {
        perror("Reading from resolver failed");
    }

    std::unique_ptr<http_request> cur_request = rslvr.get_task();
    std::cerr << "Resolver callback called for host "<<cur_request->get_host()<<"\n";

    std::cerr<<"Finding client "<<cur_request->get_client_fd()<<"\n";
    client *cur_client = clients[cur_request->get_client_fd()].get();

    if (cur_client == nullptr) {
        //throw_server_error("No client");
        return;
    }

    server *srvr;

    try {
        struct sockaddr result = cur_request->get_resolved_host();
        srvr = new server(result, *this, *cur_client);
    } catch (...) {
        throw_server_error("Error while connecting to server!");
    }

    cur_client->time.reset();

    srvr->set_host(cur_request->get_host());
    cur_client->bind(*srvr);
    //std::cerr << "Server with fd = %d binded to client with fd = %d\n", srvr->get_fd().get_fd(),
          //  cur_client->get_fd().get_fd());
    srvr->add_flag(EPOLLOUT);
    servers[srvr->get_fd().get_fd()] = srvr;
    std::cerr << servers.size() << " servers now\n";
    cur_client->get_buffer() = std::move(cur_request->get_data());
    cur_client->flush_client_buffer();
}

void proxy_server::erase_server(int fd) {
    //std::cerr << ("Erasing server, fd = %lu, host = [%s]\n", fd, servers[fd]->get_host().c_str());
    servers.erase(fd);
    std::cerr << servers.size() << " servers left\n";
}


void proxy_server::add_task(std::unique_ptr<http_request> request) {
    rslvr.add_task(std::move(request));
}

void proxy_server::erase_client(int fd) {
    //std::cerr << "Erasing client " << "\n";
    clients.erase(fd);
    std::cerr << clients.size() << " clients left\n";
}


lru_cache<std::string, http_response> &proxy_server::get_cache() {
    return cache;
}
