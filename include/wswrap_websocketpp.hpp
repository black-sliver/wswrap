// websocketpp implementation of wswrap
#ifndef _WSWRAP_WEBSOCKETPP_HPP
#define _WSWRAP_WEBSOCKETPP_HPP

#ifndef _WSWRAP_HPP
#error "Don't  include wswrap_websocketpp.hpp directly, include wsrap.hpp instead"
#endif


#if !defined WSWRAP_NO_SSL && !defined WSWRAP_WITH_SSL
#define WSWRAP_WITH_SSL // default to SSL enabled
#endif

#if !defined WSWRAP_NO_COMPRESSION && !defined WSWRAP_WITH_COMPRESSION
#define WSWRAP_WITH_COMPRESSION // default to compression enabled
#endif

#include <string>
#include <functional>
#include <chrono>
#include <stdarg.h>
#include <asio.hpp>

#ifdef WSWRAP_WITH_SSL
#include <websocketpp/config/asio_client.hpp>
#else
#include <websocketpp/config/asio_no_tls_client.hpp>
#endif

#ifndef WSWRAP_NO_COMPRESSION
#include <websocketpp/extensions/permessage_deflate/enabled.hpp>
#endif

#include <websocketpp/client.hpp>

#ifdef _WIN32
#include <wincrypt.h>
#endif
#ifdef WSWRAP_ASYNC_CLEANUP
#include <thread>
#endif


namespace wswrap {

    struct client_config: public websocketpp::config::asio_client {
#ifdef WSWRAP_WITH_COMPRESSION
        struct permessage_deflate_config {};
        typedef websocketpp::extensions::permessage_deflate::enabled<permessage_deflate_config>
            permessage_deflate_type;
#endif
    };

#ifdef WSWRAP_WITH_SSL
    struct tls_client_config: public websocketpp::config::asio_tls_client {
#ifdef WSWRAP_WITH_COMPRESSION
        struct permessage_deflate_config {};
        typedef websocketpp::extensions::permessage_deflate::enabled<permessage_deflate_config>
            permessage_deflate_type;
#endif
    };
#endif

    class WS final {
    private:
        typedef asio::io_service SERVICE;
#ifdef WSWRAP_WITH_SSL
        typedef websocketpp::client<tls_client_config> WSSClient;
        typedef asio::ssl::context SSLContext;
        typedef std::shared_ptr<SSLContext> SSLContextPtr;
        struct WSS_IMPL {
            typedef WSSClient Client;
            Client first;
            Client::connection_ptr second;
        };
#endif
        typedef websocketpp::client<client_config> WSClient;
        struct WS_IMPL {
            typedef WSClient Client;
            Client first;
            Client::connection_ptr second;
        };

    public:
        typedef std::function<void(void)> onopen_handler;
        typedef std::function<void(void)> onclose_handler;
        typedef std::function<void(void)> onerror_handler;
        typedef std::function<void(const std::string&)> onerror_ex_handler;
        typedef std::function<void(const std::string&)> onmessage_handler;

        WS(const std::string& uri_string, onopen_handler hopen, onclose_handler hclose, onmessage_handler hmessage,
           onerror_handler herror=nullptr, const std::string& cert_store="")
                : WS(uri_string, hopen, hclose, hmessage, [herror](const std::string&){herror();}, cert_store)
        {
        }

        WS(const std::string& uri_string, onopen_handler hopen, onclose_handler hclose, onmessage_handler hmessage,
           onerror_ex_handler herror=nullptr, const std::string& cert_store="")
                : _hopen(hopen), _hclose(hclose), _hmessage(hmessage), _herror(herror)
        {
            _service = new SERVICE();
            auto uri = websocketpp::uri(uri_string);
            _secure = uri.get_secure();
            bool is_localhost = uri.get_host() == "localhost" || uri.get_host() == "127.0.0.1" || uri.get_host() == "::1";

            bool is_init;
            if (_secure)
                is_init = init_wss(!is_localhost, cert_store);
            else
                is_init = init_ws();

            if (!is_init) {
                #ifdef __cpp_exceptions
                throw std::runtime_error("WS init failed");
                #else
                _connect_error = true;
                if (_connect_error_message.empty()) _connect_error_message = "WS init failed";
                #endif
                return;
            }

            connect(uri_string);
        }

        virtual ~WS()
        {
            cleanup();
        }

        unsigned long get_ok_connect_interval() const
        {
            return 1000;
        }

#ifdef WSWRAP_SEND_EXCEPTIONS
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
            #ifdef WSWRAP_WITH_SSL
            if (_secure)
                send<WSS_IMPL>(data, websocketpp::frame::opcode::text);
            else
            #endif
                send<WS_IMPL>(data, websocketpp::frame::opcode::text);
        }

        void send_binary(const std::string& data)
        {
            #ifdef WSWRAP_WITH_SSL
            if (_secure)
                send<WSS_IMPL>(data, websocketpp::frame::opcode::binary);
            else
            #endif
                send<WS_IMPL>(data, websocketpp::frame::opcode::binary);
        }
#else
        bool send(const std::string& data)
        {
            bool binary = data.find('\0') != data.npos; // TODO: detect if data is valid UTF8
            if (binary)
                return send_binary(data);
            else
                return send_text(data);
        }

        bool send_text(const std::string& data)
        {
            #ifdef WSWRAP_WITH_SSL
            if (_secure)
                return send<WSS_IMPL>(data, websocketpp::frame::opcode::text);
            else
            #endif
                return send<WS_IMPL>(data, websocketpp::frame::opcode::text);
        }

        bool send_binary(const std::string& data)
        {
            #ifdef WSWRAP_WITH_SSL
            if (_secure)
                return send<WSS_IMPL>(data, websocketpp::frame::opcode::binary);
            else
            #endif
                return send<WS_IMPL>(data, websocketpp::frame::opcode::binary);
        }
#endif

        bool poll()
        {
            #ifndef __cpp_exceptions
            if (_connect_error) {
                if (_herror) _herror(_connect_error_message);
                if (_hclose) _hclose();
                return false;
            }
            #endif
            _polling = true;
            auto res = _service->poll();
            _polling = false;
            return res;
        }

        size_t run()
        {
            #ifndef __cpp_exceptions
            if (_connect_error) {
                if (_herror) _herror(_connect_error_message);
                if (_hclose) _hclose();
                return 0;
            }
            #endif
            _polling = true;
            auto res = _service->run();
            _polling = false;
            return res;
        }

        void stop()
        {
            _service->stop();
        }

    private:
        template<class T>
        T* init()
        {
            T* impl = new T();
            auto& client = impl->first;
            client.clear_access_channels(websocketpp::log::alevel::all);
            client.set_access_channels(websocketpp::log::alevel::none | websocketpp::log::alevel::app);
            client.clear_error_channels(websocketpp::log::elevel::all);
            client.set_error_channels(websocketpp::log::elevel::warn|websocketpp::log::elevel::rerror|websocketpp::log::elevel::fatal);
            client.set_pong_timeout(10000);
            client.init_asio(_service);

            typedef typename T::Client::message_ptr message_ptr;
            client.set_message_handler([this] (websocketpp::connection_hdl, const message_ptr& msg) {
                T* impl = (T*)_impl;
                if (impl->second && _hmessage) _hmessage(msg->get_payload());
            });
            client.set_open_handler([this] (websocketpp::connection_hdl) {
                T* impl = (T*)_impl;
                if (impl->second && _hopen) _hopen();
                _ping_timer.reset(new asio::high_resolution_timer(*_service));
                _ping_timer->expires_from_now(std::chrono::milliseconds(_ping_interval));
                _ping_timer->async_wait([=](const asio::error_code& ec) { on_ping_expired<T>(ec); });                
            });
            client.set_close_handler([this] (websocketpp::connection_hdl) {
                T* impl = (T*)_impl;
                if (impl->second) {
                    impl->second = nullptr;
                    if (_hclose) _hclose();
                }
                if (_ping_timer)
                    _ping_timer.reset(nullptr);
            });
            client.set_fail_handler([this] (websocketpp::connection_hdl hdl) {
                T* impl = (T*)_impl;
                if (impl->second) {
                    auto ec = impl->first.get_con_from_hdl(hdl)->get_ec();
                    impl->second = nullptr;
                    if (_herror) _herror(ec.message());
                    if (_hclose) _hclose();
                }
                if (_ping_timer)
                    _ping_timer.reset(nullptr);
            });
            client.set_ping_handler([this] (websocketpp::connection_hdl, const std::string&) {
                // reset ping timer - no need to ping the server if the server pings us
                if (_ping_timer)
                    _ping_timer->expires_from_now(std::chrono::milliseconds(_ping_interval));
                return true;
            });
            client.set_pong_timeout_handler([this] (websocketpp::connection_hdl, const std::string&) {
                T* impl = (T*)_impl;
                if (impl->second && impl->second->get_state() < websocketpp::session::state::closing) {
                    impl->second->close(websocketpp::close::status::internal_endpoint_error, "ping timeout");
                }
            });

            return impl;
        }

        bool init_ws()
        {
            auto* impl = init<WS_IMPL>();
            _impl = impl;
            if (!impl) return false;
            return true;
        }

        bool init_wss(bool validate_cert, const std::string& cert_store)
        {
            #ifdef WSWRAP_WITH_SSL
            auto* impl = init<WSS_IMPL>();
            _impl = impl;
            if (!impl) return false;
            auto& client = impl->first;

            std::string store_path = cert_store; // make a copy for capture
            client.set_tls_init_handler([this, validate_cert, store_path] (std::weak_ptr<void>) -> SSLContextPtr {
                SSLContextPtr ctx = std::make_shared<SSLContext>(SSLContext::sslv23);
                asio::error_code ec;
                ctx->set_options(SSLContext::default_workarounds |
                                 SSLContext::no_sslv2 |
                                 SSLContext::no_sslv3 |
                                 SSLContext::no_tlsv1 |
                                 SSLContext::no_tlsv1_1 |
                                 SSLContext::single_dh_use, ec);
                if (ec) warn("Error in ssl init: options: %s\n", ec.message().c_str());
                if (validate_cert) {
                    if (!store_path.empty()) {
                        ctx->load_verify_file(store_path, ec);
                        if (ec) warn("Error in ssl init: load store: %s\n", ec.message().c_str());
                    }
                    if (store_path.empty() || !!ec) {
#ifdef _WIN32
                        // try to load certs from windows ca store
                        HCERTSTORE hStore = CertOpenSystemStoreA(0, "ROOT");
                        if (hStore) {
                            X509_STORE* store = X509_STORE_new();
                            PCCERT_CONTEXT cert = NULL;
                            while ((cert = CertEnumCertificatesInStore(hStore, cert)) != NULL) {
                                X509 *x509 = d2i_X509(NULL,
                                                      (const unsigned char **)&cert->pbCertEncoded,
                                                      cert->cbCertEncoded);
                                if(x509) {
                                    X509_STORE_add_cert(store, x509);
                                    X509_free(x509);
                                }
                            }

                            CertFreeCertificateContext(cert);
                            CertCloseStore(hStore, 0);

                            SSL_CTX_set_cert_store(ctx->native_handle(), store);
                        } else {
                            warn("Error in ssl init: could not open windows ca store\n");
                        }
#else
                        // try openssl default location
                        ctx->set_default_verify_paths(ec);
                        if (ec) warn("Error in ssl init: paths: %s\n", ec.message().c_str());
#endif
                    }
                    ctx->set_verify_mode(asio::ssl::verify_peer, ec);
                    if (ec) warn("Error in ssl init: mode: %s\n", ec.message().c_str());
                }
                return ctx;
            });
            return true;
            #else
            #ifdef __cpp_exceptions
            throw std::runtime_error("Requested SSL but not built in");
            #else
            warn("Requested SSL but not built in!\n");
            _connect_error_message = "Requested SSL but not built in!";
            #endif
            return false;
            #endif
        }

        template<class T>
        bool connect(const std::string& uri)
        {
            T* impl = (T*)_impl;
            auto& client = impl->first;
            auto& conn = impl->second;
            websocketpp::lib::error_code ec;
            conn = client.get_connection(uri, ec);
            if (ec) {
                #ifdef __cpp_exceptions
                throw std::system_error(ec);
                #else
                _connect_error = true;
                _connect_error_message = ec.message();
                #endif
                return false;
            }
            if (!client.connect(conn)) {
                #ifdef __cpp_exceptions
                throw std::runtime_error("Connect failed");
                #else
                _connect_error = true;
                _connect_error_message = "Connect failed";
                #endif
                return false;
            }
            return true;
        }

        bool connect(const std::string& uri)
        {
            #ifdef WSWRAP_WITH_SSL
            if (_secure)
                return connect<WSS_IMPL>(uri);
            else
            #endif
                return connect<WS_IMPL>(uri);
        }

        template<class T>
        void cleanup()
        {
            #if defined _WIN32 && !defined WSWRAP_ASYNC_CLEANUP && defined __cpp_exceptions
            // NOTE: the destructor can not be called from a ws callback in some
            //       circumstances, otherwise it will hang at delete impl on Windows.
            if (_polling) {
                throw std::runtime_error("Cannot delete WS from a callback unless WSWRAP_ASYNC_CLEANUP is defined!");
            }
            #endif
            if (_ping_timer)
                _ping_timer.reset(nullptr);
            T* impl = (T*)_impl;
            auto& client = impl->first;
            auto& conn = impl->second;
            client.set_message_handler(nullptr);
            client.set_open_handler(nullptr);
            client.set_close_handler(nullptr);
            client.set_fail_handler([this](...){ ((T*)_impl)->second = nullptr; });
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
            #ifdef WSWRAP_ASYNC_CLEANUP
            if (_polling) {
                auto service = _service;
                std::thread([impl, service]() {
                    delete impl;
                    delete service;
                }).detach();
                _impl = nullptr;
                _service = nullptr;
            } else {
                delete impl;
                _impl = nullptr;
                delete _service;
                _service = nullptr;
            }
            #else
            if (_polling) {
                warn("Cannot delete WS from a callback on all platforms unless WSWRAP_ASYNC_CLEANUP is defined!\n");
                #ifdef _WIN32
                return;
                #endif
            }
            delete impl;
            _impl = nullptr;
            delete _service;
            _service = nullptr;
            #endif
        }

        void cleanup()
        {
            #ifdef WSWRAP_WITH_SSL
            if (_secure)
                cleanup<WSS_IMPL>();
            else
            #endif
                cleanup<WS_IMPL>();
        }

#ifdef WSWRAP_SEND_EXCEPTIONS
        template<class T>
        void send(const std::string& data, websocketpp::frame::opcode::value type)
        {
            ((T*)_impl)->first.send(((T*)_impl)->second, data, type);
        }
#else
        template<class T>
        bool send(const std::string& data, websocketpp::frame::opcode::value type)
        {
            asio::error_code ec;
            ((T*)_impl)->first.send(((T*)_impl)->second, data, type, ec);
            return !ec;
        }
#endif

        template<class T>
        void on_ping_expired(const asio::error_code& ec)
        {
            if (_ping_timer) {
                if (!ec) { // not cancelled
                    asio::error_code ping_ec;
                    T* impl = (T*)_impl;
                    impl->first.ping(impl->second, "", ping_ec);
                    if (ping_ec) {
                        if (_herror) _herror(ec.message());
                        impl->second->close(websocketpp::close::status::internal_endpoint_error, "send failed");
                    }
                }
                _ping_timer->expires_from_now(std::chrono::milliseconds(_ping_interval));
                _ping_timer->async_wait([=](const asio::error_code& ec) { on_ping_expired<T>(ec); });
            }
        }

        void warn(const char* fmt, ...)
        {
            va_list args;
            va_start (args, fmt);
            vfprintf (stderr, fmt, args);
            va_end (args);
        }

        void *_impl;
        SERVICE *_service;
        bool _secure;
        bool _polling = false;
        std::unique_ptr<asio::high_resolution_timer> _ping_timer;
        int _ping_interval = 25000;
        onopen_handler _hopen;
        onclose_handler _hclose;
        onmessage_handler _hmessage;
        onerror_ex_handler _herror;
#ifndef __cpp_exceptions
        bool _connect_error = false;
        std::string _connect_error_message;
#endif
    };

}; // namespace wsrap

#endif //_WSWRAP_WEBSOCKETPP_HPP
