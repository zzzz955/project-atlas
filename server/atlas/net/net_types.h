#pragma once

// =============================================================================
// net 계층 전역에서 쓰는 boost::asio 타입 별칭 모음
// =============================================================================

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

namespace atlas
{

namespace asio = boost::asio;  // 별칭일 뿐. using-directive 아니라 심볼 누출 없음

using ErrorCode = boost::system::error_code;
using IoContext = asio::io_context;
using Strand = asio::strand< asio::io_context::executor_type >;
using Tcp = asio::ip::tcp;
using Socket = Tcp::socket;
using Acceptor = Tcp::acceptor;
using Endpoint = Tcp::endpoint;
using SteadyTimer = asio::steady_timer;

}  // namespace atlas
