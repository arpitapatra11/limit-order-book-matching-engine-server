// Single-threaded TCP matching-engine server.
//
// One poll() loop owns the OrderBook and every connection. Commands are
// applied strictly in the order their bytes are read, so arrival order at the
// server defines time priority. There is no locking because there is no second
// thread touching the book.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "orderbook/protocol.hpp"

using namespace std;
using namespace ob;

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

constexpr size_t kMaxLine = 1024;            // longest legal request line
constexpr size_t kMaxOutBuffer = 1u << 20;   // drop clients that never read replies

struct Conn {
    int    fd;
    string in;
    string out;
    bool   dead = false;
};

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void flush(Conn& c) {
    while (!c.out.empty()) {
        const ssize_t n = send(c.fd, c.out.data(), c.out.size(), 0);
        if (n > 0) { c.out.erase(0, static_cast<size_t>(n)); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        c.dead = true;
        return;
    }
}

void process_input(Conn& c, OrderBook& book, unsigned long& handled) {
    size_t start = 0, nl;
    while ((nl = c.in.find('\n', start)) != string::npos) {
        c.out += handle_line(book, string_view(c.in).substr(start, nl - start));
        ++handled;
        start = nl + 1;
    }
    c.in.erase(0, start);
    if (c.in.size() > kMaxLine) {  // no newline in sight: not a client we want
        c.out += "ERR line_too_long\n";
        flush(c);
        c.dead = true;
    }
    if (c.out.size() > kMaxOutBuffer) c.dead = true;
}

}  // namespace

int main(int argc, char** argv) {
    const int port = argc > 1 ? atoi(argv[1]) : 12000;
    if (port <= 0 || port > 65535) {
        cerr << "usage: " << argv[0] << " [port]\n";
        return EXIT_FAILURE;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;  // no SA_RESTART: poll() must wake on Ctrl-C
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    const int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return EXIT_FAILURE; }
    const int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // local only: no auth in this protocol
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) { perror("bind"); return EXIT_FAILURE; }
    if (listen(lfd, 16) < 0) { perror("listen"); return EXIT_FAILURE; }
    set_nonblocking(lfd);

    cout << "order book server listening on 127.0.0.1:" << port << endl;

    OrderBook book;
    vector<Conn> conns;
    unsigned long handled = 0;

    while (!g_stop) {
        vector<pollfd> fds;
        fds.push_back({lfd, POLLIN, 0});
        for (const Conn& c : conns) {
            fds.push_back({c.fd, static_cast<short>(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
        }

        if (poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        const size_t existing = conns.size();  // accept() below appends to conns

        if (fds[0].revents & POLLIN) {
            for (;;) {
                const int cfd = accept(lfd, nullptr, nullptr);
                if (cfd < 0) break;  // EAGAIN: backlog drained
                set_nonblocking(cfd);
                conns.push_back(Conn{cfd, {}, {}, false});
            }
        }

        for (size_t i = 0; i < existing; ++i) {
            Conn& c = conns[i];
            const short ev = fds[i + 1].revents;
            if (ev & (POLLERR | POLLNVAL)) { c.dead = true; continue; }

            if (ev & (POLLIN | POLLHUP)) {
                char buf[4096];
                const ssize_t n = recv(c.fd, buf, sizeof buf, 0);
                if (n > 0) {
                    c.in.append(buf, static_cast<size_t>(n));
                    process_input(c, book, handled);
                } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    c.dead = true;
                }
            }
            if (!c.dead) flush(c);
        }

        for (size_t i = conns.size(); i-- > 0;) {
            if (conns[i].dead) {
                close(conns[i].fd);
                conns.erase(conns.begin() + static_cast<ptrdiff_t>(i));
            }
        }
    }

    for (Conn& c : conns) close(c.fd);
    close(lfd);
    cout << "\nshutting down: " << handled << " commands handled, "
         << book.order_count() << " orders resting\n";
    return EXIT_SUCCESS;
}
