/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AuthSocketMgr_h__
#define AuthSocketMgr_h__

#include "AuthSession.h"
#include "Config.h"
#include "ConnectionFloodProtection.h"
#include "SocketMgr.h"

class AuthSocketThread : public NetworkThread<AuthSession>
{
public:
    void SocketRemoved(std::shared_ptr<AuthSession> const& sock) override
    {
        sConnectionFloodProtection.OnSocketClosed(sock->GetRemoteIpAddress());
    }
};

class AuthSocketMgr : public SocketMgr<AuthSession>
{
    typedef SocketMgr<AuthSession> BaseSocketMgr;

public:
    static AuthSocketMgr& Instance()
    {
        static AuthSocketMgr instance;
        return instance;
    }

    bool StartNetwork(Acore::Asio::IoContext& ioContext, std::string const& bindIp, uint16 port, int threadCount = 1) override
    {
        if (!BaseSocketMgr::StartNetwork(ioContext, bindIp, port, threadCount))
            return false;

        _acceptor->AsyncAcceptWithCallback<&AuthSocketMgr::OnSocketAccept>();
        return true;
    }

    void OnSocketOpen(IoContextTcpSocket&& sock, uint32 threadIndex) override
    {
        boost::system::error_code ec;
        auto endpoint = sock.remote_endpoint(ec);
        if (!ec)
        {
            if (sConnectionFloodProtection.ShouldRejectConnection(endpoint.address()))
            {
                LOG_WARN("network", "Connection flood protection: rejected connection from {}", endpoint.address().to_string());
                boost::system::error_code shutdownEc;
                sock.shutdown(boost::asio::socket_base::shutdown_both, shutdownEc);
                sock.close(shutdownEc);
                return;
            }
        }

        BaseSocketMgr::OnSocketOpen(std::move(sock), threadIndex);
    }

protected:
    NetworkThread<AuthSession>* CreateThreads() const override
    {
        AuthSocketThread* threads = new AuthSocketThread[1];

        bool proxyProtocolEnabled = sConfigMgr->GetOption<bool>("EnableProxyProtocol", false, true);
        if (proxyProtocolEnabled)
            threads[0].EnableProxyProtocol();

        return threads;
    }

    static void OnSocketAccept(IoContextTcpSocket&& sock, uint32 threadIndex)
    {
        Instance().OnSocketOpen(std::move(sock), threadIndex);
    }
};

#define sAuthSocketMgr AuthSocketMgr::Instance()

#endif // AuthSocketMgr_h__
