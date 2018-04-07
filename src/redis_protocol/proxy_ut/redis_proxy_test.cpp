// Copyright (c) 2017, Xiaomi, Inc.  All rights reserved.
// This source code is licensed under the Apache License Version 2.0, which
// can be found in the LICENSE file in the root directory of this source tree.

#include <algorithm>
#include <string>
#include <boost/lexical_cast.hpp>
#include <boost/asio.hpp>

#include <gtest/gtest.h>
#include <rrdb/rrdb.client.h>
#include <pegasus_utils.h>
#include "proxy_layer.h"
#include "redis_parser.h"

using namespace boost::asio;
using namespace ::pegasus::proxy;

class proxy_app : public ::dsn::service_app
{
public:
    proxy_app(const dsn::service_app_info *info) : service_app(info) {}
    virtual ~proxy_app() {}

    virtual ::dsn::error_code start(const std::vector<std::string> &args) override
    {
        if (args.size() < 2)
            return ::dsn::ERR_INVALID_PARAMETERS;
        proxy_session::factory f = [](proxy_stub *p, ::dsn::rpc_address remote) {
            return std::make_shared<redis_parser>(p, remote);
        };
        _proxy.reset(new proxy_stub(f, args[1].c_str()));
        return ::dsn::ERR_OK;
    }
    virtual ::dsn::error_code stop(bool) override { return ::dsn::ERR_OK; }
private:
    std::unique_ptr<pegasus::proxy::proxy_stub> _proxy;
};

bool blob_compare(const ::dsn::blob &bb1, const ::dsn::blob &bb2)
{
    return bb1.length() == bb2.length() && memcmp(bb1.data(), bb2.data(), bb1.length()) == 0;
}

class redis_test_parser1 : public redis_parser
{
protected:
    virtual void handle_command(std::unique_ptr<message_entry> &&entry)
    {
        redis_request &act_request = entry->request;
        redis_request &exp_request = reserved_entry[entry_index].request;

        ASSERT_TRUE(act_request.length > 0);
        ASSERT_EQ(act_request.length, exp_request.length);
        for (unsigned int i = 0; i < act_request.length; ++i) {
            redis_bulk_string &bs1 = act_request.buffers[i];
            redis_bulk_string &bs2 = exp_request.buffers[i];
            ASSERT_EQ(bs1.length, bs2.length);
            if (bs1.length > 0)
                ASSERT_TRUE(blob_compare(bs1.data, bs2.data));
        }

        got_a_message = true;
        ++entry_index;
    }

public:
    redis_test_parser1(proxy_stub *stub, ::dsn::rpc_address addr) : redis_parser(stub, addr)
    {
        reserved_entry.resize(20);
    }

    void test_fixed_cases()
    {
        std::cout << "test fixed cases" << std::endl;

        redis_request &rr = reserved_entry[0].request;
        // simple case
        {
            rr.length = 3;
            rr.buffers = {{3, "SET"}, {3, "foo"}, {3, "bar"}};
            got_a_message = false;
            entry_index = 0;

            const char *request_data = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
            auto request = create_message(request_data);
            ASSERT_TRUE(parse(request));
            ASSERT_TRUE(got_a_message);
        }

        // message segmented
        {
            got_a_message = false;
            entry_index = 0;
            const char *request_data1 = "*3\r\n$3\r\nSET\r\n$3\r";
            const char *request_data2 = "\nfoo\r\n$3\r\nbar\r\n";
            auto request1 = create_message(request_data1);
            auto request2 = create_message(request_data2);
            ASSERT_TRUE(parse(request1));
            ASSERT_TRUE(parse(request2));
            ASSERT_TRUE(got_a_message);
        }

        // wrong message
        {
            got_a_message = false;
            const char *data[] = {"$1\r\n$1\r\nt\r\n",
                                  "*1\r$5\r\ntest_\r\n",
                                  "*hello\r\n$1\r\nt\r\n",
                                  "*-23\r\n$1\r\nt\r\n",
                                  "*1\r\n12\r\ntest_command\r\n",
                                  "*1\r\n$12test_command\r\n",
                                  "*1\r\n$12\rtest_command\r\n",
                                  "*2\r\n$3\r\nget\r\n*6\r\nkeykey\r\n",
                                  "*2\r\n$3\r\nget\r\n$6\rkeykey\r\n",
                                  nullptr};

            for (unsigned int i = 0; data[i]; ++i) {
                auto request = create_message(data[i]);
                ASSERT_FALSE(parse(request));
                ASSERT_FALSE(got_a_message);
            }
        }

        // after wrong message, parser should be reset
        {
            got_a_message = false;
            entry_index = 0;
            rr.length = 3;
            rr.buffers = {{3, "set"}, {5, "hello"}, {0, ""}};

            const char *data = "*3\r\n$3\r\nset\r\n$5\r\nhello\r\n$0\r\n\r\n";
            auto request = create_message(data);
            ASSERT_TRUE(parse(request));
            ASSERT_TRUE(got_a_message);
        }

        // test nil bulk string
        {
            got_a_message = false;
            entry_index = 0;
            rr.length = 1;
            rr.buffers = {{-1, ""}};

            const char *data = "*1\r\n$-1\r\n";
            ASSERT_TRUE(parse(create_message(data)));
            ASSERT_TRUE(got_a_message);
        }
    }

    void test_random_cases()
    {
        std::cout << "test random cases" << std::endl;

        int total_requests = 10;
        std::vector<dsn_message_t> fake_requests;
        int total_body_size = 0;

        // create several requests
        for (entry_index = 0; entry_index < total_requests; ++entry_index) {
            redis_request &ra = reserved_entry[entry_index].request;
            ra.length = dsn_random32(1, 20);
            ra.buffers.resize(ra.length);
            for (unsigned int i = 0; i != ra.length; ++i) {
                redis_bulk_string &bs = ra.buffers[i];
                bs.length = dsn_random32(0, 8);
                if (bs.length == 0) {
                    bs.length = -1;
                } else if (bs.length == 1) {
                    bs.length = 0;
                } else {
                    bs.length = dsn_random32(1, 256);
                    std::shared_ptr<char> raw_buf(new char[bs.length],
                                                  std::default_delete<char[]>());
                    memset(raw_buf.get(), 't', bs.length);
                    bs.data.assign(std::move(raw_buf), 0, bs.length);
                }
            }
            dsn_message_t fake_response = marshalling_array(ra);
            dsn_message_t fake_request = dsn_msg_copy(fake_response, true, true);

            dsn_msg_add_ref(fake_response);
            dsn_msg_release_ref(fake_response);

            fake_requests.push_back(fake_request);
            total_body_size += dsn_msg_body_size(fake_request);
        }

        // let's copy the messages
        std::shared_ptr<char> msg_buffer(new char[total_body_size + 10],
                                         std::default_delete<char[]>());
        char *raw_msg_buffer = msg_buffer.get();

        for (dsn_message_t r : fake_requests) {
            void *rw_ptr;
            size_t length;
            while (dsn_msg_read_next(r, &rw_ptr, &length)) {
                memcpy(raw_msg_buffer, rw_ptr, length);
                raw_msg_buffer += length;
                dsn_msg_read_commit(r, length);
            }
            dsn_msg_add_ref(r);
            dsn_msg_release_ref(r);
        }
        *raw_msg_buffer = 0;

        ASSERT_EQ(raw_msg_buffer - msg_buffer.get(), total_body_size);

        raw_msg_buffer = msg_buffer.get();
        // first create a big message, test the pipeline
        {
            dsn_message_t msg = create_message(raw_msg_buffer, total_body_size);
            entry_index = 0;
            ASSERT_TRUE(parse(msg));
            ASSERT_EQ(entry_index, total_requests);
        }

        // let's split the messages into different pieces
        {
            entry_index = 0;
            size_t slice_count = dsn_random32(total_requests, total_body_size);
            std::vector<int> start_pos;
            start_pos.push_back(0);
            for (unsigned int i = 0; i < slice_count - 1; ++i) {
                start_pos.push_back(dsn_random32(0, total_body_size - 1));
            }
            start_pos.push_back(total_body_size);
            std::sort(start_pos.begin(), start_pos.end());

            for (unsigned i = 0; i < start_pos.size() - 1; ++i) {
                if (start_pos[i] != start_pos[i + 1]) {
                    int length = start_pos[i + 1] - start_pos[i];
                    dsn_message_t msg = create_message(raw_msg_buffer + start_pos[i], length);
                    ASSERT_TRUE(parse(msg));
                }
            }
            ASSERT_EQ(entry_index, total_requests);
        }
    }

public:
    static dsn_message_t create_message(const char *data)
    {
        dsn_message_t m = dsn_msg_create_received_request(
            RPC_CALL_RAW_MESSAGE, DSF_THRIFT_BINARY, (void *)data, strlen(data));
        dsn_msg_add_ref(m);
        return m;
    }
    static dsn_message_t create_message(const char *data, int length)
    {
        dsn_message_t m = dsn_msg_create_received_request(
            RPC_CALL_RAW_MESSAGE, DSF_THRIFT_BINARY, (void *)data, length);
        dsn_msg_add_ref(m);
        return m;
    }
    static dsn_message_t marshalling_array(const redis_request &ra)
    {
        dsn_message_t m = create_message("dummy");

        dsn_message_t result = dsn_msg_create_response(m);
        ::dsn::rpc_write_stream stream(result);

        stream.write_pod('*');
        std::string array_size = boost::lexical_cast<std::string>(ra.length);
        stream.write(array_size.c_str(), array_size.length());
        stream.write_pod('\r');
        stream.write_pod('\n');

        for (unsigned int i = 0; i != ra.length; ++i) {
            redis_parser::marshalling(stream, ra.buffers[i]);
        }
        dsn_msg_release_ref(m);
        return result;
    }

    std::vector<message_entry> reserved_entry;
    int entry_index;
    bool got_a_message;
};

TEST(proxy, parser)
{
    std::shared_ptr<redis_test_parser1> parser(
        new redis_test_parser1(nullptr, ::dsn::rpc_address("127.0.0.1", 123)));
    parser->test_fixed_cases();
    parser->test_random_cases();
}

TEST(proxy, utils)
{
    const char *int_buffers[] = {
        "+", "-", "aabcc", "+aa4aa", "-23455a", "+12345", "-678910", "1223334", nullptr};

    struct
    {
        bool succeed;
        int result;
    } call_results[] = {{false, 0},
                        {false, 0},
                        {false, 0},
                        {false, 0},
                        {false, 0},
                        {true, 12345},
                        {true, -678910},
                        {true, 1223334}};

    for (int i = 0; int_buffers[i]; ++i) {
        int result;
        bool succeed = pegasus::utils::buf2int(int_buffers[i], strlen(int_buffers[i]), result);

        ASSERT_EQ(call_results[i].succeed, succeed);
        if (result == true) {
            ASSERT_EQ(call_results[i].result, result);
        }
    }
}

std::string proxy_address;
TEST(proxy, connection)
{
    io_service ios;
    boost::system::error_code ec;

    std::size_t pos = proxy_address.find(':');
    ASSERT_TRUE(pos != std::string::npos);
    ::dsn::rpc_address redis_server(proxy_address.substr(0, pos).c_str(),
                                    (short)atoi(proxy_address.substr(pos + 1).c_str()));

    ip::tcp::endpoint remote_ep(ip::address_v4(redis_server.ip()), redis_server.port());
    ip::tcp::socket socket(ios);

    socket.open(ip::tcp::v4());
    ASSERT_TRUE(!ec);

    char got_reply[1024];
    // basic pipeline
    {
        std::cout << "test basic pipelines" << std::endl;
        auto ec2 = socket.connect(remote_ep, ec);
        ASSERT_TRUE(!ec2);

        const char *reqs = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$4\r\nbar1\r\n"
                           "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$4\r\nbar2\r\n"
                           "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$4\r\nbar3\r\n";

        size_t reqs_length = strlen(reqs);
        boost::asio::write(socket, boost::asio::buffer(reqs, reqs_length));

        const char *resps = "+OK\r\n"
                            "+OK\r\n"
                            "+OK\r\n";
        size_t got_length =
            boost::asio::read(socket, boost::asio::buffer(got_reply, strlen(resps)));
        got_reply[got_length] = 0;
        ddebug("got length: %u, got reply: %s", got_length, got_reply);
        ASSERT_STREQ(resps, got_reply);
    }

    // then let's get the value
    {
        std::cout << "test get values after previous pipeline write" << std::endl;
        const char *req = "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n";
        size_t req_length = strlen(req);
        boost::asio::write(socket, boost::asio::buffer(req, req_length));

        const char *resp = "$4\r\nbar3\r\n";
        size_t got_length = boost::asio::read(socket, boost::asio::buffer(got_reply, strlen(resp)));
        got_reply[got_length] = 0;
        ddebug("got length: %u, got reply: %s", got_length, got_reply);
        ASSERT_STREQ(resp, got_reply);
    }

    // then ttl test of set key
    {
        std::cout << "test the key with ttl" << std::endl;
        const char *req = "*4\r\n$5\r\nSETEX\r\n$3\r\nfo1\r\n$1\r\n4\r\n$3\r\nbar\r\n"
                          "*4\r\n$5\r\nSETEX\r\n$3\r\nfo2\r\n$4\r\n9999\r\n$3\r\nbar\r\n"
                          "*3\r\n$3\r\nSET\r\n$3\r\nfo3\r\n$3\r\nbar\r\n";
        size_t req_length = strlen(req);
        boost::asio::write(socket, boost::asio::buffer(req, req_length));

        const char *resps = "+OK\r\n"
                            "+OK\r\n"
                            "+OK\r\n";
        size_t got_length =
            boost::asio::read(socket, boost::asio::buffer(got_reply, strlen(resps)));
        got_reply[got_length] = 0;
        ddebug("got length: %u, got reply: %s", got_length, got_reply);
        ASSERT_STREQ(resps, got_reply);
    }

    {
        std::cout << "test ttl" << std::endl;
        const char *req = "*2\r\n$3\r\nTTL\r\n$3\r\nfo2\r\n"
                          "*2\r\n$3\r\nTTL\r\n$3\r\nfo3\r\n"
                          "*2\r\n$3\r\nTTL\r\n$3\r\nfo4\r\n";
        size_t req_length = strlen(req);
        boost::asio::write(socket, boost::asio::buffer(req, req_length));

        const char *resps1 = ":9999\r\n"
                             ":-1\r\n"
                             ":-2\r\n";
        const char *resps2 = ":9998\r\n"
                             ":-1\r\n"
                             ":-2\r\n";
        const char *resps3 = ":9997\r\n"
                             ":-1\r\n"
                             ":-2\r\n";
        const char *resps4 = ":9996\r\n"
                             ":-1\r\n"
                             ":-2\r\n";
        const char *resps5 = ":9995\r\n"
                             ":-1\r\n"
                             ":-2\r\n";
        size_t got_length =
            boost::asio::read(socket, boost::asio::buffer(got_reply, strlen(resps1)));
        got_reply[got_length] = 0;
        ddebug("got length: %u, got reply: %s", got_length, got_reply);
        ASSERT_EQ(got_length, strlen(resps1));
        ASSERT_TRUE(strncmp(got_reply, resps1, got_length) == 0 ||
                    strncmp(got_reply, resps2, got_length) == 0 ||
                    strncmp(got_reply, resps3, got_length) == 0 ||
                    strncmp(got_reply, resps4, got_length) == 0 ||
                    strncmp(got_reply, resps5, got_length) == 0)
            << "got_reply=" << got_reply;
    }

    {
        std::cout << "test read from ttl key" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const char *req = "*2\r\n$3\r\nGET\r\n$3\r\nfo1\r\n"
                          "*2\r\n$3\r\nGET\r\n$3\r\nfo2\r\n"
                          "*2\r\n$3\r\nGET\r\n$3\r\nfo3\r\n";
        size_t req_length = strlen(req);
        boost::asio::write(socket, boost::asio::buffer(req, req_length));

        const char *resps = "$-1\r\n"
                            "$3\r\nbar\r\n"
                            "$3\r\nbar\r\n";
        size_t got_length =
            boost::asio::read(socket, boost::asio::buffer(got_reply, strlen(resps)));
        got_reply[got_length] = 0;
        ddebug("got length: %u, got reply: %s", got_length, got_reply);
        ASSERT_STREQ(resps, got_reply);
    }

    // let's send partitial message then close the socket
    {
        std::cout << "send partial message then close socket" << std::endl;
        const char *req = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$4\r\nbar1\r\n"
                          "*3\r\n$3\r\nSET\r\n$3\r\nfo";
        size_t req_length = strlen(req);
        boost::asio::write(socket, boost::asio::buffer(req, req_length));

        try {
            socket.shutdown(boost::asio::socket_base::shutdown_both);
            socket.close();
        } catch (...) {
            ddebug("exception in shutdown");
        }

        // make sure socket is closed on server, we only want the server remove proxy session to run
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

void dsn_init()
{
    dsn::service_app::register_factory<proxy_app>("proxy");
    dsn_run_config("config.ini", false);
}

void get_proxy_address(int argc, char **argv)
{
    int i;
    for (i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--proxy_address") == 0 || strcmp(argv[i], "-a") == 0) {
            proxy_address = argv[i + 1];
            break;
        }
    }
    if (i >= argc)
        proxy_address = "127.0.0.1:12345";
}

GTEST_API_ int main(int argc, char **argv)
{
    testing::InitGoogleTest(&argc, argv);
    dsn_init();
    get_proxy_address(argc, argv);
    int ans = RUN_ALL_TESTS();
    dsn_exit(ans);
}
