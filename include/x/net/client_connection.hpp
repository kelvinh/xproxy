#ifndef CLIENT_CONNECTION_HPP
#define CLIENT_CONNECTION_HPP

#include "x/net/connection.hpp"

namespace x {
namespace net {

class client_connection : public connection {
public:
    client_connection(context_ptr ctx, connection_manager& mgr);

    DEFAULT_DTOR(client_connection);

    virtual bool keep_alive();

    virtual void start();

    virtual void connect();

    virtual void handshake(ssl::certificate ca = ssl::certificate(), DH *dh = nullptr);

    virtual void reset();

    virtual void on_connect(const boost::system::error_code& e, boost::asio::ip::tcp::resolver::iterator it);

    virtual void on_read(const boost::system::error_code& e, const char *data, std::size_t length);

    virtual void on_write();

    virtual void on_handshake(const boost::system::error_code& e);
};

} // namespace net
} // namespace x

#endif // CLIENT_CONNECTION_HPP
