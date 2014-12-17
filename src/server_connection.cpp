#include "x/codec/http/http_decoder.hpp"
#include "x/codec/http/http_encoder.hpp"
#include "x/message/http/http_response.hpp"
#include "x/net/server_connection.hpp"

namespace x {
namespace net {

enum {
    // all values are measured by second
    IDLE_WAITING_TIME = 15
};

server_connection::server_connection(context_ptr ctx)
    : connection(ctx),
      resolver_(ctx->service()) {
    decoder_.reset(new codec::http::http_decoder(HTTP_RESPONSE));
    encoder_.reset(new codec::http::http_encoder(HTTP_REQUEST));
    message_.reset(new message::http::http_response);
    XDEBUG_WITH_ID(this) << "new server connection";
}

void server_connection::start() {
    assert(host_.length() > 0);
    assert(port_ != 0);

    connect();
}

void server_connection::connect() {
    XDEBUG_WITH_ID(this) << "=> connect()";

    using namespace boost::asio::ip;
    auto callback = std::bind(&server_connection::on_resolve,
                              std::dynamic_pointer_cast<server_connection>(shared_from_this()),
                              std::placeholders::_1,
                              std::placeholders::_2);

    resolver_.async_resolve(tcp::resolver::query(host_, std::to_string(port_)), callback);

    XDEBUG_WITH_ID(this) << "<= connect()";
}

void server_connection::handshake(ssl::certificate ca, DH *dh) {
    XDEBUG_WITH_ID(this) << "=> handshake()";

    auto callback = std::bind(&connection::on_handshake,
                              shared_from_this(),
                              std::placeholders::_1);

    socket_->switch_to_ssl(boost::asio::ssl::stream_base::server, ca, dh);
    socket_->async_handshake(callback);

    XDEBUG_WITH_ID(this) << "<= handshake()";
}

void server_connection::reset() {
    connection::reset();
    // do not reset context here, it will reset itself
    // context_->reset();
    decoder_->reset();
    encoder_->reset();
    message_->reset();

    auto self(shared_from_this());
    timer_.start(IDLE_WAITING_TIME, [self, this] (const boost::system::error_code&) {
        XERROR_WITH_ID(this) << "idle waiting timed out.";
        stop();
    });
}

void server_connection::on_connect(const boost::system::error_code& e, boost::asio::ip::tcp::resolver::iterator) {
    if (stopped_) {
        XERROR_WITH_ID(this) << "connection stopped.";
        return;
    }

    CHECK_LOG_EXEC_RETURN(e, "connect", stop);

    connected_ = true;

    context_->on_event(CONNECT, *this);
}

void server_connection::on_read(const boost::system::error_code& e, const char *data, std::size_t length) {
    if (stopped_) {
        XERROR_WITH_ID(this) << "connection stopped.";
        return;
    }

    if (e) {
        if (e == boost::asio::error::eof || SSL_SHORT_READ(e)) {
            XDEBUG_WITH_ID(this) << "read, EOF in socket.";
            connected_ = false;
        } else {
            XERROR_WITH_ID(this) << "read error, code: " << e.value()
                                 << ", message: " << e.message();
            stop();
            return;
        }
    }

    if (length <= 0) {
        XERROR_WITH_ID(this) << "read, no data.";
        stop();
        return;
    }

    if (message_->completed()) {
        XERROR_WITH_ID(this) << "message already completed.";
        stop();
        return;
    }

    if (timer_.running())
        timer_.cancel();

    auto consumed = decoder_->decode(data, length, *message_);
    ASSERT_EXEC_RETNONE(consumed == length, stop);

    if (message_->deliverable())
        context_->on_event(READ, *this);

    if (!message_->completed()) {
        read();
    } else {
#warning add message exchange completed logic here
    }
}

void server_connection::on_write() {
    if (stopped_) {
        XERROR_WITH_ID(this) << "connection stopped.";
        return;
    }

    read();
}

void server_connection::on_handshake(const boost::system::error_code& e) {
    if (stopped_) {
        XERROR_WITH_ID(this) << "connection stopped.";
        return;
    }

    CHECK_LOG_EXEC_RETURN(e, "handshake", stop);

    context_->on_event(HANDSHAKE, *this);
}

void server_connection::on_resolve(const boost::system::error_code& e, boost::asio::ip::tcp::resolver::iterator it) {
    if (stopped_) {
        XERROR_WITH_ID(this) << "connection stopped.";
        return;
    }

    CHECK_LOG_EXEC_RETURN(e, "resolve", stop);

    auto callback = std::bind(&connection::on_connect,
                              shared_from_this(),
                              std::placeholders::_1,
                              std::placeholders::_2);

    socket_->async_connect(it, callback);
}

} // namespace net
} // namespace x
