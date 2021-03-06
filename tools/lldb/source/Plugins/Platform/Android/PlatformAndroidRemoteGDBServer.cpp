//===-- PlatformAndroidRemoteGDBServer.cpp ----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// Other libraries and framework includes
#include "lldb/Core/Error.h"
#include "lldb/Core/Log.h"

// Project includes
#include "AdbClient.h"
#include "PlatformAndroidRemoteGDBServer.h"
#include "Utility/UriParser.h"

using namespace lldb;
using namespace lldb_private;
using namespace platform_android;

static const lldb::pid_t g_remote_platform_pid = 0; // Alias for the process id of lldb-platform

static Error
ForwardPortWithAdb (uint16_t port, std::string& device_id)
{
    Log *log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_PLATFORM));

    // Fetch the device list from ADB and if only 1 device found then use that device
    // TODO: Handle the case when more device is available
    AdbClient adb;
    auto error = AdbClient::CreateByDeviceID (nullptr, adb);
    if (error.Fail ())
        return error;

    device_id = adb.GetDeviceID ();
    if (log)
        log->Printf("Connected to Android device \"%s\"", device_id.c_str ());

    return adb.SetPortForwarding (port);
}

static Error
DeleteForwardPortWithAdb (uint16_t port, const std::string& device_id)
{
    AdbClient adb (device_id);
    return adb.DeletePortForwarding (port);
}

PlatformAndroidRemoteGDBServer::PlatformAndroidRemoteGDBServer ()
{
}

PlatformAndroidRemoteGDBServer::~PlatformAndroidRemoteGDBServer ()
{
    for (const auto& it : m_port_forwards)
    {
        DeleteForwardPortWithAdb (it.second.first, it.second.second);
    }
}

uint16_t
PlatformAndroidRemoteGDBServer::LaunchGDBserverAndGetPort (lldb::pid_t &pid)
{
    uint16_t port = m_gdb_client.LaunchGDBserverAndGetPort (pid, "127.0.0.1");
    if (port == 0)
        return port;

    std::string device_id;
    Error error = ForwardPortWithAdb (port, device_id);
    if (error.Fail ())
        return 0;

    m_port_forwards[pid] = std::make_pair (port, device_id);

    return port;
}

bool
PlatformAndroidRemoteGDBServer::KillSpawnedProcess (lldb::pid_t pid)
{
    DeleteForwardPort (pid);
    return m_gdb_client.KillSpawnedProcess (pid);
}

Error
PlatformAndroidRemoteGDBServer::ConnectRemote (Args& args)
{
    if (args.GetArgumentCount () != 1)
        return Error ("\"platform connect\" takes a single argument: <connect-url>");
  
    int port;
    std::string scheme, host, path;
    const char *url = args.GetArgumentAtIndex (0);
    if (!UriParser::Parse (url, scheme, host, port, path))
        return Error ("invalid uri");

    std::string device_id;
    Error error = ForwardPortWithAdb (port, device_id);
    if (error.Fail ())
        return error;

    m_port_forwards[g_remote_platform_pid] = std::make_pair (port, device_id);

    error = PlatformRemoteGDBServer::ConnectRemote (args);
    if (error.Fail ())
        DeleteForwardPort (g_remote_platform_pid);

    return error;
}

Error
PlatformAndroidRemoteGDBServer::DisconnectRemote ()
{
    DeleteForwardPort (g_remote_platform_pid);
    return PlatformRemoteGDBServer::DisconnectRemote ();
}

void
PlatformAndroidRemoteGDBServer::DeleteForwardPort (lldb::pid_t pid)
{
    Log *log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_PLATFORM));

    auto it = m_port_forwards.find (pid);
    if (it == m_port_forwards.end ())
        return;

    const auto& forward_val = it->second;
    const auto error = DeleteForwardPortWithAdb (forward_val.first, forward_val.second);
    if (error.Fail ()) {
        if (log)
            log->Printf ("Failed to delete port forwarding (pid=%" PRIu64 ", port=%d, device=%s): %s",
                         pid, forward_val.first, forward_val.second.c_str (), error.AsCString ());
    }
    m_port_forwards.erase (it);
}
