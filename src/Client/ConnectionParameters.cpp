#include "ConnectionParameters.h"
#include <fstream>
#include <Core/Defines.h>
#include <Core/Protocol.h>
#include <Core/Types.h>
#include <IO/ConnectionTimeouts.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <Common/SSH/Wrappers.h>
#include <Common/Exception.h>
#include <Common/isLocalAddress.h>
#include <Common/DNSResolver.h>
#include <base/scope_guard.h>

#include <readpassphrase/readpassphrase.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int SUPPORT_IS_DISABLED;
}

namespace
{

bool enableSecureConnection(const Poco::Util::AbstractConfiguration & config, const std::string & connection_host)
{
    if (config.getBool("secure", false))
        return true;

    if (config.getBool("no-secure", false))
        return false;

    bool is_clickhouse_cloud = connection_host.ends_with(".clickhouse.cloud") || connection_host.ends_with(".clickhouse-staging.com");
    return is_clickhouse_cloud;
}

}

ConnectionParameters ConnectionParameters::createForEmbedded(const String & user, const String & database)
{
    auto connection_params = ConnectionParameters();
    connection_params.host = "localhost";
    connection_params.security = Protocol::Secure::Disable;
    connection_params.password = "";
    connection_params.user = user;
    connection_params.default_database = database;
    connection_params.compression = Protocol::Compression::Disable;

    // TODO: Pass settings struct.
    // connection_params.timeouts = ConnectionTimeouts::getTCPTimeoutsWithFailover(getGlobal);

    connection_params.timeouts.sync_request_timeout = Poco::Timespan(DBMS_DEFAULT_SYNC_REQUEST_TIMEOUT_SEC, 0);
    return connection_params;
}

ConnectionParameters::ConnectionParameters(const Poco::Util::AbstractConfiguration & config,
                                           const Host & host_,
                                           const Database & database,
                                           std::optional<UInt16> port_)
    : host(host_)
    , port(port_.value_or(getPortFromConfig(config, host_)))
    , default_database(database)
{
    security = enableSecureConnection(config, host_) ? Protocol::Secure::Enable : Protocol::Secure::Disable;

    /// changed the default value to "default" to fix the issue when the user in the prompt is blank
    user = config.getString("user", "default");

    if (!config.has("ssh-key-file"))
    {
        bool password_prompt = false;
        if (config.getBool("ask-password", false))
        {
            if (config.has("password"))
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Specified both --password and --ask-password. Remove one of them");
            password_prompt = true;
        }
        else
        {
            password = config.getString("password", "");
            /// if the value of --password is omitted, the password will be set implicitly to "\n"
            if (password == ASK_PASSWORD)
                password_prompt = true;
        }
        if (password_prompt)
        {
            std::string prompt{"Password for user (" + user + "): "};
            char buf[1000] = {};
            if (auto * result = readpassphrase(prompt.c_str(), buf, sizeof(buf), 0))
                password = result;
        }
    }
    else
    {
#if USE_SSH
        std::string filename = config.getString("ssh-key-file");
        std::string passphrase;
        if (config.has("ssh-key-passphrase"))
        {
            passphrase = config.getString("ssh-key-passphrase");
        }
        else
        {
            std::string prompt{"Enter your private key passphrase (leave empty for no passphrase): "};
            char buf[1000] = {};
            if (auto * result = readpassphrase(prompt.c_str(), buf, sizeof(buf), 0))
                passphrase = result;
        }

        ssh::SSHKey key = ssh::SSHKeyFactory::makePrivateFromFile(filename, passphrase);
        if (!key.isPrivate())
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Found public key in file: {} but expected private", filename);

        ssh_private_key = std::move(key);
#else
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "SSH is disabled, because ClickHouse is built without OpenSSL");
#endif
    }

    quota_key = config.getString("quota_key", "");

    /// By default compression is disabled if address looks like localhost.

    /// Avoid DNS request if the host is "localhost".
    /// If ClickHouse is run under QEMU-user with a binary for a different architecture,
    /// and there are all listed startup dependency shared libraries available, but not the runtime dependencies of glibc,
    /// the glibc cannot open "plugins" for DNS resolving, and the DNS resolution does not work.
    /// At the same time, I want clickhouse-local to always work, regardless.
    /// TODO: get rid of glibc, or replace getaddrinfo to c-ares.

    compression = config.getBool("compression", host != "localhost" && !isLocalAddress(DNSResolver::instance().resolveHostAllInOriginOrder(host).front()))
                  ? Protocol::Compression::Enable : Protocol::Compression::Disable;

    timeouts = ConnectionTimeouts()
            .withConnectionTimeout(
                Poco::Timespan(config.getInt("connect_timeout", DBMS_DEFAULT_CONNECT_TIMEOUT_SEC), 0))
            .withSendTimeout(
                Poco::Timespan(config.getInt("send_timeout", DBMS_DEFAULT_SEND_TIMEOUT_SEC), 0))
            .withReceiveTimeout(
                Poco::Timespan(config.getInt("receive_timeout", DBMS_DEFAULT_RECEIVE_TIMEOUT_SEC), 0))
            .withTCPKeepAliveTimeout(
                Poco::Timespan(config.getInt("tcp_keep_alive_timeout", DEFAULT_TCP_KEEP_ALIVE_TIMEOUT), 0))
            .withHandshakeTimeout(
                Poco::Timespan(config.getInt("handshake_timeout_ms", DBMS_DEFAULT_RECEIVE_TIMEOUT_SEC * 1000) * 1000))
            .withSyncRequestTimeout(
                Poco::Timespan(config.getInt("sync_request_timeout", DBMS_DEFAULT_SYNC_REQUEST_TIMEOUT_SEC), 0));
}

ConnectionParameters::ConnectionParameters(const Poco::Util::AbstractConfiguration & config_, const Host & host_, const Database & database_)
    : ConnectionParameters(config_, host_, database_, getPortFromConfig(config_, host_))
{

}

UInt16 ConnectionParameters::getPortFromConfig(const Poco::Util::AbstractConfiguration & config,
                                               std::string connection_host)
{
    bool is_secure = enableSecureConnection(config, connection_host);
    return config.getInt("port",
        config.getInt(is_secure ? "tcp_port_secure" : "tcp_port",
            is_secure ? DBMS_DEFAULT_SECURE_PORT : DBMS_DEFAULT_PORT));
}
}
