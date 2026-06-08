#include <rpc_client.h>

#include <xrpl/basics/StringUtilities.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_value.h>
#include <xrpl/json/json_writer.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <sstream>
#include <stdexcept>
#include <string>

namespace pqwallet::rpc {

json::Value
call(std::string const& rpcUrl, std::string const& method, json::Value params)
{
    xrpl::ParsedUrl url;
    if (!xrpl::parseUrl(url, rpcUrl) || url.scheme != "http")
        throw std::runtime_error(
            "rpc_client: only plain http:// URLs are supported (got '" + rpcUrl + "')");

    auto const host = url.domain;
    auto const port = url.port ? std::to_string(*url.port) : std::string("80");
    auto const target = url.path.empty() ? std::string("/") : url.path;

    json::Value envelope(json::ValueType::Object);
    envelope["method"] = method;
    json::Value paramsArr(json::ValueType::Array);
    paramsArr.append(std::move(params));
    envelope["params"] = paramsArr;

    json::FastWriter writer;
    auto const body = writer.write(envelope);

    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    using tcp = asio::ip::tcp;

    asio::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    auto const endpoints = resolver.resolve(host, port);
    stream.connect(endpoints);

    http::request<http::string_body> req(http::verb::post, target, 11);
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "pq-wallet-cli/0");
    req.set(http::field::content_type, "application/json");
    req.body() = body;
    req.prepare_payload();
    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    // Ignore shutdown errors — the response is already in hand.

    if (res.result_int() < 200 || res.result_int() >= 300)
    {
        std::ostringstream msg;
        msg << "rpc_client: " << method << " HTTP " << res.result_int() << " — " << res.body();
        throw std::runtime_error(msg.str());
    }

    json::Value root;
    json::Reader reader;
    if (!reader.parse(res.body(), root))
        throw std::runtime_error(
            "rpc_client: response is not valid JSON: " + reader.getFormattedErrorMessages());

    auto const& result = root.get("result", json::Value(json::ValueType::Null));
    if (!result.isObject())
        throw std::runtime_error("rpc_client: response has no result object: " + res.body());

    auto const status = result.get("status", "").asString();
    if (status != "success")
    {
        std::ostringstream msg;
        msg << "rpc_client: " << method << " returned status='" << status << "': " << res.body();
        throw std::runtime_error(msg.str());
    }
    return result;
}

std::uint32_t
fetchSequence(std::string const& rpcUrl, std::string const& accountId)
{
    json::Value params(json::ValueType::Object);
    params["account"] = accountId;
    params["ledger_index"] = "current";
    auto const result = call(rpcUrl, "account_info", std::move(params));
    auto const data = result.get("account_data", json::Value(json::ValueType::Object));
    // Distinguish "field absent" (account not on ledger yet) from
    // "Sequence == 0" (unusual but technically a valid u32 value) by
    // checking field presence rather than relying on a zero default.
    if (!data.isMember("Sequence"))
        throw std::runtime_error(
            "rpc_client::fetchSequence: account_info has no Sequence for"
            " '" +
            accountId + "'. Is the account funded?");
    return data["Sequence"].asUInt();
}

}  // namespace pqwallet::rpc
