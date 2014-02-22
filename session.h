#ifndef SESSION_H
#define SESSION_H

#include <boost/asio.hpp>
#include <boost/atomic.hpp>
#include <boost/enable_shared_from_this.hpp>
#include "builtin_filters.h"
#include "filter_chain.h"
#include "http_container.h"
#include "http_request_decoder.h"
#include "http_response_decoder.h"
#include "resource_manager.h"
#include "session_context.h"
#include "session_manager.h"
#include "socket.h"

class ProxyServer;

class Session : public Resettable,
                public boost::enable_shared_from_this<Session>,
                private boost::noncopyable {
public:
    typedef Session this_type;

    enum {
        kDefaultClientInBufferSize = 8192,
        kDefaultServerInBufferSize = 8192,  // TODO decide the proper value
        kDefaultClientTimeoutValue = 60,    // 60 seconds
        kDefaultServerTimeoutValue = 15     // 15 seconds
    };

    static Session *create(ProxyServer& server) {
        ++counter_;
        return new Session(server);
    }

    boost::asio::ip::tcp::socket& ClientSocket() const {
        assert(client_socket_);
        return client_socket_->socket();
    }

    boost::asio::ip::tcp::socket& ServerSocket() const {
        assert(server_socket_);
        return server_socket_->socket();
    }

    std::size_t id() const { return id_; }

    void start();
    void stop();

    void reset();

    void AsyncReadFromClient();
    void AsyncWriteSSLReplyToClient();
    void AsyncConnectToServer();
    void AsyncWriteToServer();
    void AsyncReadFromServer();
    void AsyncWriteToClient();

    virtual ~Session();

private:
    Session(ProxyServer& server);

private:
    void OnClientDataReceived(const boost::system::error_code& e);
    void OnClientSSLReplySent(const boost::system::error_code& e);
    void OnServerConnected(const boost::system::error_code& e);
    void OnServerDataSent(const boost::system::error_code& e);
    void OnServerDataReceived(const boost::system::error_code& e);
    void OnClientDataSent(const boost::system::error_code& e);
    void OnServerTimeout(const boost::system::error_code& e);
    void OnClientTimeout(const boost::system::error_code& e);

private:
    void InitClientSSLContext();

private:
    static boost::atomic<std::size_t> counter_;

private:
    std::size_t id_;

    ProxyServer& server_;

    SessionManager& manager_;

    boost::asio::io_service& service_;
    std::unique_ptr<Socket> client_socket_;
    std::unique_ptr<Socket> server_socket_;

    boost::asio::deadline_timer client_timer_;
    boost::asio::deadline_timer server_timer_;

    boost::asio::ip::tcp::resolver resolver_;

    std::unique_ptr<FilterChain> chain_;

    std::unique_ptr<Decoder> request_decoder_;
    std::unique_ptr<Decoder> response_decoder_;

    SessionContext context_;

    bool server_connected_;
    bool finished_;
    bool reused_;

    bool client_timer_triggered_;

    boost::asio::streambuf client_in_;
    boost::asio::streambuf client_out_;
    boost::asio::streambuf server_in_;
    boost::asio::streambuf server_out_;
};

#endif // SESSION_H
