#include "x/codec/http/http_decoder.hpp"
#include "x/codec/http/http_encoder.hpp"
#include "x/message/http/http_request.hpp"
#include "x/message/http/http_response.hpp"
#include "x/net/connection.hpp"
#include "x/net/connection_manager.hpp"

namespace x {
namespace net {

connection::connection(context_ptr ctx, connection_manager& mgr)
    : connected_(false),
      stopped_(false),
      socket_(new socket_wrapper(ctx->service())),
      timer_(ctx->service()),
      context_(ctx),
      writing_(false),
      manager_(&mgr) {}

void connection::read() {
    XDEBUG_WITH_ID(this) << "=> read()";

    ASSERT_EXEC_RETNONE(connected_, stop);
    ASSERT_EXEC_RETNONE(!stopped_, stop);

    auto callback = std::bind(&connection::on_read,
                              shared_from_this(),
                              std::placeholders::_1,
                              buffer_in_.data(),
                              std::placeholders::_2);

    boost::asio::async_read(*socket_,
                            boost::asio::buffer(buffer_in_),
                            boost::asio::transfer_at_least(1),
                            callback);

    XDEBUG_WITH_ID(this) << "<= read()";
}

void connection::write() {
    XDEBUG_WITH_ID(this) << "=> write()";

    ASSERT_EXEC_RETNONE(!stopped_,stop);

    if (timer_.running())
        cancel_timer();

    do_write();

    XDEBUG_WITH_ID(this) << "<= write()";
}

void connection::write(const message::message& message) {
    XDEBUG_WITH_ID(this) << "=> write(msg)";

    ASSERT_EXEC_RETNONE(!stopped_, stop);

    if (timer_.running())
        cancel_timer();

    memory::buffer_ptr buf(new memory::byte_buffer);
    encoder_->encode(message, *buf);

    if (buf->size() > 0)
        buffer_out_.push_back(buf);

    do_write();

    XDEBUG_WITH_ID(this) << "<= write(msg)";
}

void connection::reset() {
    buffer_out_.clear();
    writing_ = false;
}

void connection::stop(bool notify) {
    if (stopped_) {
        XWARN_WITH_ID(this) << "connection already stopped.";
        return;
    }

    XDEBUG_WITH_ID(this) << "stopping connection...";

    // cancel timer
    if (timer_.running())
        cancel_timer();

    // close socket
    if (connected_ && socket_) {
        socket_->close();
        connected_ = false;
    }

    auto self(shared_from_this());
    if (notify) {
        XDEBUG_WITH_ID(this) << "notify the peer to stop.";
        auto task = [self, this] () { context_->on_stop(self); };
        context_->service().post(task);
    }

    if (manager_)
        manager_->erase(self);

    stopped_ = true;
}

void connection::do_write() {
    if (writing_) return;
    if (buffer_out_.empty()) return;
    if (!connected_) {
        start();
        return;
    }

    XDEBUG_WITH_ID(this) << "=> do_write()";

    writing_ = true;

    auto callback = std::bind(static_cast<void(connection::*)(const::boost::system::error_code&,
                                                              std::size_t)>(&connection::on_write),
                              shared_from_this(),
                              std::placeholders::_1,
                              std::placeholders::_2);

    auto& candidate = buffer_out_.front();
    if (x::log::debug_enabled()) {
        XDEBUG_WITH_ID(this) << "\n----- dump message begin -----\n"
                             << std::string(candidate->data(), candidate->size())
                             << "\n------ dump message end ------";
    }
    socket_->async_write_some(boost::asio::buffer(candidate->data(),
                                                  candidate->size()),
                              callback);

    XDEBUG_WITH_ID(this) << "<= do_write()";
}

void connection::on_write(const boost::system::error_code& e, std::size_t length) {
    writing_ = false;

    if (stopped_) {
        XERROR_WITH_ID(this) << "connection stopped.";
        return;
    }

    CHECK_LOG_EXEC_RETURN(e, "write", stop);

    auto it = buffer_out_.begin();
    if (length < (*it)->size()) {
        XERROR_WITH_ID(this) << "write incomplete, continue.";
        (*it)->erase(0, length);
        do_write();
        return;
    }

    buffer_out_.erase(it);
    if (!buffer_out_.empty()) {
        XDEBUG_WITH_ID(this) << "more buffers added, continue.";
        do_write();
        return;
    }

    on_write();
}

} // namespace net
} // namespace x
