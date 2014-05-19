#ifndef CONNECTION_HPP
#define CONNECTION_HPP

#include <array>
#include <list>
#include <memory>
#include <set>
#include <vector>
#include <boost/asio.hpp>
#include "common.hpp"
#include "resource_manager.h"
#include "util/counter.hpp"

namespace xproxy {
namespace memory { class ByteBuffer; }
namespace message { class Message; }
namespace net {

class SocketFacade;
class Connection;
typedef std::shared_ptr<Connection> ConnectionPtr;

class ConnectionAdapter {
public:
    virtual void onConnect(const boost::system::error_code& e) = 0;
    virtual void onHandshake(const boost::system::error_code& e) = 0;
    virtual void onRead(const boost::system::error_code& e, const char *data, std::size_t length) = 0;
    virtual void onWrite(const boost::system::error_code& e) = 0;
};

struct ConnectionContext {
    ConnectionContext() : https(false), proxied(false) {}

    bool https;
    bool proxied;
    std::string remote_host;
    std::string remote_port;
};

typedef std::shared_ptr<ConnectionContext> SharedConnectionContext;

class Connection : public util::Counter<Connection>, public std::enable_shared_from_this<Connection> {
public:
    // typedef std::vector<char> buffer_type;
    typedef xproxy::memory::ByteBuffer buffer_type;

    boost::asio::io_service& service() const { return service_; }

    void closeSocket();

    virtual void start() = 0;
    virtual void stop() = 0;

    virtual void connect(const std::string& host, const std::string& port) = 0;
    virtual void handshake(ResourceManager::CertManager::CAPtr ca = nullptr,
                           ResourceManager::CertManager::DHParametersPtr dh = nullptr) = 0;
    virtual void read();
    virtual void write(const xproxy::message::Message& message);
    virtual void write(const std::string& str);

    virtual ConnectionPtr bridgeConnection() = 0;

protected:
    Connection(boost::asio::io_service& service,
               SharedConnectionContext context);
    DEFAULT_VIRTUAL_DTOR(Connection);

protected:
    virtual void doWrite();
    virtual void doConnect() = 0;

private:
    enum { kBufferSize = 8192 };

    boost::asio::io_service& service_;
    std::unique_ptr<SocketFacade> socket_;

    std::array<char, kBufferSize> buffer_in_;
    std::list<std::shared_ptr<buffer_type>> buffer_out_;

    std::unique_ptr<ConnectionAdapter> adapter_;

    SharedConnectionContext context_;
};

class ClientConnection : public Connection {
public:
    virtual void start();
    virtual void stop();
    virtual void connect(const std::string& host, const std::string& port);
    virtual void handshake(ResourceManager::CertManager::CAPtr ca, ResourceManager::CertManager::DHParametersPtr dh);
    virtual ConnectionPtr bridgeConnection();

protected:
    virtual void doConnect();
};

class ServerConnection : public Connection {
public:
    virtual void start();
    virtual void stop();
    virtual void connect(const std::string& host, const std::string& port);
    virtual void handshake(ResourceManager::CertManager::CAPtr ca, ResourceManager::CertManager::DHParametersPtr dh);
    virtual ConnectionPtr bridgeConnection();

protected:
    virtual void doConnect();
};

class ConnectionManager {
public:
    void start(ConnectionPtr& connection);
    void stop(ConnectionPtr& connection);
    void stopAll();

    DEFAULT_CTOR_AND_DTOR(ConnectionManager);

private:
    std::set<ConnectionPtr> connections_;

private:
    MAKE_NONCOPYABLE(ConnectionManager);
};

} // namespace net
} // namespace xproxy

#endif // CONNECTION_HPP
