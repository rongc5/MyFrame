#include "ws_sync_client.h"
#include "mybase64.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <string>

namespace {
struct ParsedUrl { std::string scheme, host, port, path; };
static ParsedUrl parse_url(const std::string& url) {
    ParsedUrl u; size_t p = url.find("://");
    u.scheme = (p==std::string::npos)?"":url.substr(0,p);
    size_t i = (p==std::string::npos)?0:p+3;
    size_t slash = url.find('/', i);
    std::string hostport = (slash==std::string::npos)?url.substr(i):url.substr(i, slash-i);
    size_t col = hostport.find(':');
    if (col==std::string::npos) { u.host=hostport; }
    else { u.host=hostport.substr(0,col); u.port=hostport.substr(col+1); }
    for (auto& c : u.scheme) c = (char)tolower(c);
    if (u.port.empty()) u.port = (u.scheme=="wss"?"443":"80");
    u.path = (slash==std::string::npos)?"/":url.substr(slash);
    return u;
}
static int tcp_connect(const std::string& host, const std::string& port) {
    struct addrinfo hints; memset(&hints,0,sizeof hints); hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res)!=0) return -1;
    int fd=-1; for (auto p=res;p;p=p->ai_next){ fd=::socket(p->ai_family,p->ai_socktype,p->ai_protocol); if (fd<0) continue; if (::connect(fd,p->ai_addr,p->ai_addrlen)==0) break; ::close(fd); fd=-1; }
    freeaddrinfo(res); return fd;
}
static std::string b64(const void* data, size_t n) {
    return CBase64::encode(std::string((const char*)data, n));
}
static std::string gen_sec_key() { unsigned char rnd[16]; RAND_bytes(rnd, sizeof rnd); return b64(rnd, sizeof rnd); }
static std::string calc_accept(const std::string& key) {
    static const char* GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string s = key; s += GUID; unsigned char sha[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char*)s.data(), (unsigned long)s.size(), sha);
    return b64(sha, SHA_DIGEST_LENGTH);
}
}

WsSyncClient::WsSyncClient():_fd(-1),_ssl_ctx(nullptr),_ssl(nullptr),_use_tls(false){}
WsSyncClient::~WsSyncClient(){ close(); }

bool WsSyncClient::connect(const std::string& url, const std::string& extra_headers) {
    auto u = parse_url(url); _host=u.host; _port=u.port; _path=u.path; _use_tls = (u.scheme=="wss");
    _fd = tcp_connect(_host, _port); if (_fd<0) return false;
    if (_use_tls) {
        SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms();
        _ssl_ctx = SSL_CTX_new(TLS_client_method()); if (!_ssl_ctx) { ::close(_fd); _fd=-1; return false; }
        _ssl = SSL_new((SSL_CTX*)_ssl_ctx); SSL_set_tlsext_host_name((SSL*)_ssl, _host.c_str()); SSL_set_fd((SSL*)_ssl, _fd);
        if (SSL_connect((SSL*)_ssl)!=1) { SSL_free((SSL*)_ssl); SSL_CTX_free((SSL_CTX*)_ssl_ctx); _ssl=nullptr; _ssl_ctx=nullptr; ::close(_fd); _fd=-1; return false; }
    }
    // handshake
    std::string key = gen_sec_key();
    std::string req = "GET "+_path+" HTTP/1.1\r\nHost: "+_host+"\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "+key+"\r\n";
    if (!extra_headers.empty()) req += extra_headers;
    req += "\r\n";
    if (!writen(req.c_str(), req.size())) return false;
    // read response head
    std::string resp; char buf[4096];
    while (resp.find("\r\n\r\n")==std::string::npos) {
        int r = read_some(buf, sizeof buf); if (r<=0) return false; resp.append(buf, r);
        if (resp.size() > 32*1024) return false;
    }
    size_t p = resp.find("\r\n\r\n"); std::string head = resp.substr(0,p);
    size_t eol = head.find("\r\n"); std::string status = head.substr(0, eol);
    size_t sp = status.find(' '); int code=0; if (sp!=std::string::npos){ size_t sp2=status.find(' ',sp+1); if (sp2!=std::string::npos) code=atoi(status.substr(sp+1,sp2-sp-1).c_str()); }
    if (code != 101) return false;
    // verify accept
    std::string accept;
    size_t pos = eol+2; while (pos < head.size()){
        size_t nl=head.find("\r\n", pos); std::string line=head.substr(pos, nl-pos);
        size_t col=line.find(':'); if (col!=std::string::npos){ std::string k=line.substr(0,col); std::string v=line.substr(col+1); if (!v.empty()&&v[0]==' ') v.erase(0,1); if (k=="Sec-WebSocket-Accept") accept=v; }
        if (nl==std::string::npos) break; pos=nl+2;
    }
    if (accept != calc_accept(key)) return false;
    return true;
}

bool WsSyncClient::send_text(const std::string& msg) {
    if (_fd<0) return false;
    // Build WS frame (FIN=1, opcode=1, MASK=1)
    std::string frame; frame.reserve(2 + 4 + msg.size());
    unsigned char h1 = 0x80 | 0x1; frame.push_back((char)h1);
    size_t len = msg.size(); unsigned char lenb;
    unsigned char maskbit = 0x80;
    if (len < 126) { lenb = (unsigned char)len; frame.push_back((char)(maskbit | lenb)); }
    else if (len <= 0xFFFF) { frame.push_back((char)(maskbit | 126)); unsigned char ext[2]={(unsigned char)((len>>8)&0xff),(unsigned char)(len&0xff)}; frame.append((char*)ext,2);} 
    else { frame.push_back((char)(maskbit | 127)); unsigned char ext[8]={0,0,0,0,(unsigned char)((len>>24)&0xff),(unsigned char)((len>>16)&0xff),(unsigned char)((len>>8)&0xff),(unsigned char)(len&0xff)}; frame.append((char*)ext,8);} 
    unsigned char mask[4]; RAND_bytes(mask, 4); frame.append((char*)mask,4);
    std::string payload(msg);
    for (size_t i=0;i<payload.size();++i) payload[i] = (char)(payload[i] ^ mask[i%4]);
    frame += payload;
    return writen(frame.data(), frame.size());
}

bool WsSyncClient::recv_text(std::string& out) {
    out.clear(); if (_fd<0) return false;
    unsigned char hdr[2]; if (!readn(hdr,2)) return false;
    bool fin = (hdr[0] & 0x80)!=0; unsigned char opcode = hdr[0] & 0x0f; bool masked = (hdr[1] & 0x80)!=0; uint64_t len = hdr[1] & 0x7f;
    if (len==126) { unsigned char ext[2]; if (!readn(ext,2)) return false; len = ((uint64_t)ext[0]<<8) | ext[1]; }
    else if (len==127) { unsigned char ext[8]; if (!readn(ext,8)) return false; len = ((uint64_t)ext[4]<<24)|((uint64_t)ext[5]<<16)|((uint64_t)ext[6]<<8)|ext[7]; }
    unsigned char mask[4]={0}; if (masked) { if (!readn(mask,4)) return false; }
    std::string data; data.resize((size_t)len);
    if (!readn(&data[0], (size_t)len)) return false;
    if (masked) { for (size_t i=0;i<data.size();++i) data[i] = (char)(data[i]^mask[i%4]); }
    if (opcode==0x1) { out = data; return true; }
    return false;
}

void WsSyncClient::close() {
    if (_ssl) { SSL_shutdown((SSL*)_ssl); SSL_free((SSL*)_ssl); _ssl=nullptr; }
    if (_ssl_ctx) { SSL_CTX_free((SSL_CTX*)_ssl_ctx); _ssl_ctx=nullptr; }
    if (_fd>=0) { ::close(_fd); _fd=-1; }
}

bool WsSyncClient::readn(void* buf, size_t n) {
    size_t off=0; while(off<n){ int r = read_some((char*)buf+off, n-off); if (r<=0) return false; off+= (size_t)r; } return true;
}
int WsSyncClient::read_some(void* buf, size_t n) {
    return _use_tls? SSL_read((SSL*)_ssl, buf, (int)n) : (int)::recv(_fd, buf, n, 0);
}
bool WsSyncClient::writen(const void* buf, size_t n) {
    size_t off=0; while(off<n){ int w = _use_tls? SSL_write((SSL*)_ssl, (const char*)buf+off, (int)(n-off)) : (int)::send(_fd, (const char*)buf+off, n-off, 0); if (w<=0) return false; off+= (size_t)w; } return true;
}

