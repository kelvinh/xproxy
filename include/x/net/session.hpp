#ifndef SESSION_HPP
#define SESSION_HPP

namespace x {
namespace net {

class session : public util::counter<session>,
                public std::enable_shared_from_this<session> {
public:
    enum conn_type {
        CLIENT_SIDE, SERVER_SIDE
    };

    session(const server& server)
        : service_(server.get_service()),
          config_(server.get_config()),
          session_manager_(server.get_session_manager()),
          cert_manager_(server.get_certificate_manager()),
          client_connection_(service),
          server_connection_(service) {}

    DEFAULT_DTOR(session);

    void start() {
        client_connection_->start();
    }

    void stop() {
        manager_.erase(shared_from_this());
    }

    conn_ptr get_connection(conn_type type) const {
        return type == CLIENT_SIDE ? client_connection_ : server_connection_;
    }

private:
    boost::asio::io_service& service_;
    x::conf::config& config_;
    session_manager& session_manager_;
    x::ssl::certificate_manager& cert_manager_;

    conn_ptr client_connection_;
    conn_ptr server_connection_;

    MAKE_NONCOPYABLE(session);
};

typedef std::shared_ptr<session> session_ptr;

} // namespace net
} // namespace x

#endif // SESSION_HPP