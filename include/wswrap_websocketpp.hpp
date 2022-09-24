// websocketpp implementation of wswrap
#ifndef _WSWRAP_WEBSOCKETPP_HPP
#define _WSWRAP_WEBSOCKETPP_HPP

#ifndef _WSWRAP_HPP
#error "Don't  include wswrap_websocketpp.hpp directly, include wsrap.hpp instead"
#endif


#include <string>
#include <functional>
#include <chrono>
#include <stdarg.h>
#include <asio.hpp>
#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>


namespace wswrap {

    class WS final {
    private:
        typedef asio::io_service SERVICE;
        typedef websocketpp::client<websocketpp::config::asio_client> WSClient;
        typedef struct {
            WSClient first;
            WSClient::connection_ptr second;
        } IMPL;

    public:
        typedef std::function<void(void)> onopen_handler;
        typedef std::function<void(void)> onclose_handler;
        typedef std::function<void(void)> onerror_handler;
        typedef std::function<void(const std::string&)> onmessage_handler;

        WS(const std::string& uri, onopen_handler hopen, onclose_handler hclose, onmessage_handler hmessage, onerror_handler herror=nullptr)
        {
            _service = new SERVICE();
            _impl = new IMPL();
            auto& client = _impl->first;
            auto& conn = _impl->second;
            client.clear_access_channels(websocketpp::log::alevel::all);
            client.set_access_channels(websocketpp::log::alevel::none | websocketpp::log::alevel::app);
            client.clear_error_channels(websocketpp::log::elevel::all);
            client.set_error_channels(websocketpp::log::elevel::warn|websocketpp::log::elevel::rerror|websocketpp::log::elevel::fatal);
            client.init_asio(_service);

            client.set_message_handler([this,hmessage] (websocketpp::connection_hdl hdl, WSClient::message_ptr msg) {
                if (_impl->second && hmessage) hmessage(msg->get_payload());
            });
            client.set_open_handler([this,hopen] (websocketpp::connection_hdl hdl) {
                if (_impl->second && hopen) hopen();
            });
            client.set_close_handler([this,hclose] (websocketpp::connection_hdl hdl) {
                if (_impl->second) {
                    _impl->second = nullptr;
                    if (hclose) hclose();
                }
            });
            client.set_fail_handler([this,herror,hclose] (websocketpp::connection_hdl hdl) {
                if (_impl->second) {
                    _impl->second = nullptr;
                    if (herror) herror();
                    if (hclose) hclose();
                }
            });

            websocketpp::lib::error_code ec;
            conn = client.get_connection(uri, ec);
            if (ec) {
                // TODO: run close and error handler? or throw exception?
            }
            if (!client.connect(conn)) {
                // TODO: run close and error handler? or throw exception?
            }
        }

        virtual ~WS()
        {
            auto& client = _impl->first;
            auto& conn = _impl->second;
            client.set_message_handler(nullptr);
            client.set_open_handler(nullptr);
            client.set_close_handler(nullptr);
            client.set_fail_handler([this](...){ _impl->second = nullptr; });
            try {
                if (conn) {
                    conn->close(websocketpp::close::status::normal, "");
                    conn = nullptr;
                    // wait for connection to close -- client.run() will hang if
                    // if the destructor is called from a message callback, so we poll
                    // with timeout instead, possibly leaking the underlying socket
                    auto t = std::chrono::steady_clock::now();
                    while (!client.stopped()) {
                        client.poll();
                        auto td = (std::chrono::steady_clock::now()-t);
                        if (td > std::chrono::milliseconds(500)) break; // timeout
                    }
                    if (!client.stopped()) {
                        warn("wswrap: disconnect timed out. "
                             "Possibly disconnecting while handling an event.\n");
                        client.stop();
                    }
                }
            } catch (const std::exception& ex) {
                warn("wswrap: exception during close: %s\n", ex.what());
                conn = nullptr;
                client.stop();
            }
            // NOTE: the destructor can not be called from a ws callback in some
            //       circumstances, otherwise it will hang here. TODO: Document this.
            delete _impl;
            _impl = nullptr;
            delete _service;
            _service = nullptr;
        }

        unsigned long get_ok_connect_interval() const
        {
            return 1000;
        }

        void send(const std::string& data)
        {
            bool binary = data.find('\0') != data.npos; // TODO: detect if data is valid UTF8
            if (binary)
                send_binary(data);
            else
                send_text(data);
        }

        void send_text(const std::string& data)
        {
            _impl->first.send(_impl->second,data,websocketpp::frame::opcode::text);
        }

        void send_binary(const std::string& data)
        {
            _impl->first.send(_impl->second,data,websocketpp::frame::opcode::binary);
        }

        bool poll()
        {
            return _service->poll();
        }

        size_t run()
        {
            return _service->run();
        }

    private:
        void warn(const char* fmt, ...)
        {
            va_list args;
            va_start (args, fmt);
            vfprintf (stderr, fmt, args);
            va_end (args);
        }

        IMPL *_impl;
        SERVICE *_service;
    };

}; // namespace wsrap

#endif //_WSWRAP_WEBSOCKETPP_HPP
