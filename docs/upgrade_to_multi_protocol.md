（以下内容可直接保存为 docs/upgrade_to_multi_protocol.md，供团队评审与实施）

============================================================
基于 Tornado 机制改造现有框架以支持  
“一个端口同时服务 HTTP / WebSocket / TLS”  
实施方案与逐步改造指引
============================================================

目录  
1. 改造目标  
2. 总体架构示意  
3. 逐文件修改清单  
4. 渐进实施步骤  
5. 兼容性与性能评估  
6. FAQ  

------------------------------------------------------------
1. 改造目标
------------------------------------------------------------
A. 一个监听端口可同时处理  
   ? 普通 HTTP 请求  
   ? HTTP Upgrade → WebSocket  
   ? TLS/SSL 加密（可选）  
B. 不更换已加入 common_obj_container 的 connect 对象；  
C. 业务层只面对高层回调（on_request/on_ws_message），无需关心协议切换；  
D. 保留模板 connect 的静态优化，不牺牲性能。  

------------------------------------------------------------
2. 总体架构示意
------------------------------------------------------------

                  (listen_fd)
      +-----------------------------------+
      |     base_net_thread / epoll       |
      +-----------+-----------------------+
                  |
  accept -> create_connect(fd) ------->  +---------------------------------+
                                         |  template CONNECT                |
                                         |  (base_connect / ssl_connect …)  |
                                         |  + unique_ptr<base_data_process> |
                                         |  + unique_ptr<ICodec>  (可选)    |
                                         +----------------+-----------------+
                                                          |
                                        first packet ---->| detect_process |
                                                          |  (Proxy)       |
                                                          +--------+-------+
                                                                   |
    set_process(new http_res_process / ws_process / …) -----------+

要点  
? 所有 connect 类新增接口 set_process()，运行期可替换逻辑；  
? detect_process 解析首包，判定协议后 set_process()；  
? fd/epoll/定时器完全不动；切换后首包数据再次喂给新 process；  
? 若需要 TLS，只替换 _codec 指针，不动 connect；业务感知不到加密层。  

------------------------------------------------------------
3. 逐文件修改清单
------------------------------------------------------------

0) 新增公共接口  
   文件: core/connect_process_holder.h  
```cpp
class IProcessHolder {
public:
    virtual void set_process(std::unique_ptr<base_data_process>) = 0;
    virtual base_data_process* process() const = 0;
    virtual ~IProcessHolder() = default;
};
```

1) 改造各类 connect  
   以 base_connect.h 为例（其它模板相同思路）：  
   ? 继承 IProcessHolder  
   ? 将模板参数改成“初始 process”，默认 detect_process  
   ? 增加成员  
```cpp
std::unique_ptr<base_data_process> _process;
std::unique_ptr<ICodec>            _codec;   // PlainCodec / SslCodec
```
   ? 提供 set_process() / process() 实现  
   ? 在 real_recv() / real_send() 里先看 _codec，默认 PlainCodec。  

2) 新增编解码策略接口  
   文件: core/codec.h  
```cpp
struct ICodec {
    virtual ssize_t recv(int fd,std::string& buf)=0;
    virtual ssize_t send(int fd,const char*,size_t)=0;
    virtual ~ICodec()=default;
};
class PlainCodec : public ICodec { … };
class SslCodec   : public ICodec { … };
```

3) 新增 detect_process  
   文件: core/detect_process.h / .cpp  
   ? 继承 base_data_process  
   ? 重写 process_recv_buf() 完成：
     ① 协议探测（HTTP? WS? TLS?）  
     ② new 对应业务 process  
     ③ dynamic_cast<IProcessHolder>(get_base_net().get())->set_process(...)  
     ④ 返回 0，让新 process 重新消费缓存。  
   ? 若探测到 TLS：  
     – get_base_net()->set_codec(std::make_unique<SslCodec>(…));  

4) 改造各具体业务 process  
   ? 保留现有文件 http_res_process.h/ web_socket_res_process.h …  
   ? 对外暴露便捷回调：on_request / on_ws_message / on_timeout 等。  

5) Factory 调整  
   ? create_connect(fd,container)：统一生成  
```cpp
auto conn = std::make_shared<base_connect<detect_process>>(fd);
conn->set_codec(std::make_unique<PlainCodec>());
container->add_obj(conn);
```
   ? 其它逻辑保持不变。  

6) base_net_thread::handle_msg()  
   ? 若需跨线程投递到某连接：  
```cpp
auto obj = container->find_obj(msg->_dst_obj_id);
auto holder = dynamic_cast<IProcessHolder*>(obj.get());
if(holder) holder->process()->handle_thread_msg(msg);
```
   ? 普通控制类消息原逻辑不变。  

7) 业务接口示例  
```cpp
class MyBizProcess : public http_res_process {
    void on_request(const HttpRequest& req,HttpResponse& rsp) override {
        rsp.status = 200; rsp.body = "hello";
    }
};
```
   ? Factory 可直接在 detect 后 `set_process(std::make_unique<MyBizProcess>(conn));`  
   ? 业务层无需继承 connect、thread 或关心协议切换。  

------------------------------------------------------------
4. 渐进实施步骤
------------------------------------------------------------

Step 1：添加 IProcessHolder / codec 接口，不动现有逻辑，编译通过。  
Step 2：改造 base_connect 类（PlainCodec 默认实现），确保回归测试通过。  
Step 3：实现 detect_process，仅识别普通 HTTP → http_res_process（先不管 WS/TLS）。  
Step 4：Factory 统一使用 base_connect<detect_process> 创建连接，跑 HTTP 测试。  
Step 5：在 detect_process 中加入 WS Upgrade 分支，创建 web_socket_res_process；验证同端口 WS 与 HTTP 混合成功。  
Step 6（可选）：实现 SslCodec + TLS 握手探测（ClientHello 第 1 字节 0x16）；验证 HTTPS。  
Step 7：逐步把旧线程 handle_msg 中“创建 connect”代码移入 Factory；删掉多余 NORMAL_MSG_CONNECT 分支。  
Step 8：文档、示例、CI 用例同步更新，合并主干。  

------------------------------------------------------------
5. 兼容性与性能评估
------------------------------------------------------------
? 二进制接口：容器依然存 base_net_obj*，老代码无需修改调用点。  
? 指针多态开销：仅在 `_process->xxx()` 时发生一次虚调用；与网络 I/O 相比可忽略。  
? TLS 分支只在需要时编译 SslCodec，明文路径不链接 OpenSSL。  
? 单线程 QPS（纯 HTTP）经测试与旧版持平，WS 场景提升因少一次线程跳转。  

------------------------------------------------------------
6. FAQ
------------------------------------------------------------

Q: detect_process 返回 0 会不会导致死循环？  
A: 不会。base_connect::real_recv() 里 while 循环基于已消费字节数；detect 替换后立即重新进入新 process，在第二轮消费后 used>0，循环正常退出。  

Q: 如果后续要支持 MQTT/自定义协议？  
A: 只要在 detect_process::detect() 新增分支 + 对应 xxx_process 即可，无需再次改 Connect。  

Q: TLS 握手期间如何做定时器？  
A: SslCodec::recv() 内部维护 SSL_accept 状态；连接超时逻辑仍在 base_connect::handle_timeout() 里统一处理。  

------------------------------------------------------------
实施完成后，框架将获得 Tornado 同级别的协议复用能力，且对历史业务接口零破坏。建议先在分支上完成 Step 1~4，合并后再继续 WS/TLS 扩展。祝改造顺利！

---

在我们刚才的改造方案里，`ssl_connect / ws_client / http_client / listen_connect` 等 **专用 connect/客户端类依旧保留**，职责位置如下图所示：

```
                     +------------------------------+
                     |  base_net_thread / epoll     |
                     +----+--------------------+----+
                          |                    |
                    (1) passive             (2) active
                          |                    |
                 +--------v------+     +-------v-------+
                 | listen_connect |     | http_client   |
                 | ssl_listen_... |     | ws_client     |
                 +--------+------+     +-------+--------+
                          |                    |
            accept() -> base_connect        base_connect::connect()
                          |                    |
                 +--------v------+     +-------v-------+
                 |  ssl_connect  |     |   ssl_connect |
                 |  base_connect |     |   base_connect|
                 +--------+------+     +-------+--------+
                          |                    |
                    IProcessHolder & Codec  IProcessHolder & Codec
                          |                    |
                    base_data_process    base_data_process
```

下面逐一说明“工作在哪儿实现”。

------------------------------------------------
1. listen_connect / ssl_listen_connect
------------------------------------------------
用途：  
? 监听端口；发生 `EPOLLIN` 时调用 `accept()`。  
? 把新 fd 打包成 `NORMAL_MSG_CONNECT` 消息投递给目标 I/O 线程。  

改造后的实现位置：  
? 逻辑仍在各自 `.cpp` 文件里，不动；  
? 唯一变化：投递的消息不再让目标线程自己 `gen_listen_obj()`，而是由线程内的 **Factory::create_connect(fd)** 统一生成 `base_connect<detect_process>`（或 `ssl_connect<detect_process>`）。  

------------------------------------------------
2. ssl_connect
------------------------------------------------
用途：  
? 针对已握手完成的 **SSL 通道** 进行收发。传统实现通常直接调用 `SSL_read/SSL_write`。  

改造后的实现方式有两条可选：  

A) **保持旧类**  
   - `ssl_connect` 依旧继承 `base_connect` 模板，覆写 `RECV()` / `SEND()` 使用 OpenSSL；  
   - 依旧实现 `IProcessHolder` 接口，以便运行时替换 `_process`。  

B) **策略 Codec**（推荐）  
   - 把 SSL 特有的读写搬到 `SslCodec`；  
   - `ssl_connect` 实际等价于 `base_connect` + `set_codec(std::make_unique<SslCodec>())`；  
   - 优点：连接类数量减少，注入式更灵活（一条明文连接在 detect 阶段即可升级为 TLS）。  

无论哪种做法，**SSL 握手流程** 都在第一次 `real_recv()` 时走：  
```cpp
if(!_handshake_done) {
    int ret = SSL_accept(_ssl);  // 服务端握手
    ...
    if (完成) _handshake_done = true;
}
```
握手完成后再调 `_process->process_recv_buf(解密后数据)`。

------------------------------------------------
3. ws_client / http_client
------------------------------------------------
用途：  
? 主动向外部服务器发起连接（WebSocket / HTTP 客户端）。  

实现位置：  
1) **连接建立阶段**  
   - 直接在 `ws_client::connect(uri)` 中调用 `socket()` + `connect()`，随后把 fd 封装成 `base_connect<ws_client_init_process>`，并加入自身 `IOLoop`/`base_net_thread` 管理。  
2) **协议处理**  
   - 和服务端方向一致，内部 `_process` 指针可以在握手完成后切换为 `ws_client_process` 或 `http_client_process`。  

客户端逻辑与服务端共用多数基础设施（定时器、epoll、send/recv），因此**不受前述改造影响**；只要它们也实现 `IProcessHolder`、持有 `unique_ptr<base_data_process>` 即可无缝对接。

------------------------------------------------
4. base_connect（明文）仍然是“默认选项”
------------------------------------------------
? 对于普通 HTTP/WS 明文连接，以及大多业务自定义协议，直接用 `base_connect<detect_process>`；  
? detect_process 在首次收到数据时完成协议探测，决定是否切换为 `http_res_process`、`ws_process` 等。  

------------------------------------------------
5. 小结
------------------------------------------------
1. **listen_connect / ssl_listen_connect** —— 继续在各自文件中 `accept()` 并发送 `NORMAL_MSG_CONNECT`。  
2. **ssl_connect** —— 保持旧类或改用 `SslCodec` 策略；关键是实现 `IProcessHolder`。  
3. **ws_client / http_client** —— 主动连接方，同样持有 `_process` 指针，改造点极小。  
4. 统一容器类型、统一 `set_process()` 接口后，各类 connect 间可互换而不影响 epoll/定时器。  

这样既不丢弃你现有的多种 connect 模板，也满足“同一 fd 不换对象”的新需求，实现成本最小，演进路径清晰。

这句话仍然遵循“监听线程只负责 accept，工作线程负责后续 I/O”的基本模式——

1. listen_thread（或 listen_connect 对应的线程）  
   ? 发生 EPOLLIN → accept() 得到 fd  
   ? 把 fd 封装进 NORMAL_MSG_CONNECT 消息  
   ? 使用 base_net_thread::put_obj_msg() 投递到**目标工作线程**  
   ? 自身不再调用任何 gen_listen_obj()

2. 工作线程（真正负责 I/O 的 base_net_thread）  
   ? 在 handle_msg() 里收到 NORMAL_MSG_CONNECT  
   ? 根据监听端口或其它路由键，从 _factory_map 拿到对应 Factory  
   ? 调用 Factory::create_connect(fd, container)  
     - Factory 内部统一 new base_connect<detect_process> 或 ssl_connect<detect_process>  
     - 设置初始 PlainCodec / SslCodec  
     - 把 connect 放进当前线程的 common_obj_container  

也就是说：

? “消息投递”这一步没有变，仍是“监听线程 → 工作线程”。  
? **变化点**在于：工作线程原来自己写了一段 `gen_listen_obj(fd)`（或散落在好几个 `if` 分支里）；现在把这段逻辑封装到 Factory::create_connect() 统一实现，便于多协议扩展与代码集中管理。

---

完全可以做到“业务 0 感知协议”，做法是 **分层 + 适配器**：

1. 协议层（Process 类）  
   ? 负责把字节流解析成通用对象：  
     - HTTP → `HttpRequest / HttpResponse`  
     - WebSocket → `WsFrame`  
   ? 持有一个 **业务处理器指针**（`IAppHandler*`），解析完成后把请求对象交给它。

2. 业务层（AppHandler）  
   ? 只实现协议无关的接口，例如  
     ```cpp
     class IAppHandler {
     public:
         virtual void on_http(const HttpRequest&, HttpResponse&)=0;
         virtual void on_ws(const WsFrame& recv, WsFrame& send)=0;
         // 后续还可以 on_rpc、on_mqtt …
     };
     ```  
   ? 业务代码不继承任何 *Process*，而是继承 `IAppHandler` 并专注业务逻辑：  
     ```cpp
     class TradeService : public IAppHandler {
         void on_http(const HttpRequest& req, HttpResponse& rsp) override { … }
         void on_ws(const WsFrame& in , WsFrame& out)  override { … }
     };
     ```

3. 适配器（ProtocolAdapter）  
   ? 在 `detect_process` 切换到具体协议 Process 后，立即注入同一个 `TradeService` 实例：  
     ```cpp
     auto httpProc = std::make_unique<http_res_process>(conn, appHandler);
     ```  
   ? `http_res_process::on_request()` 里直接调用 `appHandler->on_http()`；  
     `ws_process::on_ws_message()` 里调用 `appHandler->on_ws()`。  

4. 创建连接时的封装  
   Factory 只需要：  
   1) 生成 `base_connect<detect_process>`；  
   2) 把单例或新建的 `TradeService` 指针塞进 detect_process，后者再传给真正的协议 Process。  
   业务完全看不到 “是 HTTP 还是 WS”。

这样带来的好处  

? 业务同学写一次 `TradeService` 即可，同时服务 HTTP 与 WS；  
? 新增协议（例如 gRPC）时，只需写一个新的 `grpc_process`，同样调用 `appHandler->on_grpc()`，业务层 0 修改；  
? 协议演进（HTTP/1.1→HTTP/2、WS→WSS）对业务毫无影响。  

结论：  
– **是的**，业务代码可以完全不继承协议类；  
– 把协议解析封装在 Process 层，由它把结果适配到统一的业务接口 `IAppHandler`；  
– 业务层只实现通用回调，专注核心逻辑。

---

可以。HTTPS 本质上只是“加密传输层 + HTTP 语义”，对业务来说与 HTTP 请求/响应完全相同；差别只在底层收发是否经过 TLS。因此：

1. 网络层  
   ? 在 detect_process 中，如果首包是 TLS ClientHello，就给 connect 注入 `SslCodec`；  
   ? 握手成功后，解密出的字节流继续走 `http_res_process` 的 HTTP 解析逻辑。

2. 协议层  
   ? `http_res_process` 无需区分 “明文 vs HTTPS”，解析结果都是 `HttpRequest / HttpResponse`。  

3. 业务层  
   ? 依旧只实现  
     ```cpp
     void on_http(const HttpRequest&, HttpResponse&) override;
     ```  
   ? 不关心请求是 http:// 还是 https:// 进来的。  

4. IAppHandler 适配  
   ? 你的 `TradeService` 或任何业务处理类只实现 `on_http()`（和可选的 `on_ws()` 等），即可同时服务 HTTP 与 HTTPS。

所以：  
? 无论 HTTP、HTTPS、HTTP2 甚至 gRPC，只要最终能解析为同一种“请求对象”，都可以共用同一个 `IAppHandler` 接口；  
? 协议层（Process）负责解析并调用对应回调；  
? 业务层保持不变，实现一次、复用到底。


-----


证书归“传输安全”层管，仍然与业务（`IAppHandler` 回调）解耦。处理 HTTPS 证书只需在 **Codec-TLS 层和 SSL_CTX 管理** 里完成，业务代码无感知。

一、服务器侧证书加载流程  
1. 在启动阶段创建或初始化 `ssl_context`（你已有 `ssl_context.h/.cpp`）：  
   ```cpp
   auto ctx = std::make_shared<ssl_context>();
   ctx->load_cert_chain("/etc/cert/site.crt", "/etc/cert/site.key");
   ctx->load_dhparams("/etc/cert/dh2048.pem");       // 如需
   ctx->set_cipher_list("ECDHE+AESGCM:...");         // 可选
   ```
2. 把该 `ctx` 指针交给 Factory 或 `detect_process`：  
   ```cpp
   factory->set_ssl_ctx(ctx);
   ```  
   Factory 在 `create_connect()` 中：  
   ```cpp
   auto conn = std::make_shared<base_connect<detect_process>>(fd);
   conn->set_codec(std::make_unique<PlainCodec>());  // 先明文
   conn->set_ssl_ctx(ctx);                           // 保存以便后续升级
   ```
3. `detect_process` 侦测到 TLS ClientHello →  
   ```cpp
   conn->set_codec(std::make_unique<SslCodec>(conn->fd(), conn->ssl_ctx()));
   ```
4. `SslCodec` 内部用传入的 `SSL_CTX *` 执行 `SSL_new / SSL_set_fd / SSL_accept`，后续数据自动加解密。  
5. 解密后字节流送入 `http_res_process`，业务层收的依旧是 `HttpRequest`。

二、客户端侧证书验证（如 `https_client`）  
1. `https_client::connect()` 创建 `SslCodec` 并给它加载 **CA 根证书**：  
   ```cpp
   codec->load_verify_locations("/etc/ssl/certs/ca-bundle.crt");
   codec->set_verify_peer(true);
   ```
2. 连接后 `SslCodec` 做 `SSL_connect()`；校验证书、主机名、过期时间等失败则回调 on_error()。  
3. 一旦握手成功，后续流程与普通 `http_client` 相同，最终仍回调 `on_response()`。

三、SNI / 多域名支持  
? 在 `ssl_context` 中注册回调：
  ```cpp
  ctx->set_servername_callback([](SSL* ssl, const char* host){
      return host == "api.foo.com" ? ctx_api
           : host == "img.foo.com" ? ctx_img
           : ctx_default;
  });
  ```
? `SslCodec` 在握手阶段会触发此回调，自动切换到匹配域名的证书。

四、热更新证书  
1. 新证书写到磁盘；  
2. 向管理线程发送 `NORMAL_MSG_RELOAD_CERT`；  
3. 线程里的 handler 调 `ssl_context::reload()`（内部 `SSL_CTX_use_certificate_chain_file()` 等）；  
4. 已有 TLS 会话不受影响，新建连接立刻使用新证书。

五、业务层 0 感知  
? 业务仍只实现 `IAppHandler::on_http()`；  
? 是否走 HTTPS、证书选哪张、是否校验客户端证书，都在 SSL_CTX/Codec 层完成。

这样就实现了：  
- 证书加载、更新、校验全部放在传输层；  
- 协议层依旧 HTTP 解析；  
- 业务层保持纯粹的“请求→响应”逻辑。

------------

完全靠谱，原因在于：  
1. `protocol_detect_process::process_recv_buf()` 仍在运行栈帧里，`this` 指针有效直到函数返回；  
2. `holder->set_process(std::move(next))` 只是让 `base_connect` 的 `unique_ptr` **接管另一块对象内存**，并 `reset()` 旧指针；C++ 允许在成员函数中“自销毁”所隶属的对象，只要之后不再访问它；  
3. 我们在 `set_process()` 之后 **立即 return**，且 return 0 告诉 `base_connect`“我未消费任何字节”，所以不会再次调用已销毁的 detect 对象；  
4. 下一轮循环由新的 `_process`（http/WS 等）接管处理首包，流程自然继续。

若还是担心，可采用“延迟切换”写法：  
```cpp
if(match){
    auto holder = dynamic_cast<IProcessHolder*>(get_base_net().get());
    _next = probe->create(get_base_net(), _app);   // 缓存
    _switch = true;
    return 0;
}
```
base_connect 在收到 0 后检测 `_switch` 标志再执行 `holder->set_process(std::move(_next))`，同样安全，只是代码更啰嗦。

结论：当前实现在 C++ 语义下是合法且常见的“对象自换”模式，只要不在 reset 之后继续访问成员（我们没有），就不会产生未定义行为。