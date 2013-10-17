#include <boost/algorithm/string.hpp>
#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>
#include "http_proxy_session.h"
#include "https_direct_handler.h"
#include "log.h"

HttpsDirectHandler::HttpsDirectHandler(HttpProxySession &session,
                                       HttpRequestPtr request)
    : session_(session), local_ssl_context_(session.LocalSSLContext()),
      local_ssl_socket_(session.LocalSocket(), local_ssl_context_),
      remote_ssl_context_(boost::asio::ssl::context::sslv23),
      remote_socket_(session.service(), remote_ssl_context_),
      resolver_(session.service()), request_(request) {
    TRACE_THIS_PTR;
    remote_socket_.set_verify_mode(boost::asio::ssl::verify_peer);
    remote_socket_.set_verify_callback(boost::bind(&HttpsDirectHandler::VerifyCertificate, this, _1, _2));
}

HttpsDirectHandler::~HttpsDirectHandler() {
    TRACE_THIS_PTR;
}

void HttpsDirectHandler::HandleRequest() {
    XTRACE << "Received a HTTPS request, host: " << request_->host()
           << ", port: " << request_->port();
    ResolveRemote();
}

void HttpsDirectHandler::ResolveRemote() {
    const std::string& host = request_->host();
    short port = request_->port();
    port = 443;

    XDEBUG << "Resolving remote address, host: " << host << ", port: " << port;

    // TODO cache the DNS query result here
    boost::asio::ip::tcp::resolver::query query(host, boost::lexical_cast<std::string>(port));
    boost::asio::ip::tcp::resolver::iterator endpoint_iterator = resolver_.resolve(query);

    XDEBUG << "Connecting to remote address: " << endpoint_iterator->endpoint().address();

    boost::asio::async_connect(remote_socket_.lowest_layer(), endpoint_iterator,
                               boost::bind(&HttpsDirectHandler::OnRemoteConnected,
                                           this,
                                           boost::asio::placeholders::error));
}

void HttpsDirectHandler::OnRemoteConnected(const boost::system::error_code& e) {
    if(e) {
        XWARN << "Failed to connect to remote server, message: " << e.message();
        session_.Stop();
        return;
    }

    static std::string response("HTTP/1.1 200 Connection Established\r\nProxy-Connection: Keep-Alive\r\n\r\n");
    boost::asio::async_write(local_ssl_socket_.next_layer(), boost::asio::buffer(response),
                             boost::bind(&HttpsDirectHandler::OnLocalDataSent,
                                         this,
                                         boost::asio::placeholders::error, false));

    local_ssl_socket_.async_handshake(boost::asio::ssl::stream_base::server,
                                      boost::bind(&HttpsDirectHandler::OnLocalHandshaken,
                                                  this,
                                                  boost::asio::placeholders::error));
}

void HttpsDirectHandler::OnRemoteDataSent(const boost::system::error_code& e) {
    if(e) {
        XWARN << "Failed to write request to remote server, message: " << e.message();
        session_.Stop();
        return;
    }
    boost::asio::async_read_until(remote_socket_, remote_buffer_, "\r\n",
                                  boost::bind(&HttpsDirectHandler::OnRemoteStatusLineReceived,
                                              this,
                                              boost::asio::placeholders::error));
}

void HttpsDirectHandler::OnRemoteStatusLineReceived(const boost::system::error_code& e) {
    if(e) {
        XWARN << "Failed to read status line from remote server, message: " << e.message();
        session_.Stop();
        return;
    }

    // As async_read_until may return more data beyond the delimiter, so we only process the status line
    std::istream response(&remote_buffer_);
    std::getline(response, response_.status_line());
    response_.status_line() += '\n'; // append the missing newline character

    XDEBUG << "Status line from remote server: " << response_.status_line();

    boost::asio::async_write(local_ssl_socket_, boost::asio::buffer(response_.status_line()),
                             boost::bind(&HttpsDirectHandler::OnLocalDataSent,
                                         this,
                                         boost::asio::placeholders::error, false));

    boost::asio::async_read_until(remote_socket_, remote_buffer_, "\r\n\r\n",
                                  boost::bind(&HttpsDirectHandler::OnRemoteHeadersReceived,
                                              this,
                                              boost::asio::placeholders::error));
}

void HttpsDirectHandler::OnRemoteHeadersReceived(const boost::system::error_code& e) {
    if(e) {
        XWARN << "Failed to read response header from remote server, message: " << e.message();
        session_.Stop();
        return;
    }

    XDEBUG << "Headers from remote server: \n" << boost::asio::buffer_cast<const char *>(remote_buffer_.data());

    std::istream response(&remote_buffer_);
    std::string header;
    std::size_t body_len = 0;
    bool chunked_encoding = false;
    while(std::getline(response, header)) {
        if(header == "\r") { // there is no more headers
            XDEBUG << "no more headers";
            break;
        }

        std::string::size_type sep_idx = header.find(": ");
        if(sep_idx == std::string::npos) {
            XWARN << "Invalid header: " << header;
            continue;
        }

        std::string name = header.substr(0, sep_idx);
        std::string value = header.substr(sep_idx + 2, header.length() - 1 - name.length() - 2); // remove the last \r
        response_.AddHeader(name, value);

        XTRACE << "header name: " << name << ", value: " << value;

        if(name == "Transfer-Encoding") {
            XINFO << "Transfer-Encoding header is found, value: " << value;
            if(value == "chunked")
                chunked_encoding = true;
        }

        if(name == "Content-Length") {
            if(chunked_encoding)
                XWARN << "Both Transfer-Encoding and Content-Length headers are found";
            boost::algorithm::trim(value);
            body_len = boost::lexical_cast<std::size_t>(value);
        }
    }

    boost::asio::async_write(local_ssl_socket_, boost::asio::buffer(response_.headers()),
                             boost::bind(&HttpsDirectHandler::OnLocalDataSent,
                                         this,
                                         boost::asio::placeholders::error,
                                         !chunked_encoding && body_len <= 0));

    if(chunked_encoding) {
        boost::asio::async_read(remote_socket_, remote_buffer_, boost::asio::transfer_at_least(1),
                                boost::bind(&HttpsDirectHandler::OnRemoteChunksReceived,
                                            this,
                                            boost::asio::placeholders::error));
        return;
    }

    if(body_len <= 0) {
        XDEBUG << "This response seems have no body.";
        boost::system::error_code ec;
        //remote_socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        return;
    }

    response_.body_lenth(body_len);
    boost::asio::async_read(remote_socket_, remote_buffer_, boost::asio::transfer_at_least(1),
                            boost::bind(&HttpsDirectHandler::OnRemoteBodyReceived,
                                        this,
                                        boost::asio::placeholders::error));
}

void HttpsDirectHandler::OnRemoteChunksReceived(const boost::system::error_code& e) {
    if(e) {
        XWARN << "Failed to read chunk from remote server, message: " << e.message();
        session_.Stop();
        return;
    }

    std::size_t read = remote_buffer_.size();
    std::size_t copied = boost::asio::buffer_copy(boost::asio::buffer(response_.body()), remote_buffer_.data());

    XTRACE << "Chunk from remote server, read size: " << read;
    XTRACE << "Body copied from raw stream to response, copied: " << copied;

    if(copied < read) {
        // TODO here we should handle the condition that the buffer size is
        // smaller than the raw stream size
    }

    remote_buffer_.consume(read);

    bool finished = false;
    if(response_.body()[copied - 4] == '\r' && response_.body()[copied - 3] == '\n'
                    && response_.body()[copied - 2] == '\r' && response_.body()[copied - 1] == '\n')
            finished = true;

    boost::asio::async_write(local_ssl_socket_, boost::asio::buffer(response_.body(), copied),
                             boost::bind(&HttpsDirectHandler::OnLocalDataSent,
                                         this,
                                         boost::asio::placeholders::error, finished));

    if(!finished) {
        boost::asio::async_read(remote_socket_, remote_buffer_, boost::asio::transfer_at_least(1),
                                boost::bind(&HttpsDirectHandler::OnRemoteChunksReceived,
                                            this,
                                            boost::asio::placeholders::error));
    } else {
        boost::system::error_code ec;
        //remote_socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    }
}

void HttpsDirectHandler::OnLocalDataReceived(const boost::system::error_code& e, std::size_t size) {
    if(e) {
        XWARN << "Failed to receive data from local socket, message: " << e.message();
        session_.Terminate();
        return;
    }

    total_size_ += size;

    XTRACE << "Dump ssl encrypted data from local socket(size:" << total_size_ << "):\n"
           << "--------------------------------------------\n"
           << std::string(local_buffer_, local_buffer_ + total_size_)
           << "\n--------------------------------------------";

    // TODO can we build on the original request?
    // TODO2 we should send the raw data directly, do not do parse and compose work
    HttpRequest::State result = HttpRequest::BuildRequest(local_buffer_, total_size_, *request_);

    if(result != HttpRequest::kComplete) {
        XWARN << "This request is not complete, continue to read from the ssl socket.";
        local_ssl_socket_.async_read_some(boost::asio::buffer(local_buffer_ + total_size_, 4096 - total_size_),
                                          boost::bind(&HttpsDirectHandler::OnLocalDataReceived,
                                                      this,
                                                      boost::asio::placeholders::error,
                                                      boost::asio::placeholders::bytes_transferred));
    }

//    if(result == HttpRequest::kIncomplete) {
//        XWARN << "Not a complete request, but currently partial request is not supported.";
//        session_.Terminate();
//        return;
//    } else if(result == HttpRequest::kBadRequest) {
//        XWARN << "Bad request: " << local_ssl_socket_.lowest_layer().remote_endpoint().address()
//              << ":" << local_ssl_socket_.lowest_layer().remote_endpoint().port();
//        // TODO here we should write a bad request response back
//        session_.Terminate();
//        return;
//    }

    // do remote hand shake here, and then sends the request
    remote_socket_.async_handshake(boost::asio::ssl::stream_base::client,
                                   boost::bind(&HttpsDirectHandler::OnRemoteHandshaken,
                                               this,
                                               boost::asio::placeholders::error));
}

void HttpsDirectHandler::OnRemoteBodyReceived(const boost::system::error_code& e) {
    if(e) {
        if(e == boost::asio::error::eof)
            XDEBUG << "The remote peer closed the connection.";
        else {
            XWARN << "Failed to read body from remote server, message: " << e.message();
            session_.Stop();
            return;
        }
    }

    std::size_t read = remote_buffer_.size();
    std::size_t copied = boost::asio::buffer_copy(boost::asio::buffer(response_.body()), remote_buffer_.data());

    XDEBUG << "Body from remote server, size: " << read
           << ", content:\n" << boost::asio::buffer_cast<const char *>(remote_buffer_.data());
    XDEBUG << "Body copied from raw stream to response, copied: " << copied
           << ", response body size: " << response_.body().size();

    boost::asio::async_write(local_ssl_socket_, boost::asio::buffer(response_.body(), copied),
                             boost::bind(&HttpsDirectHandler::OnLocalDataSent,
                                         this,
                                         boost::asio::placeholders::error, read >= response_.body_length()));
    if(copied < read) {
        // TODO the response's body buffer is less than read content, try write again
    }

    remote_buffer_.consume(read); // the read bytes are consumed

    if(read < response_.body_length()) { // there is more content
        response_.body_lenth(response_.body_length() - read);
        boost::asio::async_read(remote_socket_, remote_buffer_, boost::asio::transfer_at_least(1/*body_len*/),
                                boost::bind(&HttpsDirectHandler::OnRemoteBodyReceived,
                                            this,
                                            boost::asio::placeholders::error));
    } else {
        boost::system::error_code ec;
        //remote_socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    }
}

void HttpsDirectHandler::OnLocalDataSent(const boost::system::error_code& e,
                                         bool finished) {
    if(e) {
        XWARN << "Failed to write response to local socket, message: " << e.message();
        session_.Stop();
        return;
    }

    XDEBUG << "Content written to local socket.";

    if(!finished)
        return;

    if(!e) {
        boost::system::error_code ec;
        //local_socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        //remote_socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    }

    if(e != boost::asio::error::operation_aborted) {
        //manager_.Stop(shared_from_this());
        session_.Terminate();
    }
}

bool HttpsDirectHandler::VerifyCertificate(bool pre_verified, boost::asio::ssl::verify_context& ctx) {
    // TODO enhance this function
    char subject_name[256];
    X509 *cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
    X509_NAME_oneline(X509_get_subject_name(cert), subject_name, 256);
    XDEBUG << "Verify remote certificate, subject name: " << subject_name
           << ", pre_verified value: " << pre_verified;

    return true;
}

void HttpsDirectHandler::OnLocalHandshaken(const boost::system::error_code& e) {
    if(e) {
        XWARN << "Failed to handshake with local client, message: " << e.message();
        session_.Stop();
        return;
    }

    local_ssl_socket_.async_read_some(boost::asio::buffer(local_buffer_, 4096),
                                      boost::bind(&HttpsDirectHandler::OnLocalDataReceived,
                                                  this,
                                                  boost::asio::placeholders::error,
                                                  boost::asio::placeholders::bytes_transferred));
}

void HttpsDirectHandler::OnRemoteHandshaken(const boost::system::error_code& e) {
    if(e) {
        XWARN << "Failed to handshake with remote server, message: " << e.message();
        session_.Stop();
        return;
    }

    XTRACE << boost::asio::buffer_cast<const char *>(request_->OutboundBuffer().data());

    boost::asio::async_write(remote_socket_, request_->OutboundBuffer(),
                             boost::bind(&HttpsDirectHandler::OnRemoteDataSent,
                                         this,
                                         boost::asio::placeholders::error));
}