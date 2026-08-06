#pragma once
#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

namespace atlas {

namespace asio = boost::asio;  // 네임스페이스 별칭 (using-directive 아님 — 심볼 누출 없음)

using ErrorCode = boost::system::error_code;
using IoContext = asio::io_context;
using Strand = asio::strand<asio::io_context::executor_type>;
using Tcp = asio::ip::tcp;
using Socket = Tcp::socket;
using Acceptor = Tcp::acceptor;
using Endpoint = Tcp::endpoint;
using SteadyTimer = asio::steady_timer;

}  // namespace atlas
