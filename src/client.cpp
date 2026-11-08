// Test client: sends commands to the server one at a time and waits for each
// reply, so the latency it reports is a full request/response round trip.
//
//   client [--host H] [--port P] [--file F | --random N [--seed S]] [--quiet]
//
// With neither --file nor --random it reads commands from stdin.

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "orderbook/workload.hpp"

using namespace std;
using namespace ob;

namespace {

int connect_to(const string& host, const string& port) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (addrinfo* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

bool send_all(int fd, const string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

class LineReader {
public:
    explicit LineReader(int fd) : fd_(fd) {}
    bool read_line(string& out) {
        for (;;) {
            const size_t nl = buf_.find('\n');
            if (nl != string::npos) {
                out = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                return true;
            }
            char tmp[4096];
            const ssize_t n = recv(fd_, tmp, sizeof tmp, 0);
            if (n <= 0) return false;
            buf_.append(tmp, static_cast<size_t>(n));
        }
    }
private:
    int fd_;
    string buf_;
};

bool is_terminator(const string& line) {
    return line == "OK" || line.rfind("ERR", 0) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    string host = "127.0.0.1", port = "12000", file;
    long random_n = 0;
    unsigned long seed = 12345;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const string a = argv[i];
        auto next = [&]() -> string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--host") host = next();
        else if (a == "--port") port = next();
        else if (a == "--file") file = next();
        else if (a == "--random") random_n = atol(next().c_str());
        else if (a == "--seed") seed = strtoul(next().c_str(), nullptr, 10);
        else if (a == "--quiet") quiet = true;
        else { cerr << "unknown option: " << a << "\n"; return EXIT_FAILURE; }
    }

    // Build the command list up front so generation/IO is not in the timings.
    vector<string> lines;
    if (random_n > 0) {
        Workload w(seed);
        for (long i = 0; i < random_n; ++i) lines.push_back(format_command(w.next()));
    } else {
        ifstream f;
        if (!file.empty()) {
            f.open(file);
            if (!f) { cerr << "cannot open " << file << "\n"; return EXIT_FAILURE; }
        }
        istream& in = file.empty() ? cin : f;
        string l;
        while (getline(in, l)) lines.push_back(l);
    }

    const int fd = connect_to(host, port);
    if (fd < 0) { cerr << "cannot connect to " << host << ":" << port << "\n"; return EXIT_FAILURE; }
    LineReader reader(fd);

    using clock = chrono::steady_clock;
    vector<double> lat_us;
    lat_us.reserve(lines.size());
    const auto t0 = clock::now();

    for (const string& l : lines) {
        const auto s = clock::now();
        if (!send_all(fd, l + "\n")) { cerr << "send failed\n"; return EXIT_FAILURE; }
        string reply;
        for (;;) {
            if (!reader.read_line(reply)) { cerr << "server closed connection\n"; return EXIT_FAILURE; }
            if (!quiet) cout << reply << "\n";
            if (is_terminator(reply)) break;
        }
        lat_us.push_back(chrono::duration<double, micro>(clock::now() - s).count());
    }

    const double secs = chrono::duration<double>(clock::now() - t0).count();
    close(fd);

    if (!lat_us.empty()) {
        sort(lat_us.begin(), lat_us.end());
        auto pct = [&](double p) { return lat_us[static_cast<size_t>(p * (lat_us.size() - 1))]; };
        cerr << "requests=" << lat_us.size() << " time=" << secs << "s"
             << " rate=" << static_cast<long>(lat_us.size() / secs) << "/s"
             << " rtt_us p50=" << pct(0.50) << " p99=" << pct(0.99) << " max=" << lat_us.back() << "\n";
    }
    return EXIT_SUCCESS;
}
