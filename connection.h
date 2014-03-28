#ifndef CONNECTION_H
#define CONNECTION_H

#include <memory>
#include "common.h"
#include "socket.h"

class HttpMessage;
class Session;

class Connection : public std::enable_shared_from_this<Connection> {
public:
    virtual void read();
    virtual void write();

    boost::asio::io_service& service() { return service_; }

    boost::asio::streambuf& OutBuffer() { return buffer_out_; }

    socket_type& socket() { return socket_->socket(); }

    // in class ServerConnection, this method should be overridden
    virtual void OnHeadersComplete() {}

    // in class ServerConnection, this method should be overridden
    virtual void OnBody() {}

    // this method should be overridden in both derived classes
    virtual void OnBodyComplete() {}

protected:
    Connection(std::shared_ptr<Session> session,
               long timeout = 30,
               std::size_t buffer_size = 8192); // TODO
    virtual ~Connection() = default;

    virtual void init() = 0;

    virtual void connect() = 0;

    virtual void OnRead(const boost::system::error_code& e) = 0;
    virtual void OnWritten(const boost::system::error_code& e) = 0;
    virtual void OnTimeout(const boost::system::error_code& e) = 0;

    virtual void ConstructMessage();

    void reset();

protected:
    std::weak_ptr<Session> session_;

    boost::asio::io_service& service_;

    boost::asio::deadline_timer timer_;
    boost::posix_time::seconds timeout_;
    bool timer_triggered_;

    std::unique_ptr<Socket> socket_;
    bool connected_;

    std::size_t buffer_size_;
    boost::asio::streambuf buffer_in_;
    boost::asio::streambuf buffer_out_;

    std::shared_ptr<HttpMessage> message_;

private:
    DISABLE_COPY_AND_ASSIGNMENT(Connection);
};

#endif // CONNECTION_H
