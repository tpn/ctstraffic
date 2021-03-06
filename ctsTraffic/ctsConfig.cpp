/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

// cpp headers
#include <vector>
#include <string>
#include <algorithm>

// OS headers
#include <windows.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <Mmsystem.h>

// ctl headers
#include <ctSockaddr.hpp>
#include <ctString.hpp>
#include <ctNetAdapterAddresses.hpp>
#include <ctTimer.hpp>
#include <ctRandom.hpp>

// local headers
#include "ctsConfig.h"
#include "ctsLogger.hpp"
#include "ctsIOPattern.h"
#include "ctsPrintStatus.hpp"

// local functors
#include "ctsConnectEx.hpp"
#include "ctsSimpleConnect.hpp"
#include "ctsWSASocket.hpp"
#include "ctsAcceptEx.hpp"
#include "ctsSimpleAccept.hpp"
#include "ctsSendRecvIocp.hpp"
#include "ctsReadWriteIocp.hpp"
#include "ctsrioiocp.hpp"
#include "ctsMediaStreamClient.hpp"
#include "ctsMediaStreamServer.hpp"


using namespace std;
using namespace ctl;


namespace ctsTraffic {
    namespace ctsConfig {

        ////////////////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Settings is being defined in this cpp - it was extern'd from ctsConfig.h
        ///
        ////////////////////////////////////////////////////////////////////////////////////////////////////
        ctsConfigSettings* Settings;

        ////////////////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Hiding the details of the raw data in an unnamed namespace to make it completely private
        /// Free functions below provide proper access to this information
        /// This design avoids having to pass a "config" object all over to share this information
        ///
        ////////////////////////////////////////////////////////////////////////////////////////////////////
        static CRITICAL_SECTION StatusUpdateLock;
        static CRITICAL_SECTION ShutdownLock;

        static const unsigned short DefaultPort = 4444;

        static const unsigned long long DefaultTransfer = 0x40000000; // 1Gbyte

        static const unsigned long DefaultBufferSize = 0x10000; // 64kbyte
        static const unsigned long DefaultAcceptLimit = 10;
        static const unsigned long DefaultAcceptExLimit = 100;
        static const unsigned long DefaultTcpConnectionLimit = 8;
        static const unsigned long DefaultUdpConnectionLimit = 1;
        static const unsigned long DefaultConnectionThrottleLimit = 1000;
        static const unsigned long DefaultThreadpoolFactor = 2;

        static PTP_POOL ptp_pool = nullptr;
        static TP_CALLBACK_ENVIRON tp_environment;
        static unsigned long tp_thread_count = 0;

        static const wchar_t* CreateFunctionName = nullptr;
        static const wchar_t* ConnectFunctionName = nullptr;
        static const wchar_t* AcceptFunctionName = nullptr;
        static const wchar_t* IoFunctionName = nullptr;

        // connection info + error info
        static unsigned long verbosity = 4;
        static unsigned long buffersize_low = 0;
        static unsigned long buffersize_high = 0;
        static long long ratelimit_low = 0;
        static long long ratelimit_high = 0;
        static unsigned long long transfer_low = DefaultTransfer;
        static unsigned long long transfer_high = 0;

        static const unsigned long DefaultPushBytes = 0x100000;
        static const unsigned long DefaultPullBytes = 0x100000;

        static ctsUnsignedLong timer_changed_count = 0;

        static ctsSignedLongLong printing_previous_timeslice;
        static ctsSignedLongLong printing_timeslice_count;

        static NET_IF_COMPARTMENT_ID compartment_id = NET_IF_COMPARTMENT_ID_UNSPECIFIED;
        static ctNetAdapterAddresses* netAdapterAddresses = nullptr;

        static MediaStreamSettings media_stream_settings;
        static ctRandomTwister random;

        // default to 5 seconds
        static const unsigned long DefaultStatusUpdateFrequency = 5000;
        static std::shared_ptr<ctsStatusInformation> print_status;
        static std::shared_ptr<ctsLogger> connectionlogger;
        static std::shared_ptr<ctsLogger> statuslogger;
        static std::shared_ptr<ctsLogger> errorlogger;
        static std::shared_ptr<ctsLogger> jitterlogger;

        static bool break_on_error = false;
        static bool shutdown_called = false;


        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Singleton values used as the actual implementation for every 'connection'
        ///
        /// publicly exposed callers invoke ::InitOnceExecuteOnce(&InitImpl, InitOncectsConfigImpl, NULL, NULL);
        ///
        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        static INIT_ONCE InitImpl = INIT_ONCE_STATIC_INIT;
        static
        BOOL CALLBACK InitOncectsConfigImpl(PINIT_ONCE, PVOID, PVOID *)
        {
            if (!::InitializeCriticalSectionEx(&StatusUpdateLock, 4000, 0)) {
                ctl::ctAlwaysFatalCondition(L"InitializeCriticalSectionEx failed: %u", ::GetLastError());
            }
            if (!::InitializeCriticalSectionEx(&ShutdownLock, 4000, 0)) {
                ctl::ctAlwaysFatalCondition(L"InitializeCriticalSectionEx failed: %u", ::GetLastError());
            }

            Settings = new ctsConfigSettings;
            Settings->Port = DefaultPort;
            Settings->SocketFlags = WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT;
            Settings->Iterations = MAXULONGLONG;
            Settings->ConnectionLimit = 1;
            Settings->AcceptLimit = DefaultAcceptLimit;
            Settings->ConnectionThrottleLimit = DefaultConnectionThrottleLimit;
            Settings->ServerExitLimit = MAXULONGLONG;
            Settings->StatusUpdateFrequencyMilliseconds = DefaultStatusUpdateFrequency;
            // defaulting to verifying - therefore not using a shared buffer
            Settings->ShouldVerifyBuffers = true;
            Settings->UseSharedBuffer = false;

            printing_previous_timeslice = 0LL;
            printing_timeslice_count = 0LL;

            return TRUE;
        }
        static
        void ctsConfigInitOnce() throw()
        {
            ctFatalCondition(
                !::InitOnceExecuteOnce(&InitImpl, InitOncectsConfigImpl, NULL, NULL),
                L"ctsConfig InitOnceExecuteOnce failed: %lu", ::GetLastError());
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// parses the input argument to determine if it matches the expected parameter
        /// if so, it returns a ptr to the corresponding parameter value
        /// otherwise, returns nullptr
        ///
        /// throws invalid_parameter if something is obviously wrong 
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        wchar_t* ParseArgument(_In_z_ wchar_t* _input_argument, _In_z_ wchar_t* _expected_param)
        {
            wchar_t* param_end = _input_argument + wcslen(_input_argument);
            wchar_t* param_delimiter = find(_input_argument, param_end, L':');
            if (!(param_end > param_delimiter + 1)) {
                throw invalid_argument("Invalid argument");
            }
            // temporarily null-terminate it at the delimiter to do a string compare
            *(param_delimiter) = L'\0';
            wchar_t* return_value = nullptr;
            if (ctString::iordinal_equals(_expected_param, _input_argument)) {
                return_value = param_delimiter + 1;
            }
            *(param_delimiter) = L':';
            return return_value;
        }

        ////////////////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// as_integral(wstring)
        ///
        /// Directly converts the *entire* contents of the passed in string to a numeric value
        /// - the type of that numeric value being the template type specified
        ///
        /// e.g.
        /// long a = as_integral<long>(L"-1");
        /// long b = as_integral<unsigned long>(L"0xa");
        /// long a = as_integral<long long>(L"0x123456789abcdef");
        /// long a = as_integral<unsigned long long>(L"999999999999999999");
        /// 
        /// NOTE:
        /// - will *only* assume a string starting with "0x" to be converted as hexadecimal
        ///   if does not start with "0x", will assume as base-10
        /// - if an unsigned type is specified in the template and a negative number is entered,
        ///   will convert that to the "unsigned" version of that set of bits
        ///   e.g.
        ///       unsigned long long test = as_integral<unsigned long long>(L"-1");
        ///       // test == 0xffffffffffffffff
        ///
        ///  TODO: need to revisit the above policy of allowing implicit negative -> unsigned conversions
        ///
        ////////////////////////////////////////////////////////////////////////////////////////////////////
        template <typename T>
        T as_integral(const std::wstring& _string);
        /// LONG and ULONG
        template <>
        long as_integral<long>(const std::wstring& _string)
        {
            long return_value;
            size_t first_unconverted_offset = 0;
            if (_string.find(L'x') != wstring::npos || _string.find(L'X') != wstring::npos) {
                return_value = std::stol(_string, &first_unconverted_offset, 16);
            } else {
                return_value = std::stol(_string, &first_unconverted_offset, 10);
            }

            if (first_unconverted_offset != _string.length()) {
                throw invalid_argument(
                    ctString::convert_to_string(
                        ctString::format_string(
                            L"Invalid argument: %s", _string.c_str())).c_str());
            }
            return return_value;
        }
        template <>
        unsigned long as_integral<unsigned long>(const std::wstring& _string)
        {
            unsigned long return_value;
            size_t first_unconverted_offset = 0;
            if (_string.find(L'x') != wstring::npos || _string.find(L'X') != wstring::npos) {
                return_value = std::stoul(_string, &first_unconverted_offset, 16);
            } else {
                return_value = std::stoul(_string, &first_unconverted_offset, 10);
            }

            if (first_unconverted_offset != _string.length()) {
                throw invalid_argument(
                    ctString::convert_to_string(
                        ctString::format_string(
                            L"Invalid argument: %s", _string.c_str())).c_str());
            }
            return return_value;
        }
        /// INT and UINT
        template <>
        int as_integral<int>(const std::wstring& _string)
        {
            return as_integral<long>(_string);
        }
        template <>
        unsigned int as_integral<unsigned int>(const std::wstring& _string)
        {
            return as_integral<unsigned long>(_string);
        }
        /// SHORT and USHORT
        template <>
        short as_integral<short>(const std::wstring& _string)
        {
            long return_value = as_integral<long>(_string);
            if (return_value > MAXSHORT || return_value < MINSHORT) {
                throw invalid_argument(
                    ctString::convert_to_string(
                        ctString::format_string(
                            L"Invalid argument: %s", _string.c_str())).c_str());
            }
            return static_cast<short>(return_value);
        }
        template <>
        unsigned short as_integral<unsigned short>(const std::wstring& _string)
        {
            unsigned long return_value = as_integral<unsigned long>(_string);
            // MAXWORD == MAXUSHORT
            if (return_value > MAXWORD) {
                throw invalid_argument(
                    ctString::convert_to_string(
                        ctString::format_string(
                            L"Invalid argument: %s", _string.c_str())).c_str());
            }
            return static_cast<unsigned short>(return_value);
        }
        /// LONGLONG and ULONGLONG
        template <>
        long long as_integral<long long>(const std::wstring& _string)
        {
            long long return_value;
            size_t first_unconverted_offset = 0;
            if (_string.find(L'x') != wstring::npos || _string.find(L'X') != wstring::npos) {
                return_value = std::stoll(_string, &first_unconverted_offset, 16);
            } else {
                return_value = std::stoll(_string, &first_unconverted_offset, 10);
            }

            if (first_unconverted_offset != _string.length()) {
                throw invalid_argument(
                    ctString::convert_to_string(
                        ctString::format_string(
                            L"Invalid argument: %s", _string.c_str())).c_str());
            }
            return return_value;
        }
        template <>
        unsigned long long as_integral<unsigned long long>(const std::wstring& _string)
        {
            unsigned long long return_value;
            size_t first_unconverted_offset = 0;
            if (_string.find(L'x') != wstring::npos || _string.find(L'X') != wstring::npos) {
                return_value = std::stoull(_string, &first_unconverted_offset, 16);
            } else {
                return_value = std::stoull(_string, &first_unconverted_offset, 10);
            }

            if (first_unconverted_offset != _string.length()) {
                throw invalid_argument(
                    ctString::convert_to_string(
                        ctString::format_string(
                            L"Invalid argument: %s", _string.c_str())).c_str());
            }
            return return_value;
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the connect function to use
        ///
        /// --conn:connect
        /// --conn:wsaconnect
        /// --conn:wsaconnectbyname
        /// --conn:connectex  (*default)
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_create(vector<wchar_t*>& _args)
        {
            UNREFERENCED_PARAMETER(_args);
            if (nullptr == Settings->CreateFunction) {
                Settings->CreateFunction = ctsWSASocket;
                CreateFunctionName = L"WSASocket";
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the connect function to use
        ///
        /// --conn:connect
        /// --conn:wsaconnect
        /// --conn:wsaconnectbyname
        /// --conn:connectex  (*default)
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_connect(vector<wchar_t*>& _args)
        {
            bool connect_specifed = false;
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"--conn");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (Settings->Protocol != ctsConfig::ProtocolType::TCP) {
                    throw invalid_argument("--conn (only applicable to TCP)");
                }

                wchar_t* value = ParseArgument(*found_arg, L"--conn");
                if (ctString::iordinal_equals(L"ConnectEx", value)) {
                    Settings->ConnectFunction = ctsConnectEx;
                    ConnectFunctionName = L"ConnectEx";
                } else if (ctString::iordinal_equals(L"connect", value)) {
                    Settings->ConnectFunction = ctsSimpleConnect;
                    ConnectFunctionName = L"connect";
                } else {
                    throw invalid_argument("--conn");
                }
                connect_specifed = true;
                // always remove the arg from our vector
                _args.erase(found_arg);

            } else {
                if (Settings->IoPattern != IoPatternType::MediaStream) {
                    Settings->ConnectFunction = ctsConnectEx;
                    ConnectFunctionName = L"ConnectEx";
                } else {
                    Settings->ConnectFunction = ctsMediaStreamClientConnect;
                    ConnectFunctionName = L"MediaStream Client Connect";
                }
            }

            if (IoPatternType::MediaStream == Settings->IoPattern && connect_specifed) {
                throw invalid_argument("-conn (MediaStream has its own internal connection handler)");
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the accept function to use
        ///
        /// --acc:accept
        /// --acc:wsaaccept
        /// --acc:acceptex  (*default)
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_accept(vector<wchar_t*>& _args)
        {
            Settings->AcceptLimit = DefaultAcceptExLimit;

            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"--acc");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (Settings->Protocol != ctsConfig::ProtocolType::TCP) {
                    throw invalid_argument("--acc (only applicable to TCP)");
                }

                wchar_t* value = ParseArgument(*found_arg, L"--acc");
                if (ctString::iordinal_equals(L"accept", value)) {
                    Settings->AcceptFunction = ctsSimpleAccept();
                    AcceptFunctionName = L"accept";
                } else if (ctString::iordinal_equals(L"AcceptEx", value)) {
                    Settings->AcceptFunction = ctsAcceptEx();
                    AcceptFunctionName = L"AcceptEx";
                } else {
                    throw invalid_argument("--acc");
                }
                // always remove the arg from our vector
                _args.erase(found_arg);

            } else if (Settings->ListenAddresses.size() > 0) {
                if (IoPatternType::MediaStream != Settings->IoPattern) {
                    // only default an Accept function if listening
                    Settings->AcceptFunction = ctsAcceptEx();
                    AcceptFunctionName = L"AcceptEx";
                } else {
                    Settings->AcceptFunction = ctsMediaStreamServerListener;
                    AcceptFunctionName = L"MediaStream Server Listener";
                }
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the IO (read/write) function to use
        /// -- only applicable to TCP
        ///
        /// -io:blocking
        /// -io:nonblocking
        /// -io:event
        /// -io:iocp (*default)
        /// -io:wsapoll
        /// -io:rioiocp
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_ioFunction(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-io");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (Settings->Protocol != ctsConfig::ProtocolType::TCP) {
                    throw invalid_argument("-io (only applicable to TCP)");
                }

                wchar_t* value = ParseArgument(*found_arg, L"-io");
                if (ctString::iordinal_equals(L"iocp", value)) {
                    Settings->IoFunction = ctsSendRecvIocp;
                    Settings->Options |= OptionType::HANDLE_INLINE_IOCP;
                    IoFunctionName = L"iocp (WSASend/WSARecv using IOCP)";

                } else if (ctString::iordinal_equals(L"readwritefile", value)) {
                    Settings->IoFunction = ctsReadWriteIocp;
                    IoFunctionName = L"readwritefile (ReadFile/WriteFile using IOCP)";

                } else if (ctString::iordinal_equals(L"rioiocp", value)) {
                    Settings->IoFunction = ctsRioIocp;
                    Settings->SocketFlags |= WSA_FLAG_REGISTERED_IO;
                    IoFunctionName = L"RioIocp (RIO using IOCP notifications)";

                } else {
                    throw invalid_argument("-io");
                }
                // always remove the arg from our vector
                _args.erase(found_arg);

            } else {
                if (ProtocolType::TCP == Settings->Protocol) {
                    // Default for TCP is WSASend/WSARecv using IOCP
                    Settings->IoFunction = ctsSendRecvIocp;
                    Settings->Options |= OptionType::HANDLE_INLINE_IOCP;
                    IoFunctionName = L"iocp (WSASend/WSARecv using IOCP)";

                } else {
                    // UDP only has one IOFunction: media streaming
                    if (IsListening()) {
                        Settings->IoFunction = ctsMediaStreamServerIo;
                        IoFunctionName = L"MediaStream Server";
                    } else {
                        Settings->IoFunction = ctsMediaStreamClient;
                        Settings->Options |= OptionType::MAX_RECV_BUF;
                        Settings->Options |= OptionType::HANDLE_INLINE_IOCP;
                        IoFunctionName = L"MediaStream Client";
                    }
                }
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the L4 Protocol to limit to usage
        ///
        /// -Protocol:tcp
        /// -Protocol:udp
        /// -Protocol:raw
        /// -Protocol:multicast
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_protocol(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-Protocol");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                wchar_t* value = ParseArgument(*found_arg, L"-Protocol");
                if (ctString::iordinal_equals(L"tcp", value)) {
                    Settings->Protocol = ProtocolType::TCP;
                } else if (ctString::iordinal_equals(L"udp", value)) {
                    Settings->Protocol = ProtocolType::UDP;
                } else {
                    throw invalid_argument("-Protocol");
                }
                // always remove the arg from our vector
                _args.erase(found_arg);
            } else {
                // default to TCP
                Settings->Protocol = ProtocolType::TCP;
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for socket Options
        /// - allows for more than one option to be set
        /// -Options:<keepalive,tcpfastpath,phonesubappdata> [-Options:<...>] [-Options:<...>]
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_options(vector<wchar_t*>& _args)
        {
            for (;;) {
                // loop until cannot fine -Options
                auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                    wchar_t* value = ParseArgument(parameter, L"-Options");
                    return (value != nullptr);
                });

                if (found_arg != end(_args)) {
                    wchar_t* value = ParseArgument(*found_arg, L"-Options");
                    if (ctString::iordinal_equals(L"keepalive", value)) {
                        if (ProtocolType::TCP == Settings->Protocol) {
                            Settings->Options |= OptionType::KEEPALIVE;
                        } else {
                            throw invalid_argument("-Options (keepalive only allowed with TCP sockets)");
                        }

                    } else if (ctString::iordinal_equals(L"tcpfastpath", value)) {
                        if (ProtocolType::TCP == Settings->Protocol) {
                            Settings->Options |= OptionType::LOOPBACK_FAST_PATH;
                        } else {
                            throw invalid_argument("-Options (tcpfastpath only allowed with TCP sockets)");
                        }

                    } else {
                        throw invalid_argument("-Options");
                    }

                    // always remove the arg from our vector
                    _args.erase(found_arg);
                } else {
                    // didn't find -Options
                    break;
                }
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the wire-Protocol to use
        /// --- these only apply to TCP
        ///
        /// -pattern:push
        /// -pattern:pull
        /// -pattern:pushpull
        /// -pattern:duplex
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_ioPattern(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-pattern");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (Settings->Protocol != ctsConfig::ProtocolType::TCP) {
                    throw invalid_argument("-pattern (only applicable to TCP)");
                }

                wchar_t* value = ParseArgument(*found_arg, L"-pattern");
                if (ctString::iordinal_equals(L"push", value)) {
                    Settings->IoPattern = IoPatternType::Push;

                } else if (ctString::iordinal_equals(L"pull", value)) {
                    Settings->IoPattern = IoPatternType::Pull;

                } else if (ctString::iordinal_equals(L"pushpull", value)) {
                    Settings->IoPattern = IoPatternType::PushPull;

                } else if (ctString::iordinal_equals(L"flood", value) || ctString::iordinal_equals(L"duplex", value)) {
                    // the old name for this was 'flood'
                    Settings->IoPattern = IoPatternType::Duplex;

                } else {
                    throw invalid_argument("-pattern");
                }

                // always remove the arg from our vector
                _args.erase(found_arg);

            } else {
                if (Settings->Protocol == ProtocolType::UDP) {
                    Settings->IoPattern = IoPatternType::MediaStream;
                } else {
                    // default the TCP pattern to Push
                    Settings->IoPattern = IoPatternType::Push;
                }
            }

            // Now look for options tightly coupled to Protocol
            auto found_pushbytes = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-pushbytes");
                return (value != nullptr);
            });
            if (found_pushbytes != end(_args)) {
                if (Settings->IoPattern != IoPatternType::PushPull) {
                    throw invalid_argument("-PushBytes can only be set with -Pattern:PushPull");
                }
                Settings->PushBytes = as_integral<unsigned long>(ParseArgument(*found_pushbytes, L"-pushbytes"));
                // always remove the arg from our vector
                _args.erase(found_pushbytes);
            } else {
                Settings->PushBytes = DefaultPushBytes;
            }

            auto found_pullbytes = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-pullbytes");
                return (value != nullptr);
            });
            if (found_pullbytes != end(_args)) {
                if (Settings->IoPattern != IoPatternType::PushPull) {
                    throw invalid_argument("-PullBytes can only be set with -Pattern:PushPull");
                }
                Settings->PullBytes = as_integral<unsigned long>(ParseArgument(*found_pullbytes, L"-pullbytes"));
                // always remove the arg from our vector
                _args.erase(found_pullbytes);
            } else {
                Settings->PullBytes = DefaultPullBytes;
            }

            //
            // Options for the UDP protocol
            //

            found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-BitsPerSecond");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (Settings->Protocol != ProtocolType::UDP) {
                    throw invalid_argument("-BitsPerSecond requires -Protocol:UDP");
                }
                media_stream_settings.BitsPerSecond = as_integral<long long>(ParseArgument(*found_arg, L"-BitsPerSecond"));
                // bitspersecond must align on a byte-boundary
                if (media_stream_settings.BitsPerSecond % 8 != 0) {
                    media_stream_settings.BitsPerSecond -= media_stream_settings.BitsPerSecond % 8;
                }
                // always remove the arg from our vector
                _args.erase(found_arg);
            }

            found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-FrameRate");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (Settings->Protocol != ProtocolType::UDP) {
                    throw invalid_argument("-FrameRate requires -Protocol:UDP");
                }
                media_stream_settings.FramesPerSecond = as_integral<unsigned long>(ParseArgument(*found_arg, L"-FrameRate"));
                // always remove the arg from our vector
                _args.erase(found_arg);
            }

            found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-BufferDepth");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (Settings->Protocol != ProtocolType::UDP) {
                    throw invalid_argument("-BufferDepth requires -Protocol:UDP");
                }
                media_stream_settings.BufferDepthSeconds = as_integral<unsigned long>(ParseArgument(*found_arg, L"-BufferDepth"));
                // always remove the arg from our vector
                _args.erase(found_arg);
            }

            found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-StreamLength");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (Settings->Protocol != ProtocolType::UDP) {
                    throw invalid_argument("-StreamLength requires -Protocol:UDP");
                }
                media_stream_settings.StreamLengthSeconds = as_integral<unsigned long>(ParseArgument(*found_arg, L"-StreamLength"));
                // always remove the arg from our vector
                _args.erase(found_arg);
            }

            found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-StreamCodec");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (Settings->Protocol != ProtocolType::UDP) {
                    throw invalid_argument("-StreamCodec requires -Protocol:UDP");
                }
                auto codec = ParseArgument(*found_arg, L"-StreamCodec");
                if (ctString::iordinal_equals(L"NoResends", codec)) {
                    media_stream_settings.StreamCodec = MediaStreamSettings::StreamCodecValues::NoResends;

                } else if (ctString::iordinal_equals(L"ResendOnce", codec)) {
                    media_stream_settings.StreamCodec = MediaStreamSettings::StreamCodecValues::ResendOnce;

                } else {
                    throw invalid_argument("-StreamCodec");
                }

                // always remove the arg from our vector
                _args.erase(found_arg);
            }

            // validate and resolve the UDP protocol options
            if (ProtocolType::UDP == Settings->Protocol) {
                if (0 == media_stream_settings.BitsPerSecond) {
                    throw invalid_argument("-BitsPerSecond is required");
                }
                if (0 == media_stream_settings.FramesPerSecond) {
                    throw invalid_argument("-FrameRate is required");
                }
                // BufferDepth is only required on client
                if (!IsListening() && 0 == media_stream_settings.BufferDepthSeconds) {
                    throw invalid_argument("-BufferDepth is required");
                }
                if (0 == media_stream_settings.StreamLengthSeconds) {
                    throw invalid_argument("-StreamLength is required");
                }

                // finally calculate the total stream length after all settings are captured from the user
                transfer_low = media_stream_settings.CalculateTransferSize();
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for IP address or machine name target to use
        /// Can be comma-delimited if more than one
        ///
        /// 3 different parameters read address/name settings:
        /// Supports specifying the parameter multiple times:
        ///   e.g. -target:machinea -target:machineb
        ///
        /// -listen: (address to listen on)
        ///   - specifying * == listen to all addresses
        /// -target: (address to connect to)
        /// -bind:   (address to bind before connecting)
        ///   - specifying * == bind to all addresses (default)
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_address(vector<wchar_t*>& _args)
        {
            // -listen:<addr> 
            auto found_listen = begin(_args);
            while (found_listen != end(_args)) {
                found_listen = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                    wchar_t* value = ParseArgument(parameter, L"-listen");
                    return (value != nullptr);
                });
                if (found_listen != end(_args)) {
                    // default to keep-alive on TCP servers
                    if (ProtocolType::TCP == Settings->Protocol) {
                        Settings->Options |= OptionType::KEEPALIVE;
                    }
                    wchar_t* value = ParseArgument(*found_listen, L"-listen");
                    if (ctString::iordinal_equals(L"*", value)) {
                        // add both v4 and v6
                        ctSockaddr listen_addr(AF_INET);
                        listen_addr.setAddressAny();
                        Settings->ListenAddresses.push_back(listen_addr);
                        listen_addr.reset(AF_INET6);
                        listen_addr.setAddressAny();
                        Settings->ListenAddresses.push_back(listen_addr);
                    } else {
                        vector<ctSockaddr> temp_addresses(ctSockaddr::ResolveName(value));
                        if (temp_addresses.empty()) {
                            throw invalid_argument("-listen value did not resolve to an IP address");
                        }
                        Settings->ListenAddresses.insert(end(Settings->ListenAddresses), begin(temp_addresses), end(temp_addresses));
                    }
                    // always remove the arg from our vector
                    _args.erase(found_listen);
                    // found_listen is now invalidated since we just erased what it's pointing to
                    // - reset it to begin() since we know it's not end()
                    found_listen = _args.begin();
                }
            }

            // -target:<addr> 
            auto found_target = begin(_args);
            while (found_target != end(_args)) {
                found_target = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                    wchar_t* value = ParseArgument(parameter, L"-target");
                    return (value != nullptr);
                });
                if (found_target != end(_args)) {
                    if (!Settings->ListenAddresses.empty()) {
                        throw invalid_argument("cannot specify both -Listen and -Target");
                    }
                    wchar_t* value = ParseArgument(*found_target, L"-target");
                    vector<ctSockaddr> temp_addresses(ctSockaddr::ResolveName(value));
                    if (temp_addresses.empty()) {
                        throw invalid_argument("-target value did not resolve to an IP address");
                    }
                    Settings->TargetAddresses.insert(end(Settings->TargetAddresses), begin(temp_addresses), end(temp_addresses));
                    // always remove the arg from our vector
                    _args.erase(found_target);
                    // found_target is now invalidated since we just erased what it's pointing to
                    // - reset it to begin() since we know it's not end()
                    found_target = _args.begin();
                }
            }
            // -bind:<addr> 
            auto found_bind = begin(_args);
            while (found_bind != end(_args)) {
                found_bind = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                    wchar_t* value = ParseArgument(parameter, L"-bind");
                    return (value != nullptr);
                });
                if (found_bind != end(_args)) {
                    wchar_t* value = ParseArgument(*found_bind, L"-bind");
                    // check for a comma-delimited list of IP Addresses
                    if (ctString::iordinal_equals(L"*", value)) {
                        // add both v4 and v6
                        ctSockaddr bind_addr(AF_INET);
                        bind_addr.setAddressAny();
                        Settings->BindAddresses.push_back(bind_addr);
                        bind_addr.reset(AF_INET6);
                        bind_addr.setAddressAny();
                        Settings->BindAddresses.push_back(bind_addr);
                    } else {
                        vector<ctSockaddr> temp_addresses(ctSockaddr::ResolveName(value));
                        if (temp_addresses.empty()) {
                            throw invalid_argument("-bind value did not resolve to an IP address");
                        }
                        Settings->BindAddresses.insert(end(Settings->BindAddresses), begin(temp_addresses), end(temp_addresses));
                    }
                    // always remove the arg from our vector
                    _args.erase(found_bind);
                    // found_bind is now invalidated since we just erased what it's pointing to
                    // - reset it to begin() since we know it's not end()
                    found_bind = _args.begin();
                }
            }
            if ((Settings->ListenAddresses.size() > 0) && (Settings->TargetAddresses.size() > 0)) {
                throw invalid_argument("cannot specify both -target and -listen");
            }
            if ((Settings->ListenAddresses.size() > 0) && (Settings->BindAddresses.size() > 0)) {
                throw invalid_argument("cannot specify both -bind and -listen");
            }
            if ((Settings->ListenAddresses.size() == 0) && (Settings->TargetAddresses.size() == 0)) {
                throw invalid_argument("must specify either -target or -listen");
            }

            // default bind addresses if not listening and did not exclusively want to bind
            if ((Settings->ListenAddresses.size() == 0) && (Settings->BindAddresses.size() == 0)) {
                ctSockaddr defaultAddr(AF_INET);
                defaultAddr.setAddressAny();
                Settings->BindAddresses.push_back(defaultAddr);
                defaultAddr.reset(AF_INET6);
                defaultAddr.setAddressAny();
                Settings->BindAddresses.push_back(defaultAddr);
            }

            if (Settings->TargetAddresses.size() > 0) {
                //
                // guarantee that bindaddress and targetaddress families can match
                // - can't allow a bind address to be chosen if there are no TargetAddresses with the same family
                //
                ctsUnsignedLong bind_v4 = 0;
                ctsUnsignedLong bind_v6 = 0;
                ctsUnsignedLong target_v4 = 0;
                ctsUnsignedLong target_v6 = 0;
                for (const auto& addr : Settings->BindAddresses) {
                    if (addr.family() == AF_INET) {
                        ++bind_v4;
                    } else {
                        ++bind_v6;
                    }
                }
                for (const auto& addr : Settings->TargetAddresses) {
                    if (addr.family() == AF_INET) {
                        ++target_v4;
                    } else {
                        ++target_v6;
                    }
                }
                //
                // if either bind or target has zero of either family, remove those addrs from the other vector
                //
                if (0 == bind_v4) {
                    Settings->TargetAddresses.erase(
                        remove_if(
                            begin(Settings->TargetAddresses),
                            end(Settings->TargetAddresses),
                            [&] (ctSockaddr& addr) -> bool { return addr.family() == AF_INET; }),
                        Settings->TargetAddresses.end()
                    );
                } else if (0 == target_v4) {
                    Settings->BindAddresses.erase(
                        remove_if(
                            begin(Settings->BindAddresses),
                            end(Settings->BindAddresses),
                            [&] (ctSockaddr& addr) -> bool { return addr.family() == AF_INET; }),
                        Settings->BindAddresses.end()
                    );
                }

                if (0 == bind_v6) {
                    Settings->TargetAddresses.erase(
                        remove_if(
                            begin(Settings->TargetAddresses),
                            end(Settings->TargetAddresses),
                            [&] (ctSockaddr& addr) -> bool { return addr.family() == AF_INET6; }),
                        Settings->TargetAddresses.end()
                    );
                } else if (0 == target_v6) {
                    Settings->BindAddresses.erase(
                        remove_if(
                            begin(Settings->BindAddresses),
                            end(Settings->BindAddresses),
                            [&] (ctSockaddr& addr) -> bool { return addr.family() == AF_INET6; }),
                        Settings->BindAddresses.end()
                    );
                }
                //
                // now if either are of size zero, the user specified addresses which didn't align
                //
                if (Settings->BindAddresses.empty() || Settings->TargetAddresses.empty()) {
                    throw exception("Invalid input: bind addresses and target addresses must match families");
                }
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the Port # to listen to/connect to
        ///
        /// -Port:##
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_port(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-Port");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                Settings->Port = as_integral<unsigned short>(ParseArgument(*found_arg, L"-Port"));
                if (0 == Settings->Port) {
                    throw invalid_argument("-Port");
                }
                // always remove the arg from our vector
                _args.erase(found_arg);
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the connection limit [max number of connections to maintain]
        ///
        /// -connections:####
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_connections(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-connections");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (IsListening()) {
                    throw invalid_argument("-Connections is only supported when running as a client");
                }
                Settings->ConnectionLimit = as_integral<unsigned long>(ParseArgument(*found_arg, L"-connections"));
                if (0 == Settings->ConnectionLimit) {
                    throw invalid_argument("-connections");
                }
                // always remove the arg from our vector
                _args.erase(found_arg);
            }
        }
        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the server limit [max number of connections before the server exits]
        ///
        /// -ServerExitLimit:####
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_serverExitLimit(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-ServerExitLimit");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (!IsListening()) {
                    throw invalid_argument("-ServerExitLimit is only supported when running as a client");
                }
                Settings->ServerExitLimit = as_integral<ULONGLONG>(ParseArgument(*found_arg, L"-ServerExitLimit"));
                if (0 == Settings->ServerExitLimit) {
                    // zero indicates no exit
                    Settings->ServerExitLimit = MAXULONGLONG;
                }
                // always remove the arg from our vector
                _args.erase(found_arg);
            }
        }
        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the connection limit [max number of connections to maintain]
        ///
        /// -throttleconnections:####
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_throttleConnections(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-throttleconnections");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (IsListening()) {
                    throw invalid_argument("-ThrottleConnections is only supported when running as a client");
                }
                Settings->ConnectionThrottleLimit = as_integral<unsigned long>(ParseArgument(*found_arg, L"-throttleconnections"));
                if (0 == Settings->ConnectionThrottleLimit) {
                    // zero means no limit
                    Settings->ConnectionThrottleLimit = MAXUINT32;
                }
                // always remove the arg from our vector
                _args.erase(found_arg);
            }
        }

        template <typename T>
        void get_range(_In_z_ wchar_t* _value, T& _out_low, T& _out_high)
        {
            // a range was specified
            // - find the ',' the '[', and the ']'
            size_t value_length = ::wcslen(_value);
            wchar_t* value_end = _value + value_length;
            if ((value_length < 5) || (_value[0] != L'[') || (_value[value_length - 1] != L']')) {
                throw invalid_argument("range value [###,###]");
            }
            wchar_t* comma_delimiter = find(_value, value_end, L',');
            if (!(value_end > comma_delimiter + 1)) {
                throw invalid_argument("range value [###,###]");
            }

            // null-terminate the first number at the delimiter to do a string -> int conversion
            *(comma_delimiter) = L'\0';
            wchar_t* value_low = _value + 1; // move past the '['
            _out_low = as_integral<T>(value_low);

            // null-terminate for the 2nd number over the last ']' to doa string -> int conversion
            _value[value_length - 1] = L'\0';
            wchar_t* value_high = comma_delimiter + 1;
            _out_high = as_integral<T>(value_high);

            // validate buffer values
            if (_out_high < _out_low) {
                throw invalid_argument("range value [###,###]");
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the buffer size to push down per IO
        ///
        /// -buffer:####
        ///        :[low,high]
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_buffer(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-buffer");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (Settings->Protocol != ctsConfig::ProtocolType::TCP) {
                    throw invalid_argument("-buffer (only applicable to TCP)");
                }

                wchar_t* value = ParseArgument(*found_arg, L"-buffer");
                if (value[0] == L'[') {
                    get_range(value, buffersize_low, buffersize_high);
                } else {
                    // singe values are written to buffersize_low, with buffersize_high left at zero
                    buffersize_low = as_integral<unsigned long>(value);
                }
                if (0 == buffersize_low) {
                    throw invalid_argument("-buffer");
                }

                // always remove the arg from our vector
                _args.erase(found_arg);
            } else {
                buffersize_low = DefaultBufferSize;
                buffersize_high = 0;
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the total transfer size in bytes per connection
        ///
        /// -transfer:####
        ///          :[low,high]
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_transfer(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-transfer");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (Settings->Protocol != ctsConfig::ProtocolType::TCP) {
                    throw invalid_argument("-transfer (only applicable to TCP)");
                }

                wchar_t* value = ParseArgument(*found_arg, L"-transfer");
                if (value[0] == L'[') {
                    get_range(value, transfer_low, transfer_high);
                } else {
                    // singe values are written to transfer_low, with transfer_high left at zero
                    transfer_low = as_integral<unsigned long long>(value);
                }
                if (0 == transfer_low) {
                    throw invalid_argument("-transfer");
                }

                // always remove the arg from our vector
                _args.erase(found_arg);
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the LocalPort # to bind for local connect
        /// 
        /// -LocalPort:##
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_localport(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-LocalPort");
                return (value != nullptr);
            });

            if (found_arg != end(_args)) {
                wchar_t* value = ParseArgument(*found_arg, L"-LocalPort");
                if (value[0] == L'[') {
                    get_range(value, Settings->LocalPortLow, Settings->LocalPortHigh);
                } else {
                    // single value are written to localport_low with localport_high left at zero
                    Settings->LocalPortLow = as_integral<USHORT>(value);
                }
                if (0 == Settings->LocalPortLow) {
                    throw invalid_argument("-LocalPort");
                }

                // always remove the arg from our vector
                _args.erase(found_arg);
            }
        }


        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for Tcp throttling parameters
        ///
        /// -RateLimit:####
        ///           :[low,high]
        /// -RateLimitPeriod:####
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_ratelimit(vector<wchar_t*>& _args)
        {
            auto found_ratelimit = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-RateLimit");
                return (value != nullptr);
            });
            if (found_ratelimit != end(_args)) {
                if (Settings->Protocol != ctsConfig::ProtocolType::TCP) {
                    throw invalid_argument("-RateLimit (only applicable to TCP)");
                }
                wchar_t* value = ParseArgument(*found_ratelimit, L"-RateLimit");
                if (value[0] == L'[') {
                    get_range(value, ratelimit_low, ratelimit_low);
                } else {
                    // singe values are written to buffersize_low, with buffersize_high left at zero
                    ratelimit_low = as_integral<long long>(ParseArgument(*found_ratelimit, L"-RateLimit"));
                }
                if (0LL == ratelimit_low) {
                    throw invalid_argument("-RateLimit");
                }
                // always remove the arg from our vector
                _args.erase(found_ratelimit);
            }

            auto found_ratelimit_period = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-RateLimitPeriod");
                return (value != nullptr);
            });
            if (found_ratelimit_period != end(_args)) {
                if (Settings->Protocol != ctsConfig::ProtocolType::TCP) {
                    throw invalid_argument("-RateLimitPeriod (only applicable to TCP)");
                }
                if (0LL == ratelimit_low) {
                    throw invalid_argument("-RateLimitPeriod requires specifying -RateLimit");
                }
                Settings->TcpBytesPerSecondPeriod = as_integral<long long>(ParseArgument(*found_ratelimit_period, L"-RateLimitPeriod"));
                // always remove the arg from our vector
                _args.erase(found_ratelimit_period);
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the total # of iterations
        ///
        /// -Iterations:####
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_iterations(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-Iterations");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                if (IsListening()) {
                    throw invalid_argument("-Iterations is only supported when running as a client");
                }
                Settings->Iterations = as_integral<ULONGLONG>(ParseArgument(*found_arg, L"-Iterations"));
                if (0 == Settings->Iterations) {
                    Settings->Iterations = MAXULONGLONG;
                }
                // always remove the arg from our vector
                _args.erase(found_arg);
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the verbosity level
        ///
        /// -ConsoleVerbosity:## <0-6>
        /// -StatusUpdate:####
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_logging(vector<wchar_t*>& _args)
        {
            auto found_verbosity = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-ConsoleVerbosity");
                return (value != nullptr);
            });
            if (found_verbosity != end(_args)) {
                verbosity = as_integral<unsigned long>(ParseArgument(*found_verbosity, L"-ConsoleVerbosity"));
                if (verbosity > 6) {
                    throw invalid_argument("-ConsoleVerbosity");
                }

                // always remove the arg from our vector
                _args.erase(found_verbosity);
            }

            auto found_status_update = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-StatusUpdate");
                return (value != nullptr);
            });
            if (found_status_update != end(_args)) {
                Settings->StatusUpdateFrequencyMilliseconds = as_integral<unsigned long>(ParseArgument(*found_status_update, L"-StatusUpdate"));
                if (0 == Settings->StatusUpdateFrequencyMilliseconds) {
                    throw invalid_argument("-StatusUpdate");
                }

                // always remove the arg from our vector
                _args.erase(found_status_update);
            }

            wstring connectionFilename;
            wstring errorFilename;
            wstring statusFilename;
            wstring jitterFilename;

            auto found_connection_filename = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-ConnectionFilename");
                return (value != nullptr);
            });
            if (found_connection_filename != end(_args)) {
                connectionFilename = ParseArgument(*found_connection_filename, L"-ConnectionFilename");
                // always remove the arg from our vector
                _args.erase(found_connection_filename);
            }

            auto found_error_filename = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-ErrorFilename");
                return (value != nullptr);
            });
            if (found_error_filename != end(_args)) {
                errorFilename = ParseArgument(*found_error_filename, L"-ErrorFilename");
                // always remove the arg from our vector
                _args.erase(found_error_filename);
            }

            auto found_status_filename = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-StatusFilename");
                return (value != nullptr);
            });
            if (found_status_filename != end(_args)) {
                statusFilename = ParseArgument(*found_status_filename, L"-StatusFilename");
                // always remove the arg from our vector
                _args.erase(found_status_filename);
            }

            auto found_jitter_filename = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-JitterFilename");
                return (value != nullptr);
            });
            if (found_jitter_filename != end(_args)) {
                jitterFilename = ParseArgument(*found_jitter_filename, L"-JitterFilename");
                // always remove the arg from our vector
                _args.erase(found_jitter_filename);
            }

            if (!connectionFilename.empty()) {
                if (ctString::iends_with(connectionFilename, L".csv")) {
                    connectionlogger = make_shared<ctsTextLogger>(connectionFilename.c_str(), StatusFormatting::Csv);
                } else {
                    connectionlogger = make_shared<ctsTextLogger>(connectionFilename.c_str(), StatusFormatting::ClearText);
                }
            }

            if (!errorFilename.empty()) {
                if (ctString::iordinal_equals(connectionFilename, errorFilename)) {
                    if (connectionlogger->IsCsvFormat()) {
                        throw invalid_argument("The error logfile cannot be of csv format");
                    }
                    errorlogger = connectionlogger;
                } else {
                    if (ctString::iends_with(errorFilename, L".csv")) {
                        throw invalid_argument("The error logfile cannot be of csv format");
                    } else {
                        errorlogger = make_shared<ctsTextLogger>(errorFilename.c_str(), StatusFormatting::ClearText);
                    }
                }
            }

            if (!statusFilename.empty()) {
                if (ctString::iordinal_equals(connectionFilename, statusFilename)) {
                    statuslogger = connectionlogger;
                } else if (ctString::iordinal_equals(errorFilename, statusFilename)) {
                    statuslogger = errorlogger;
                } else {
                    if (ctString::iends_with(statusFilename, L".csv")) {
                        statuslogger = make_shared<ctsTextLogger>(statusFilename.c_str(), StatusFormatting::Csv);
                    } else {
                        statuslogger = make_shared<ctsTextLogger>(statusFilename.c_str(), StatusFormatting::ClearText);
                    }
                }
            }

            if (!jitterFilename.empty()) {
                if (ctString::iordinal_equals(connectionFilename, jitterFilename)) {
                    if (!connectionlogger->IsCsvFormat()) {
                        throw invalid_argument("Jitter can only be logged using a csv format");
                    }
                    jitterlogger = connectionlogger;
                } else if (ctString::iordinal_equals(errorFilename, jitterFilename)) {
                    if (!errorlogger->IsCsvFormat()) {
                        throw invalid_argument("Jitter can only be logged using a csv format");
                    }
                    jitterlogger = errorlogger;
                } else if (ctString::iordinal_equals(statusFilename, jitterFilename)) {
                    if (!statuslogger->IsCsvFormat()) {
                        throw invalid_argument("Jitter can only be logged using a csv format");
                    }
                    jitterlogger = statuslogger;
                } else {
                    if (ctString::iends_with(jitterFilename, L".csv")) {
                        jitterlogger = make_shared<ctsTextLogger>(jitterFilename.c_str(), StatusFormatting::Csv);
                    } else {
                        throw invalid_argument("Jitter can only be logged using a csv format");
                    }
                }
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Sets error policy
        ///
        /// -OnError:<log,break>
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_error(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-OnError");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                wchar_t* value = ParseArgument(*found_arg, L"-OnError");
                if (ctString::iordinal_equals(L"log", value)) {
                    break_on_error = false;
                } else if (ctString::iordinal_equals(L"break", value)) {
                    break_on_error = true;
                } else {
                    throw invalid_argument("-OnError");
                }

                // always remove the arg from our vector
                _args.erase(found_arg);
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Sets optional prepostrecvs value
        ///
        /// -PrePostRecvs:#####
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_prepostrecvs(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-PrePostRecvs");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                Settings->PrePostRecvs = as_integral<unsigned long>(ParseArgument(*found_arg, L"-PrePostRecvs"));
                if (0 == Settings->PrePostRecvs) {
                    throw invalid_argument("-PrePostRecvs");
                }

                // always remove the arg from our vector
                _args.erase(found_arg);
            } else {
                Settings->PrePostRecvs = 1;
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Sets a threadpool environment for TP APIs
        ///
        /// -Compartment:<ifalias>
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_compartment(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-Compartment");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                wchar_t* value = ParseArgument(*found_arg, L"-Compartment");
                netAdapterAddresses = new ctNetAdapterAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_ALL_COMPARTMENTS);
                auto found_interface = find_if(
                    netAdapterAddresses->begin(),
                    netAdapterAddresses->end(),
                    [&] (const IP_ADAPTER_ADDRESSES& _adapter_address) {
                    return ctString::iordinal_equals(value, _adapter_address.FriendlyName);
                });
                if (found_interface == netAdapterAddresses->end()) {
                    throw ctException(
                        ERROR_NOT_FOUND,
                        ctString::format_string(
                            L"GetAdaptersAddresses could not find the interface alias '%s'",
                            value).c_str(),
                        L"ctsConfig::set_compartment",
                        true);
                }

                compartment_id = found_interface->CompartmentId;
                // always remove the arg from our vector
                _args.erase(found_arg);
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Sets the compartment to use for incoming and outgoing connections
        ///
        /// Configuring for max threads == number of processors * 2
        ///
        /// currently not exposing this as a command-line parameter
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_threadpool(vector<wchar_t*>&)
        {
            SYSTEM_INFO system_info;
            ::GetSystemInfo(&system_info);
            tp_thread_count = system_info.dwNumberOfProcessors * DefaultThreadpoolFactor;

            ptp_pool = ::CreateThreadpool(NULL);
            if (NULL == ptp_pool) {
                throw ctException(::GetLastError(), L"CreateThreadPool", L"ctsConfig", false);
            }
            ::SetThreadpoolThreadMaximum(ptp_pool, tp_thread_count);

            ::InitializeThreadpoolEnvironment(&tp_environment);
            ::SetThreadpoolCallbackPool(&tp_environment, ptp_pool);

            Settings->PTPEnvironment = &tp_environment;
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for whether to verify buffer contents on receiver
        ///
        /// -verify:<connection,data>
        /// (the old options were <always,never>)
        ///
        /// Note this controls if using a SharedBuffer across all IO or unique buffers
        /// - if not validating data, won't waste memory creating buffers for every connection
        /// - if validating data, must create buffers for every connection
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_shouldVerifyBuffers(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-verify");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                wchar_t* value = ParseArgument(*found_arg, L"-verify");
                if (ctString::iordinal_equals(L"always", value) || ctString::iordinal_equals(L"data", value)) {
                    Settings->ShouldVerifyBuffers = true;
                    Settings->UseSharedBuffer = false;
                } else if (ctString::iordinal_equals(L"never", value) || ctString::iordinal_equals(L"connection", value)) {
                    Settings->ShouldVerifyBuffers = false;
                    Settings->UseSharedBuffer = true;
                } else {
                    throw invalid_argument("-verify");
                }

                // always remove the arg from our vector
                _args.erase(found_arg);
            }
        }

        //////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Parses for the optional maximum time to run
        ///
        /// -TimeLimit:##
        ///
        //////////////////////////////////////////////////////////////////////////////////////////
        static
        void set_timelimit(vector<wchar_t*>& _args)
        {
            auto found_arg = find_if(begin(_args), end(_args), [&] (wchar_t* parameter) -> bool {
                wchar_t* value = ParseArgument(parameter, L"-timelimit");
                return (value != nullptr);
            });
            if (found_arg != end(_args)) {
                Settings->TimeLimit = as_integral<unsigned long>(ParseArgument(*found_arg, L"-timelimit"));
                if (0 == Settings->Port) {
                    throw invalid_argument("-timelimit");
                }
                // always remove the arg from our vector
                _args.erase(found_arg);
            }
        }

        ////////////////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Members within the ctsConfig namespace that can be accessed anywhere within ctsTraffic
        ///
        ////////////////////////////////////////////////////////////////////////////////////////////////////
        void PrintUsage(PrintUsageOption option)
        {
            ctsConfigInitOnce();

            wstring usage;

            switch (option) {
                case PrintUsageOption::Default:
                    usage.append(L"\n\n"
                                 L"ctsTraffic is a utility to generate and validate the integrity of network traffic. It is a client / server application "
                                 L"with the ability to send and receive traffic in a variety of protocol patterns, utilizing a variety of API calling patterns. "
                                 L"The protocol is validated in bytes sent and received for every connection established. Should there be any API failure, any "
                                 L"connection lost prematurely, any protocol failure in bytes sent or received, the tool will capture and log that error information. "
                                 L"Any errors will additionally cause ctsTraffic to return a non-zero error code.\n"
                                 L"Once started, ctrl-c or ctrl-break will cleanly shutdown the client or server\n"
                                 L"\n\n"
                                 L"For issues or questions, please contact 'ctsSupport'\n"
                                 L"\n\n"
                                 L"For details on TCP, UDP, or Logging options, specify the applicable Help option:\n"
                                 L"-Help:[tcp] [udp] [logging] [advanced]\n"
                                 L"\t- <default> == prints this usage statement\n"
                                 L"\t- tcp : prints usage for TCP-specific options\n"
                                 L"\t- udp : prints usage for UDP-specific options\n"
                                 L"\t- logging : prints usage for logging options\n"
                                 L"\t- advanced : prints the usage for advanced and experimental options\n"
                                 L"\n\n"
                                 L"Server-side usage:\n"
                                 L"\tctsTraffic -Listen:<addr or *> [-Port:####] [-ServerExitLimit:<####>] [-Protocol:<tcp/udp>] [-Verify:####] [Protocol-specific options]\n"
                                 L"\n"
                                 L"Client-side usage:\n"
                                 L"\tctsTraffic -Target:<addr or name> [-Port:####] [-Connections:<####>] [-Iterations:<####>] [-Protocol:<tcp/udp>] [-Verify:####] [Protocol-specific options]\n"
                                 L"\n"
                                 L"The Server-side and Client-side may have fully independent settings *except* for the following:\n"
                                 L" (these must match exactly between the client and the server)\n"
                                 L"\t-Port\n"
                                 L"\t-Protocol\n"
                                 L"\t-Verify\n"
                                 L"\t-Pattern (on TCP)\n"
                                 L"\t-Transfer (on TCP)\n"
                                 L"\t-BitsPerSecond (on UDP)\n"
                                 L"\t-FrameRate (on UDP)\n"
                                 L"\t-StreamLength (on UDP)\n"
                                 L"\n\n"
                                 L"----------------------------------------------------------------------\n"
                                 L"                    Common Server-side options                        \n"
                                 L"                                                                      \n"
                                 L"  -Listen, -Port, -ServerExitLimit                                    \n"
                                 L"                                                                      \n"
                                 L"----------------------------------------------------------------------\n"
                                 L"-Listen:<addr or *> [-Listen:<addr> -Listen:<addr>]\n"
                                 L"   - the specific IP Address for the server-side to listen, or '*' for all IP Addresses\n"
                                 L"\t- <required>\n"
                                 L"\t  note : can specify multiple addresses by providing -Listen for each address\n"
                                 L"-ServerExitLimit:####\n"
                                 L"   - the total # of accepted connections before server gracefully exits\n"
                                 L"\t- <default> == 0  (infinite)\n"
                                 L"\n\n"
                                 L"----------------------------------------------------------------------\n"
                                 L"                    Common Client-side options                        \n"
                                 L"                                                                      \n"
                                 L"  -Connections, -Iterations, -Port, -Target                           \n"
                                 L"                                                                      \n"
                                 L"----------------------------------------------------------------------\n"
                                 L"-Connections:####\n"
                                 L"   - the total # of connections at any one time\n"
                                 L"\t- <default> == 8  (there will always be 8 connections doing IO)\n"
                                 L"-Iterations:####\n"
                                 L"   - the number of times to iterate across the number of '-Connections'\n"
                                 L"\t- <default> == 0  (infinite)\n"
                                 L"\t  note : the total # of connections to be made before exit == Iterations * Connections\n"
                                 L"-Target:<addr or name>\n"
                                 L"   - the server-side IP Address, FQDN, or hostname to connect\n"
                                 L"\t- <required>\n"
                                 L"\t  note : given a FQDN or hostname, each new connection will iterate across\n"
                                 L"\t       : all IPv4 and IPv6 addresses which the name resolved\n"
                                 L"\t  note : one can specify '-Target:localhost' when client and server are both local\n"
                                 L"\t  note : one can specify multiple targets by providing -Target for each address or name\n"
                                 L"\n\n"
                                 L"----------------------------------------------------------------------\n"
                                 L"                    Common options for all roles                      \n"
                                 L"                                                                      \n"
                                 L"  -Port, -Protocol, -Verify                                           \n"
                                 L"                                                                      \n"
                                 L"----------------------------------------------------------------------\n"
                                 L"-Port:####\n"
                                 L"   - the port # the server will listen and the client will connect\n"
                                 L"\t- <default> == 4444\n"
                                 L"-Protocol:<udp>\n"
                                 L"   - the protocol used for connectivity and IO\n"
                                 L"\t- tcp : see -help:TCP for usage options\n"
                                 L"\t- udp : see -help:UDP for usage options\n"
                                 L"-Verify:<connection,data>\n"
                                 L"   - an enumeration to indicate the level of integrity verification\n"
                                 L"\t- <default> == data\n"
                                 L"\t- connection : the integrity of every connection is verified\n"
                                 L"\t             : including the precise # of bytes to send and receive\n"
                                 L"\t- data : the integrity of every received data buffer is verified against the an expected bit-pattern\n"
                                 L"\t       : this validation is a superset of 'connection' integrity validation\n"
                                 L"\n");
                    break;

                case PrintUsageOption::Tcp:
                    usage.append(L"\n"
                                 L"----------------------------------------------------------------------\n"
                                 L"                    TCP-specific usage options                        \n"
                                 L"                                                                      \n"
                                 L"  -Buffer, -IO, -Pattern, -PullBytes, -PushBytes, -RateLimit,         \n"
                                 L"   -Transfer                                                          \n"
                                 L"                                                                      \n"
                                 L"----------------------------------------------------------------------\n"
                                 L"-Buffer:#####\n"
                                 L"   - the # of bytes in the buffer used for each send/recv IO\n"
                                 L"\t- <default> == 65536  (each send or recv will post a 64KB buffer)\n"
                                 L"\t- supports range : [low,high]  (each connection will randomly choose a buffer size from within this range)\n"
                                 L"\t  note : Buffer is note required when -Pattern:MediaStream is specified,\n"
                                 L"\t       : FrameSize is the effective buffer size in that traffic pattern\n"
                                 L"-IO:<iocp,rioiocp>\n"
                                 L"   - the API set and usage for processing the protocol pattern\n"
                                 L"\t- <default> == iocp\n"
                                 L"\t- iocp : leverages WSARecv/WSASend using IOCP for async completions\n"
                                 L"\t- rioiocp : registered i/o using an overlapped IOCP for completion notification\n"
                                 L"-Pattern:<push,pull,pushpull,duplex>\n"
                                 L"   - the protocol pattern to send & recv over the TCP connection\n"
                                 L"\t- <default> == push\n"
                                 L"\t- push : client pushes data to server\n"
                                 L"\t- pull : client pulls data from server\n"
                                 L"\t- pushpull : client/server alternates sending/receiving data\n"
                                 L"\t- duplex : client/server sends and receives concurrently throughout the entire connection\n"
                                 L"-PullBytes:#####\n"
                                 L"   - applied only with -Pattern:PushPull - the number of bytes to 'pull'\n"
                                 L"\t- <default> == 1048576 (1MB)\n"
                                 L"\t  note : pullbytes are the bytes received on the client and sent from the server\n"
                                 L"-PushBytes:#####\n"
                                 L"   - applied only with -Pattern:PushPull - the number of bytes to 'push'\n"
                                 L"\t- <default> == 1048576 (1MB)\n"
                                 L"\t  note : pushbytes are the bytes sent from the client and received on the server\n"
                                 L"-RateLimit:#####\n"
                                 L"   - rate limits the number of bytes/sec being *sent* on each individual connection\n"
                                 L"\t- <default> == 0 (no rate limits)\n"
                                 L"\t- supports range : [low,high]  (each connection will randomly choose a rate limit setting from within this range)\n"
                                 L"-Transfer:#####\n"
                                 L"   - the total bytes to transfer per TCP connection\n"
                                 L"\t- <default> == 1073741824  (each connection will transfer a sum total of 1GB)\n"
                                 L"\t- supports range : [low,high]  (each connection will randomly choose a total transfer size send across)\n"
                                 L"\t  note : specifying a range *will* create failures (used to test TCP failures paths)\n"
                                 L"\n");
                    break;

                case PrintUsageOption::Udp:
                    usage.append(L"\n"
                                 L"----------------------------------------------------------------------\n"
                                 L"                    UDP-specific usage options                        \n"
                                 L"                                                                      \n"
                                 L"  * UDP datagrams are streamed in a controlled pattern                \n"
                                 L"    similarly to audio/video streaming solutions                      \n"
                                 L"  * In all cases, the client-side receives and server-side sends      \n"
                                 L"    at a fixed bit-rate and frame-size                                \n"
                                 L"                                                                      \n"
                                 L"  -BitsPerSecond, -FrameRate, -BufferDepth,                           \n"
                                 L"   -StreamLength, -StreamCodec                                        \n"
                                 L"                                                                      \n"
                                 L"----------------------------------------------------------------------\n"
                                 L"-BitsPerSecond:####\n"
                                 L"   - the number of bits per second to stream split across '-FrameRate' # of frames\n"
                                 L"\t- <required>\n"
                                 L"-FrameRate:####\n"
                                 L"   - the number of frames per second being streamed\n"
                                 L"\t- <required>\n"
                                 L"\t  note : for server-side this is the specific frequency that datagrams are sent\n"
                                 L"\t       : for client-side this is the frequency that frames are processed and verified\n"
                                 L"-BufferDepth:####\n"
                                 L"   - the number of seconds to buffer before processing the stream\n"
                                 L"\t- <required>\n"
                                 L"\t  note : this affects the client-side buffering of frames\n"
                                 L"\t       : this also affects how far the client-side will peek at frames to resend if missing\n"
                                 L"\t       : the client will look ahead at 1/2 the buffer depth to request a resend if missing\n"
                                 L"-StreamLength:####\n"
                                 L"   - the total number of seconds to run the entire stream\n"
                                 L"\t- <required>\n"
                                 L"-StreamCodec:<noresends,resendonce>\n"
                                 L"   - codec used when processing the received datagrams from the UDP stream\n"
                                 L"\t- <default> == noresends\n"
                                 L"\t- resendonce : as frames are verified, the client receiving datagrams will look ahead into its buffered\n"
                                 L"\t               datagrams to see if that later frame was dropped, as it should have been received\n"
                                 L"\t               if dropped, the client will immediately request the server to resend the missing frame\n"
                                 L"\t             : a frame that was dropped once but was later received due to the resend request shows\n"
                                 L"\t               under the column 'Retries' in the status updates\n"
                                 L"\t             : a frame that was dropped both in the initial stream *and* dropped from the resend\n"
                                 L"\t               request will show under the 'Dropped' column in the status updates\n"
                                 L"\t- noresends : the client will not look ahead into its buffered datagrams to request a resend\n"
                                 L"\t              from the server.\n"
                                 L"\t            : all frame drops will show under the 'Dropped' column in the status updates\n"
                                 L"\n");
                    break;

                case PrintUsageOption::Logging:
                    usage.append(L"\n"
                                 L"----------------------------------------------------------------------\n"
                                 L"                    Logging options                                   \n"
                                 L"                                                                      \n"
                                 L"  -ConsoleVerbosity,                                                  \n"
                                 L"                                                                      \n"
                                 L"  -ConnectionFilename, -ErrorFilename, -JitterFilename                \n"
                                 L"  -StatusFilename, -StatusUpdate                                      \n"
                                 L"                                                                      \n"
                                 L"----------------------------------------------------------------------\n"
                                 L"Logging in ctsTraffic:\n"
                                 L"\t- Information available to be logged is grouped into 4 basic buckets:\n"
                                 L"\t  - Connection information : this will write a data point for every successful connection established\n"
                                 L"\t                             the IP address and port tuples for the source and destination will be written\n"
                                 L"\t                           : this will also write a data point at the point of every connection completion\n"
                                 L"\t                             information unique to the protocol that was used will be included on success\n"
                                 L"\t                           : -ConnectionFilename specifies the file written with this data\n"
                                 L"\t  - Error information      : this will write error strings at the point of failure of any connection\n"
                                 L"\t                             error information will include the specific point of failure (function that failed)\n"
                                 L"\t                             as well as which connection the failure occured (based off of IP address and port)\n"
                                 L"\t                           : -ErrorFilename specifies the file written with this data\n"
                                 L"\t  - Status information     : this will write out status information as applicable to the protocol being used\n"
                                 L"\t                             the status information will be printed at a frequency set by -StatusUpdate\n"
                                 L"\t                           : the details printed are aggregate values from all connections for that time slice\n"
                                 L"\t                           : -StatusFilename specifies the file written with this data\n"
                                 L"\t  - Jitter information     : for UDP-patterns only, the jitter logging information will write out data per-datagram\n"
                                 L"\t                             this information is formatted specifically to calculate jitter between packets\n"
                                 L"\t                           : it follows the same format used with the published tool ntttcp.exe:\n"
                                 L"\t                             [frame#],[sender.qpc],[sender.qpf],[receiver.qpc],[receiver.qpf]\n"
                                 L"\t                             where qpc is the result of QueryPerformanceCounter and qpf is from QueryPerformanceFrequency\n"
                                 L"\t                           : the algorithm to apply to this data can be found on this site under 'Performance Metrics'\n"
                                 L"\t                             http://msdn.microsoft.com/en-us/library/windows/hardware/dn247504.aspx \n"
                                 L"\t                           : -JitterFilename specifies the file written with this data\n"
                                 L"\n"
                                 L"\t- The format in which the above data is logged is based off of the file extension of the filename specified above\n"
                                 L"\t  There are 3 possible file types:\n"
                                 L"\t  - txt : plain text format is used with the file extension .txt, or for an unrecognized file extension\n"
                                 L"\t          text output is formatted as one would see it printed to the console in UTF8 format\n"
                                 L"\t  - csv : comma-separated value format is used with the file extension .csv\n"
                                 L"\t          information is separated into columns separated by a comma for easier post-processing\n"
                                 L"\t          the column layout of the data is specific to the type of output and protocol being used\n"
                                 L"\t          NOTE: csv formatting will only apply to status updates and jitter, not connection or error information\n"
                                 L"\n\n"
                                 L"-ConsoleVerbosity:<0-5>\n"
                                 L"   - logging verbosity for all information to be written to the console\n"
                                 L"\t- <default> == 4\n"
                                 L"\t- 0 : off (nothing written to the console)\n"
                                 L"\t- 1 : status updates\n"
                                 L"\t- 2 : error information only\n"
                                 L"\t- 3 : connection information only\n"
                                 L"\t- 4 : connection information + error information\n"
                                 L"\t- 5 : connection information + error information + status updates\n"
                                 // L"\t- 6 : above + debug output\n" /// Not exposing debug information to users
                                 L"-ConnectionFilename:<filename with/without path>\n"
                                 L"\t- <default> == (not written to a log file)\n"
                                 L"\t  note : the same filename can be specified for the different logging options\n"
                                 L"\t         in which case the same file will receive all the specified details\n"
                                 L"-ErrorFilename:<filename with/without path>\n"
                                 L"\t- <default> == (not written to a log file)\n"
                                 L"\t  note : the same filename can be specified for the different logging options\n"
                                 L"\t         in which case the same file will receive all the specified details\n"
                                 L"-StatusFilename:<filename with/without path>\n"
                                 L"\t- <default> == (not written to a log file)\n"
                                 L"\t  note : the same filename can be specified for the different logging options\n"
                                 L"\t         in which case the same file will receive all the specified details\n"
                                 L"-JitterFilename:<filename with/without path>\n"
                                 L"\t- <default> == (not written to a log file)\n"
                                 L"\t  note : the same filename can be specified for the different logging options\n"
                                 L"\t         in which case the same file will receive all the specified details\n"
                                 L"-StatusUpdate:####\n"
                                 L"   - the millisecond frequency which real-time status updates are written\n"
                                 L"\t- <default> == 5000 (milliseconds)\n"
                                 L"\n");
                    break;

                case PrintUsageOption::Advanced:
                    usage.append(L"\n"
                                 L"----------------------------------------------------------------------\n"
                                 L"                        Advanced Options                              \n"
                                 L"                                                                      \n"
                                 L"  * these options target specific scenario requirements               \n"
                                 L"                                                                      \n"
                                 L" -Acc, -Bind, -Compartment, -Conn, -IO, -LocalPort,                   \n"
                                 L" -OnError, -Options, -Pattern, -PrePostRecvs, -RateLimitPeriod        \n"
                                 L" -ThrottleConnections, -TimeLimit                                     \n"
                                 L"                                                                      \n"
                                 L"----------------------------------------------------------------------\n"
                                 L"-Acc:<accept,AcceptEx>\n"
                                 L"   - specifies the Winsock API to process accepting inbound connections\n"
                                 L"    the default is appropriate unless deliberately needing to test other APIs\n"
                                 L"\t- <default> == AcceptEx\n"
                                 L"\t- AcceptEx : uses OVERLAPPED AcceptEx with IO Completion ports\n"
                                 L"\t- accept : uses blocking calls to accept\n"
                                 L"\t         : be careful using this as it will not scale out well as each call blocks a thread\n"
                                 L"-Bind:<IP-address or *>\n"
                                 L"   - a client-side option used to control what IP address is used for outgoing connections\n"
                                 L"\t- <default> == *  (will implicitly bind to the correct IP to connect to the target IP)\n"
                                 L"\t  note : this is typically only necessary when wanting to distribute traffic\n"
                                 L"\t         over a specific interface for multi-homed configurations\n"
                                 L"\t  note : can specify multiple addresses by providing -Bind for each address\n"
                                 L"-Compartment:<ifAlias>\n"
                                 L"   - specifies the interface alias of the compartment to use for all sockets\n"
                                 L"    this is most commonly appropriate for servers configured with IP Compartments\n"
                                 L"\t- <default> == using the default IP compartment\n"
                                 L"\t  note : all systems use the default compartment unless explicitly configured otherwise\n"
                                 L"\t  note : the IP addressese specified through -Bind (for clients) and -Listen (for servers)\n"
                                 L"\t         will be directly affected by this Compartment value, including specifying '*'\n"
                                 L"-Conn:<connect,ConnectEx>\n"
                                 L"   - specifies the Winsock API to establish outbound connections\n"
                                 L"    the default is appropriate unless deliberately needing to test other APIs\n"
                                 L"\t- <default> == ConnectEx  (appropriate unless explicitly wanting to test other APIs)\n"
                                 L"\t- ConnectEx : uses OVERLAPPED ConnectEx with IO Completion ports\n"
                                 L"\t- connect : uses blocking calls to connect\n"
                                 L"\t          : be careful using this as it will not scale out well as each call blocks a thread\n"
                                 L"-IO:<readwritefile>\n"
                                 L"   - an additional IO option beyond iocp and rioiocp\n"
                                 L"\t- readwritefile : leverages ReadFile/WriteFile using IOCP for async completions\n"
                                 L"-LocalPort:####\n"
                                 L"   - the local port to bind to when initiating a connection\n"
                                 L"\t- <default> == 0  (an ephemeral port will be chosen when making a connection)\n"
                                 L"\t- supports range : [low,high] each new connection will sequentially choose a port within this range\n"
                                 L"\t  note : You must provide a sufficiently large range to support the number of connections\n"
                                 L"\t  note : Be very careful when using with TCP connections, as port values will not be immediately\n"
                                 L"\t         reusable; TCP will hold an closed IP:port in a TIME_WAIT statue for a period of time\n"
                                 L"\t         only after which will it be able to be reused (default is 4 minutes)\n"
                                 L"-OnError:<log,break>\n"
                                 L"   - policy to control how errors are handled at runtime\n"
                                 L"\t- <default> == log \n"
                                 L"\t- log : log error information only\n"
                                 L"\t- break : break into the debugger with error information\n"
                                 L"\t          useful when live-troubleshooting difficult failures\n"
                                 L"-Options:<keepalive,tcpfastpath>  [-Options:<...>] [-Options:<...>]\n"
                                 L"   - additional socket options and IOCTLS available to be set on connected sockets\n"
                                 L"\t- <default> == None\n"
                                 L"\t- keepalive : only for TCP sockets - enables default timeout Keep-Alive probes\n"
                                 L"\t            : ctsTraffic servers have this enabled by default\n"
                                 L"\t- tcpfastpath : a new option for Windows 8, only for TCP sockets over loopback\n"
                                 L"\t              : the firewall must be disabled for the option to take effect\n"
                                 L"-PrePostRecvs:#####\n"
                                 L"   - specifies the number of recv requests to issue concurrently within an IO Pattern\n"
                                 L"   - for example, with the default -pattern:pull, the client will post recv calls \n"
                                 L"\t     one after another, immediately posting a recv after the prior completed.\n"
                                 L"\t     With -pattern:pull -PrePostRecvs:2, 2 recv calls will be kept in-flight at all times.\n"
                                 L"\t- <default> == 1 for TCP (one recv request at a time)\n"
                                 L"\t- <default> == 2 for UDP (two recv requests kept in-flight)\n"
                                 L"\t  note : with TCP patterns, -verify:connection must be specified in order to specify\n"
                                 L"\t         more than one -PrePostRecvs (UDP can always support any number)\n"
                                 L"-RateLimitPeriod:#####\n"
                                 L"   - the # of milliseconds describing the granularity by which -RateLimit bytes/second is enforced\n"
                                 L"\t     the -RateLimit bytes/second will be evenly split across -RateLimitPeriod milliseconds\n"
                                 L"\t     For example, -RateLimit:1000 -RateLimitPeriod:50 will limit send rates to 100 bytes every 20 ms\n"
                                 L"\t- <default> == 100 (-RateLimit bytes/second will be split out across 100 ms. time slices)\n"
                                 L"\t  note : only applicable to TCP connections\n"
                                 L"\t  note : only applicable is -RateLimit is set (default is not to rate limit)\n"
                                 L"-ThrottleConnections:####\n"
                                 L"   - gates currently pended connection attempts\n"
                                 L"\t- <default> == 1000  (there will be at most 1000 sockets trying to connect at any one time)\n"
                                 L"\t  note : zero means no throttling  (will immediately try to connect all '-Connections')\n"
                                 L"\t       : this is a client-only option\n"
                                 L"-TimeLimit:#####\n"
                                 L"   - the maximum number of seconds to run before the application is aborted and terminated\n"
                                 L"\t- <default> == <no time limit>\n"
                                 L"\t  note : this is to be used only to cap the maximum time to run, as this will log an error\n"
                                 L"\t         if this timelimit is exceeded; predictable results should have the scenario finish\n"
                                 L"\t         before this time limit is hit\n"
                                 L"\n");
                    break;
            }

            ::fwprintf_s(stdout, L"%s", usage.c_str());
        }

        bool Startup(_In_ int argc, _In_reads_(argc) wchar_t** argv)
        {
            ctsConfigInitOnce();

            if (argc < 2) {
                PrintUsage();
                return false;
            }

            // ignore the first argv... the exe itself
            wchar_t** arg_begin = argv + 1;
            wchar_t** arg_end = argv + argc;
            vector<wchar_t*> args(arg_begin, arg_end);

            ///
            /// first check of they asked for help text
            ///
            auto found_help = find_if(
                begin(args),
                end(args),
                [] (_In_ const wchar_t* _arg) -> bool {
                return (ctString::istarts_with(_arg, L"-Help") ||
                        ctString::iordinal_equals(_arg, L"-?"));
            });
            if (found_help != end(args)) {
                LPCWSTR help_string = *found_help;
                if (ctString::iordinal_equals(help_string, L"-Help:Advanced")) {
                    PrintUsage(PrintUsageOption::Advanced);
                    return false;
                } else if (ctString::iordinal_equals(help_string, L"-Help:Tcp")) {
                    PrintUsage(PrintUsageOption::Tcp);
                    return false;
                } else if (ctString::iordinal_equals(help_string, L"-Help:Udp")) {
                    PrintUsage(PrintUsageOption::Udp);
                    return false;
                } else if (ctString::iordinal_equals(help_string, L"-Help:Logging")) {
                    PrintUsage(PrintUsageOption::Logging);
                    return false;
                } else {
                    PrintUsage();
                    return false;
                }
            }

            ///
            /// create the handle for ctrl-c
            ///
            Settings->CtrlCHandle = ::CreateEvent(NULL, TRUE, FALSE, NULL);
            if (Settings->CtrlCHandle == NULL) {
                throw ctException(::GetLastError(), L"CreateEvent", L"ctsConfig::Startup", false);
            }

            ///
            /// Many of the below settings must be made in a specified order - comments below help to explain this reasoning
            /// note: the IO function definitions must come after *all* other settings
            ///       since instantiations of those IO functions might reference global Settings values

            ///
            /// First:
            /// Establish logging settings including verbosity levels and error policies before any functional settings
            /// Create the threadpool before instantiating any other object
            ///
            set_error(args);
            set_logging(args);
            ///
            /// Next: establish the address and port # to be used
            ///
            set_address(args);
            set_port(args);
            set_localport(args);

            ///
            /// ensure a Port is assigned to all listening addresses and target addresses
            ///
            for (auto& addr : Settings->ListenAddresses) {
                if (addr.port() == 0x0000) {
                    addr.setPort(Settings->Port);
                }
            }
            for (auto& addr : Settings->TargetAddresses) {
                if (addr.port() == 0x0000) {
                    addr.setPort(Settings->Port);
                }
            }

            ///
            /// Next: gather the protocol and Pattern to be used
            /// - set the threadpool value after identifying the pattern
            set_protocol(args);
            ///
            /// verify logging matches the protocol
            ///
            if (jitterlogger && ctsConfig::ProtocolType::UDP != Settings->Protocol) {
                throw invalid_argument("Jitter can only be logged using UDP");
            }
            set_ioPattern(args);
            set_threadpool(args);
            // validate protocol & pattern combinations
            if (ProtocolType::UDP == Settings->Protocol && IoPatternType::MediaStream != Settings->IoPattern) {
                throw invalid_argument("UDP only supports the MediaStream IO Pattern");
            }
            if (ProtocolType::TCP == Settings->Protocol && IoPatternType::MediaStream == Settings->IoPattern) {
                throw invalid_argument("TCP does not support the MediaStream IO Pattern");
            }
            // set appropriate defaults for # of connections for TCP vs. UDP
            if (ProtocolType::UDP == Settings->Protocol) {
                Settings->ConnectionLimit = DefaultUdpConnectionLimit;
            } else {
                Settings->ConnectionLimit = DefaultTcpConnectionLimit;
            }

            ///
            /// Next, set the ctsStatusInformation to be used to print status updates for this protocol
            /// - this must be called after both set_logging and set_protocol
            ///
            if (ProtocolType::TCP == Settings->Protocol) {
                print_status = std::make_shared<ctsTcpStatusInformation>();
            } else {
                print_status = std::make_shared<ctsUdpStatusInformation>();
            }
            ///
            /// Next: capture other various settings which do not have explicit dependencies
            ///
            set_options(args);
            set_compartment(args);
            set_connections(args);
            set_throttleConnections(args);
            set_buffer(args);
            set_transfer(args);
            set_ratelimit(args);
            set_iterations(args);
            set_serverExitLimit(args);
            set_timelimit(args);

            if (media_stream_settings.FrameSizeBytes > 0) {
                // the buffersize is now effectively the frame size
                buffersize_high = 0;
                buffersize_low = media_stream_settings.FrameSizeBytes;
                if (buffersize_low < 20) {
                    throw invalid_argument("The media stream frame size (buffer) must be at least 20 bytes");
                }
            }

            // validate localport usage
            if ((Settings->ListenAddresses.size() > 0) && (Settings->LocalPortLow != 0)) {
                throw invalid_argument("Cannot specify both -listen and -LocalPort. To listen on a specific port, use -Port:####");
            }
            if (Settings->LocalPortLow != 0) {
                USHORT numberOfPorts = (Settings->LocalPortHigh == 0) ? 1 : static_cast<USHORT>(Settings->LocalPortHigh - Settings->LocalPortLow + 1);
                if (numberOfPorts < Settings->ConnectionLimit) {
                    throw invalid_argument(
                        "Cannot specify more connections than specified local ports. "
                        "Reduce the number of connections or increase the range of local ports.");
                }
            }

            ///
            /// Set the default values as this setting is optional
            ///
            Settings->ShouldVerifyBuffers = true;
            Settings->UseSharedBuffer = false;
            set_shouldVerifyBuffers(args);
            if (ProtocolType::UDP == Settings->Protocol) {
                // UDP clients can never recv into the same shared buffer since it uses it for seq. numbers, etc
                if (!IsListening()) {
                    Settings->UseSharedBuffer = false;
                }
            }
            set_prepostrecvs(args);
            if (ProtocolType::TCP == Settings->Protocol && Settings->ShouldVerifyBuffers && Settings->PrePostRecvs > 1) {
                throw invalid_argument("-PrePostRecvs > 1 requires -Verify:connection when using TCP");
            }
            ///
            /// finally set the functions to use once all other settings are established
            /// set_ioFunction changes global options for socket operation for instance WSA_FLAG_REGISTERED_IO flag
            /// - hence it is requirement to invoke it prior to any socket operation
            ///
            set_ioFunction(args);
            set_create(args);
            set_connect(args);
            set_accept(args);
            if (Settings->ListenAddresses.size() > 0) {
                // servers 'create' connections when they accept them
                Settings->CreateFunction = Settings->AcceptFunction;
                Settings->ConnectFunction = nullptr;
            }

            if (!args.empty()) {
                std::wstring error_string;
                for (const auto& arg_string : args) {
                    error_string.append(ctString::format_string(L" %s", arg_string));
                }
                error_string.append(L"\n");
                PrintErrorInfoOverride(
                    L"[%.3f] %s\n",
                    ctsConfig::GetStatusTimeStamp(),
                    error_string.c_str());
                throw invalid_argument(ctString::convert_to_string(error_string).c_str());
            }

            auto timer = ::timeBeginPeriod(1);
            if (timer != TIMERR_NOERROR) {
                throw ctl::ctException(timer, L"timeBeginPeriod", false);
            }
            ++timer_changed_count;
            return true;
        }

        void Shutdown()
        {
            ctsConfigInitOnce();

            ctl::ctAutoReleaseCriticalSection lock(&ShutdownLock);
            shutdown_called = true;
            if (Settings->CtrlCHandle != NULL) {
                if (!::SetEvent(Settings->CtrlCHandle)) {
                    ctAlwaysFatalCondition(
                        L"SetEvent(%p) failed [%u] when trying to shutdown",
                        Settings->CtrlCHandle, ::GetLastError());
                }
            }

            delete netAdapterAddresses;
            netAdapterAddresses = nullptr;

            while (timer_changed_count > 0) {
                ::timeEndPeriod(1);
                --timer_changed_count;
            }
        }

        /// the Legend is to explain the fields for status updates
        /// - only print if status updates are going to be provided
        void PrintLegend()
        {
            ctsConfigInitOnce();

            bool write_to_console = false;
            switch (verbosity) {
                // case 0: // nothing
                case 1: // status updates
                    // case 2: // error info
                    // case 3: // connection info
                    // case 4: // connection info + error info
                case 5: // connection info + error info + status updates
                case 6: // above + debug info
                {
                    write_to_console = true;
                }
            }

            if (print_status) {
                if (write_to_console) {
                    LPCWSTR legend = print_status->print_legend(ctsConfig::StatusFormatting::ClearText);
                    if (legend != nullptr) {
                        ::fwprintf(stdout, L"%s\n", legend);
                    }
                    LPCWSTR header = print_status->print_header(ctsConfig::StatusFormatting::ClearText);
                    if (header != nullptr) {
                        ::fwprintf(stdout, L"%s\n", header);
                    }
                }

                if (statuslogger) {
                    statuslogger->LogLegend(print_status);
                    statuslogger->LogHeader(print_status);
                }
                if (connectionlogger && connectionlogger->IsCsvFormat()) {
                    if (ProtocolType::UDP == Settings->Protocol) {
                        connectionlogger->LogMessage(L"TimeSlice,LocalAddress,RemoteAddress,Bits/Sec,Completed,Dropped,Repeated,Retries,Errors,Result\n");

                    } else { // TCP
                        connectionlogger->LogMessage(L"TimeSlice,LocalAddress,RemoteAddress,SendBytes,SendBps,RecvBytes,RecvBps,TimeMs,Result\n");
                    }
                }
            }
        }

        /// Always print to console if override
        void PrintExceptionOverride(const std::exception& e) throw()
        {
            ctsConfigInitOnce();

            ctFatalCondition(break_on_error, L"[ctsTraffic] >> exception - %S\n", e.what());

            try {
                auto formatted_string(
                    ctl::ctString::format_string(
                    L"[%.3f] %s\n",
                    ctsConfig::GetStatusTimeStamp(),
                    ctString::format_exception(e).c_str()));

                ::fwprintf(stderr, L"%s\n", formatted_string.c_str());
                if (errorlogger) {
                    errorlogger->LogError(formatted_string.c_str());
                }
            }
            catch (const std::exception&) {
                ::fwprintf(stderr, L"Error : failed to allocate memory\n");
                if (errorlogger) {
                    errorlogger->LogError(L"Error : failed to allocate memory\n");
                }
            }
        }
        ////////////////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Print* functions
        /// - tracks what level of -verbose was specified
        ///   and prints to console accordingly
        ///
        ////////////////////////////////////////////////////////////////////////////////////////////////////
        void PrintException(const exception& e) throw()
        {
            ctsConfigInitOnce();

            try {
                std::wstring exception_text(ctString::format_exception(e));

                if (!shutdown_called) {
                    ctFatalCondition(break_on_error, L"Fatal exception: %s", exception_text.c_str());
                }

                PrintErrorInfo(
                    L"[%.3f] %s\n",
                    ctsConfig::GetStatusTimeStamp(),
                    exception_text.c_str());
            }
            catch (const std::exception&) {
                if (!shutdown_called) {
                    ctFatalCondition(break_on_error, L"Fatal exception: %S", e.what());
                }

                switch (verbosity) {
                    // case 0: // nothing
                    // case 1: // status updates
                    case 2: // error info
                        // case 3: // connection info
                    case 4: // connection info + error info
                    case 5: // connection info + error info + status updates
                    case 6: // above + debug info
                    {
                        ::wprintf(
                            L"[%.3f] Exception thrown: %S\n",
                            ctsConfig::GetStatusTimeStamp(),
                            e.what());
                    }
                }
            }
        }
        /// Always print to console if override
        void PrintErrorInfoOverride(_In_z_ _Printf_format_string_ LPCWSTR _text, ...) throw()
        {
            ctsConfigInitOnce();

            va_list argptr;
            va_start(argptr, _text);

            ctFatalConditionVa(break_on_error, _text, argptr);

            ::vwprintf_s(_text, argptr);
            if (errorlogger) {
                try {
                    errorlogger->LogError(ctString::format_string_va(_text, argptr).c_str());
                }
                catch (const std::exception&) {
                }
            }

            va_end(argptr);
        }
        void PrintErrorInfo(_In_z_ _Printf_format_string_ LPCWSTR _text, ...) throw()
        {
            ctsConfigInitOnce();

            if (!shutdown_called) {
                va_list argptr;
                va_start(argptr, _text);

                ctFatalConditionVa(break_on_error, _text, argptr);

                bool write_to_console = false;
                switch (verbosity) {
                    // case 0: // nothing
                    // case 1: // status updates
                    case 2: // error info
                        // case 3: // connection info
                    case 4: // connection info + error info
                    case 5: // connection info + error info + status updates
                    case 6: // above + debug info
                    {
                        write_to_console = true;
                    }
                }

                if (write_to_console) {
                    ::vwprintf_s(_text, argptr);
                }

                if (errorlogger) {
                    try {
                        errorlogger->LogError(ctString::format_string_va(_text, argptr).c_str());
                    }
                    catch (const std::exception&) {
                    }
                }

                va_end(argptr);
            }
        }
        void PrintErrorIfFailed(_In_ LPCWSTR _what, unsigned long _why) throw()
        {
            ctsConfigInitOnce();

            if (!shutdown_called && (_why != 0)) {
                ctFatalCondition(break_on_error, L"%s failed (%u)\n", _what, _why);

                bool write_to_console = false;
                switch (verbosity) {
                    // case 0: // nothing
                    // case 1: // status updates
                    case 2: // error info
                        // case 3: // connection info
                    case 4: // connection info + error info
                    case 5: // connection info + error info + status updates
                    case 6: // above + debug info
                    {
                        write_to_console = true;
                    }
                }


                try {
                    std::wstring error_string;
                    if (ctsIOPatternProtocolError(static_cast<ctsIOPatternStatus>(_why))) {
                        error_string = ctl::ctString::format_string(
                            L"[%.3f] Connection aborted due to the protocol error %s\n",
                            ctsConfig::GetStatusTimeStamp(),
                            ctsIOPatternProtocolErrorString(static_cast<ctsIOPatternStatus>(_why)));
                    } else {
                        ctException error_details(_why, _what);
                        error_string = ctl::ctString::format_string(
                            L"[%.3f] %s failed (%u) %s\n",
                            ctsConfig::GetStatusTimeStamp(),
                            _what,
                            _why,
                            error_details.translation_w());
                    }

                    if (write_to_console) {
                        ::fwprintf(stderr, L"%s", error_string.c_str());
                    }

                    if (errorlogger) {
                        errorlogger->LogError(error_string.c_str());
                    }
                }
                catch (const std::exception&) {
                }
            }
        }
        void PrintStatusUpdate() throw()
        {
            ctsConfigInitOnce();

            if (!shutdown_called) {
                if (print_status) {
                    bool write_to_console = false;
                    switch (verbosity) {
                        // case 0: // nothing
                        case 1: // status updates
                            // case 2: // error info
                            // case 3: // connection info
                            // case 4: // connection info + error info
                        case 5: // connection info + error info + status updates
                        case 6: // above + debug info
                        {
                            write_to_console = true;
                        }
                    }

                    if (::TryEnterCriticalSection(&StatusUpdateLock)) {
                        ctlScopeGuard(leaveCSOnExit, { ::LeaveCriticalSection(&StatusUpdateLock); });

                        // capture the timeslices
                        ctsSignedLongLong l_previoutimeslice = printing_previous_timeslice;
                        ctsSignedLongLong l_current_timeslice = ctTimer::snap_qpc_msec() - Settings->StartTimeMilliseconds;

                        if (l_current_timeslice > l_previoutimeslice) {
                            // write out the header to the console every 40 updates 
                            if (write_to_console) {
                                if (printing_timeslice_count != 0 && 0 == printing_timeslice_count % 40) {
                                    LPCWSTR header = print_status->print_header(ctsConfig::StatusFormatting::ClearText);
                                    if (header != nullptr) {
                                        ::fwprintf(stdout, L"%s", header);
                                    }
                                }
                            }

                            // need to indicate either print_status() or LogStatus() to reset the status info,
                            // - the data *must* be reset once and *only once* in this function

                            int status_count = 0;
                            if (write_to_console) {
                                ++status_count;
                            }
                            if (statuslogger) {
                                ++status_count;
                            }

                            if (write_to_console) {
                                --status_count;
                                bool clear_status = (0 == status_count);
                                LPCWSTR print_string = print_status->print_status(
                                    ctsConfig::StatusFormatting::ClearText,
                                    l_current_timeslice,
                                    clear_status);
                                if (print_string != nullptr) {
                                    ::fwprintf(stdout, L"%s", print_string);
                                }
                            }

                            if (statuslogger) {
                                --status_count;
                                bool clear_status = (0 == status_count);
                                statuslogger->LogStatus(
                                    print_status,
                                    l_current_timeslice,
                                    clear_status);
                            }

                            // update tracking values
                            printing_previous_timeslice = l_current_timeslice;
                            ++printing_timeslice_count;
                        }
                    }
                }
            }
        }

        void PrintJitterUpdate(long long _sequence_number, long long _sender_qpc, long long _sender_qpf, long long _recevier_qpc, long long _receiver_qpf) throw()
        {
            ctsConfigInitOnce();

            if (!shutdown_called) {
                if (jitterlogger) {
                    // long long ~= up to 20 characters long, plus 5 for commas & CR
                    static const size_t formatted_text_length = (20 * 5) + 5;
                    wchar_t formatted_text[formatted_text_length];
                    formatted_text[0] = L'\0';
                    ::_snwprintf_s(
                        formatted_text,
                        formatted_text_length,
                        _TRUNCATE,
                        L"%lld,%lld,%lld,%lld,%lld\n",
                        _sequence_number, _sender_qpc, _sender_qpf, _recevier_qpc, _receiver_qpf);
                    jitterlogger->LogMessage(formatted_text);
                }
            }
        }

        void PrintNewConnection(const ctl::ctSockaddr& _remote_addr) throw()
        {
            ctsConfigInitOnce();

            // write even after shutdown so can print the final summaries
            bool write_to_console = false;
            switch (verbosity) {
                // case 0: // nothing
                // case 1: // status updates
                // case 2: // error info
                case 3: // connection info
                case 4: // connection info + error info
                case 5: // connection info + error info + status updates
                case 6: // above + debug info
                {
                    write_to_console = true;
                }
            }

            if (write_to_console) {
                ::wprintf_s(
                    (ProtocolType::TCP == Settings->Protocol) ?
                        L"[%.3f] TCP connection established to %s\n" :
                        L"[%.3f] UDP connection established to %s\n",
                    GetStatusTimeStamp(),
                    _remote_addr.writeCompleteAddress().c_str());
            }

            if (connectionlogger && !connectionlogger->IsCsvFormat()) {
                try {
                    connectionlogger->LogMessage(
                        ctString::format_string(
                            (ProtocolType::TCP == Settings->Protocol) ?
                                L"[%.3f] TCP connection established to %s\n" :
                                L"[%.3f] UDP connection established to %s\n",
                            GetStatusTimeStamp(),
                            _remote_addr.writeCompleteAddress().c_str()).c_str());
                }
                catch (const std::exception&) {
                }
            }
        }

        void PrintConnectionResults(const ctl::ctSockaddr& _local_addr, const ctl::ctSockaddr& _remote_addr, unsigned long _error, const ctsTcpStatistics& _stats) throw()
        {
            ctsConfigInitOnce();

            // write even after shutdown so can print the final summaries
            bool write_to_console = false;
            switch (verbosity) {
                // case 0: // nothing
                // case 1: // status updates
                // case 2: // error info
                case 3: // connection info
                case 4: // connection info + error info
                case 5: // connection info + error info + status updates
                case 6: // above + debug info
                {
                    write_to_console = true;
                }
            }

            enum ErrorType {
                Success,
                NetworkError,
                ProtocolError
            } error_type;

            if (0 == _error) {
                error_type = Success;
            } else if (ctsIOPatternProtocolError(static_cast<ctsIOPatternStatus>(_error))) {
                error_type = ProtocolError;
            } else {
                error_type = NetworkError;
            }

            static LPCWSTR TCPSuccessfulResultTextFormat = L"[%.3f] TCP connection succeeded : [%s - %s] : SendBytes[%lld]  SendBps[%lld]  RecvBytes[%lld]  RecvBps[%lld]  Time[%lld ms]\n";
            static LPCWSTR TCPNetworkFailureResultTextFormat = L"[%.3f] TCP connection failed with the error %s : [%s - %s] : SendBytes[%lld]  SendBps[%lld]  RecvBytes[%lld]  RecvBps[%lld]  Time[%lld ms]\n";
            static LPCWSTR TCPProtocolFailureResultTextFormat = L"[%.3f] TCP connection failed with the protocol error %s : [%s - %s] : SendBytes[%lld]  SendBps[%lld]  RecvBytes[%lld]  RecvBps[%lld]  Time[%lld ms]\n";

            // csv format : L"TimeSlice,LocalAddress,RemoteAddress,SendBytes,SendBps,RecvBytes,RecvBps,TimeMs,Result"
            static LPCWSTR TCPResultCsvFormat = L"%.3f,%s,%s,%lld,%lld,%lld,%lld,%lld,%s\n";

            long long total_time(_stats.end_time.get() - _stats.start_time.get());
            ctl::ctFatalCondition(
                total_time < 0LL,
                L"end_time is less than start_time in this ctsTcpStatistics object (%p)", &_stats);
            float current_time = ctsConfig::GetStatusTimeStamp();

            try {
                std::wstring csv_string;
                std::wstring text_string;
                std::wstring error_string;
                if (ProtocolError != error_type) {
                    if (0 == _error) {
                        error_string = L"Succeeded";
                    } else {
                        error_string = ctl::ctString::format_string(
                            L"%lu: %s",
                            _error,
                            ctl::ctException(_error).translation_w());
                    }
                }

                if (connectionlogger && connectionlogger->IsCsvFormat()) {
                    csv_string = ctString::format_string(
                        TCPResultCsvFormat,
                        current_time,
                        _local_addr.writeCompleteAddress().c_str(),
                        _remote_addr.writeCompleteAddress().c_str(),
                        _stats.bytes_sent.get(),
                        (total_time > 0LL) ? static_cast<long long>(_stats.bytes_sent.get() * 1000LL / total_time) : 0LL,
                        _stats.bytes_recv.get(),
                        (total_time > 0LL) ? static_cast<long long>(_stats.bytes_recv.get() * 1000LL / total_time) : 0LL,
                        total_time,
                        (ProtocolError == error_type) ?
                            ctsIOPatternProtocolErrorString(static_cast<ctsIOPatternStatus>(_error)) :
                            error_string.c_str());
                }
                // we'll never write csv format to the console so we'll need a text string in that case
                // - and/or in the case the connectionlogger isn't writing to csv
                if (write_to_console || (connectionlogger && !connectionlogger->IsCsvFormat())) {
                    if (0 == _error) {
                        text_string = ctString::format_string(
                            TCPSuccessfulResultTextFormat,
                            current_time,
                            _local_addr.writeCompleteAddress().c_str(),
                            _remote_addr.writeCompleteAddress().c_str(),
                            _stats.bytes_sent.get(),
                            (total_time > 0LL) ? static_cast<long long>(_stats.bytes_sent.get() * 1000LL / total_time) : 0LL,
                            _stats.bytes_recv.get(),
                            (total_time > 0LL) ? static_cast<long long>(_stats.bytes_recv.get() * 1000LL / total_time) : 0LL,
                            total_time);
                    } else {
                        text_string = ctString::format_string(
                            (ProtocolError == error_type) ? TCPProtocolFailureResultTextFormat : TCPNetworkFailureResultTextFormat,
                            current_time,
                            (ProtocolError == error_type) ?
                                ctsIOPatternProtocolErrorString(static_cast<ctsIOPatternStatus>(_error)) :
                                error_string.c_str(),
                            _local_addr.writeCompleteAddress().c_str(),
                            _remote_addr.writeCompleteAddress().c_str(),
                            _stats.bytes_sent.get(),
                            (total_time > 0LL) ? static_cast<long long>(_stats.bytes_sent.get() * 1000LL / total_time) : 0LL,
                            _stats.bytes_recv.get(),
                            (total_time > 0LL) ? static_cast<long long>(_stats.bytes_recv.get() * 1000LL / total_time) : 0LL,
                            total_time);
                    }
                }

                if (write_to_console) {
                    // text strings always go to the console
                    ::wprintf(L"%s", text_string.c_str());
                }

                if (connectionlogger) {
                    if (connectionlogger->IsCsvFormat()) {
                        connectionlogger->LogMessage(csv_string.c_str());
                    } else {
                        connectionlogger->LogMessage(text_string.c_str());
                    }
                }
            }
            catch (const std::exception&) {
            }
        }
        void PrintConnectionResults(const ctl::ctSockaddr& _local_addr, const ctl::ctSockaddr& _remote_addr, unsigned long _error, const ctsUdpStatistics& _stats) throw()
        {
            ctsConfigInitOnce();

            // write even after shutdown so can print the final summaries
            bool write_to_console = false;
            switch (verbosity) {
                // case 0: // nothing
                // case 1: // status updates
                // case 2: // error info
                case 3: // connection info
                case 4: // connection info + error info
                case 5: // connection info + error info + status updates
                case 6: // above + debug info
                {
                    write_to_console = true;
                }
            }

            enum ErrorType {
                Success,
                NetworkError,
                ProtocolError
            } error_type;

            if (0 == _error) {
                error_type = Success;
            } else if (ctsIOPatternProtocolError(static_cast<ctsIOPatternStatus>(_error))) {
                error_type = ProtocolError;
            } else {
                error_type = NetworkError;
            }

            static LPCWSTR UDPSuccessfulResultTextFormat = L"[%.3f] UDP connection succeeded : [%s - %s] : BitsPerSecond [%llu]  Completed [%llu]  Dropped [%llu]  Repeated [%llu]  Retries [%llu]  Errors [%llu]\n";
            static LPCWSTR UDPNetworkFailureResultTextFormat = L"[%.3f] UDP connection failed with the error %s : [%s - %s] : BitsPerSecond [%llu]  Completed [%llu]  Dropped [%llu]  Repeated [%llu]  Retries [%llu]  Errors [%llu]\n";
            static LPCWSTR UDPProtocolFailureResultTextFormat = L"[%.3f] UDP connection failed with the protocol error %s : [%s - %s] : BitsPerSecond [%llu]  Completed [%llu]  Dropped [%llu]  Repeated [%llu]  Retries [%llu]  Errors [%llu]\n";

            // csv format : "TimeSlice,LocalAddress,RemoteAddress,Bits/Sec,Completed,Dropped,Repeated,Retries,Errors,Result"
            static LPCWSTR UDPResultCsvFormat = L"%.3f,%s,%s,%llu,%llu,%llu,%llu,%llu,%llu,%s\n";

            float current_time = ctsConfig::GetStatusTimeStamp();
            long long elapsed_time(_stats.end_time.get() - _stats.start_time.get());
            long long bits_per_second = (elapsed_time > 0LL) ? static_cast<long long>(_stats.bits_received.get() * 1000LL / elapsed_time) : 0LL;

            try {
                std::wstring csv_string;
                std::wstring text_string;
                std::wstring error_string;
                if (ProtocolError != error_type) {
                    if (0 == _error) {
                        error_string = L"Succeeded";
                    } else {
                        error_string = ctl::ctString::format_string(
                            L"%u: %s",
                            _error,
                            ctl::ctException(_error).translation_w());
                    }
                }

                if (connectionlogger && connectionlogger->IsCsvFormat()) {
                    csv_string = ctString::format_string(
                        UDPResultCsvFormat,
                        current_time,
                        _local_addr.writeCompleteAddress().c_str(),
                        _remote_addr.writeCompleteAddress().c_str(),
                        bits_per_second,
                        _stats.successful_frames.get(),
                        _stats.dropped_frames.get(),
                        _stats.duplicate_frames.get(),
                        _stats.retry_attempts.get(),
                        _stats.error_frames.get(),
                        (ProtocolError == error_type) ?
                            ctsIOPatternProtocolErrorString(static_cast<ctsIOPatternStatus>(_error)) :
                            error_string.c_str());
                }
                // we'll never write csv format to the console so we'll need a text string in that case
                // - and/or in the case the connectionlogger isn't writing to csv
                if (write_to_console || (connectionlogger && !connectionlogger->IsCsvFormat())) {
                    if (0 == _error) {
                        text_string = ctString::format_string(
                            UDPSuccessfulResultTextFormat,
                            current_time,
                            _local_addr.writeCompleteAddress().c_str(),
                            _remote_addr.writeCompleteAddress().c_str(),
                            bits_per_second,
                            _stats.successful_frames.get(),
                            _stats.dropped_frames.get(),
                            _stats.duplicate_frames.get(),
                            _stats.retry_attempts.get(),
                            _stats.error_frames.get());
                    } else {
                        text_string = ctString::format_string(
                            (ProtocolError == error_type) ? UDPProtocolFailureResultTextFormat : UDPNetworkFailureResultTextFormat,
                            current_time,
                            (ProtocolError == error_type) ?
                                ctsIOPatternProtocolErrorString(static_cast<ctsIOPatternStatus>(_error)) :
                                error_string.c_str(),
                            _local_addr.writeCompleteAddress().c_str(),
                            _remote_addr.writeCompleteAddress().c_str(),
                            bits_per_second,
                            _stats.successful_frames.get(),
                            _stats.dropped_frames.get(),
                            _stats.duplicate_frames.get(),
                            _stats.retry_attempts.get(),
                            _stats.error_frames.get());
                    }
                }

                if (write_to_console) {
                    // text strings always go to the console
                    ::wprintf(L"%s", text_string.c_str());
                }

                if (connectionlogger) {
                    if (connectionlogger->IsCsvFormat()) {
                        connectionlogger->LogMessage(csv_string.c_str());
                    } else {
                        connectionlogger->LogMessage(text_string.c_str());
                    }
                }
            }
            catch (const std::exception&) {
            }
        }
        void PrintDebug(_In_z_ _Printf_format_string_ LPCWSTR _text, ...) throw()
        {
            ctsConfigInitOnce();

            if (!shutdown_called) {
                switch (verbosity) {
                    // case 0: // nothing
                    // case 1: // status updates
                    // case 2: // error info
                    // case 3: // connection info
                    // case 4: // connection info + error info
                    // case 5: // connection info + error info + status updates
                    case 6: // above + debug info
                    {
                        va_list argptr;
                        va_start(argptr, _text);
                        ::vwprintf_s(_text, argptr);
                        va_end(argptr);
                    }
                }
            }
        }
        void PrintDebugIfFailed(_In_ LPCWSTR _what, unsigned long _why, _In_ LPCWSTR _where) throw()
        {
            ctsConfigInitOnce();

            if (!shutdown_called && (_why != 0)) {
                switch (verbosity) {
                    // case 0: // nothing
                    // case 1: // status updates
                    // case 2: // error info
                    // case 3: // connection info
                    // case 4: // connection info + error info
                    // case 5: // connection info + error info + status updates
                    case 6: // above + debug info
                    {
                        ::fwprintf_s(stdout, L"\tNonFatal Error: %s failed (%u) [%s]", _what, _why, _where);
                    }
                }
            }
        }
        void PrintSummary(_In_z_ _Printf_format_string_ LPCWSTR _text, ...) throw()
        {
            ctsConfigInitOnce();

            // write even after shutdown so can print the final summaries
            bool write_to_console = false;
            switch (verbosity) {
                // case 0: // nothing
                case 1: // status updates
                case 2: // error info
                case 3: // connection info
                case 4: // connection info + error info
                case 5: // connection info + error info + status updates
                case 6: // above + debug info
                {
                    write_to_console = true;
                }
            }

            va_list argptr;
            va_start(argptr, _text);

            if (write_to_console) {
                ::vwprintf_s(_text, argptr);
            }

            if (connectionlogger && !connectionlogger->IsCsvFormat()) {
                try {
                    connectionlogger->LogMessage(ctString::format_string_va(_text, argptr).c_str());
                }
                catch (const std::exception&) {
                }
            }

            va_end(argptr);
        }


        ////////////////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Get* 
        /// - accessor functions made public to retrieve configuration details
        ///
        ////////////////////////////////////////////////////////////////////////////////////////////////////
        ctsUnsignedLong GetBufferSize() throw()
        {
            ctsConfigInitOnce();

            if (0 == buffersize_high) {
                // range was not specified
                return buffersize_low;
            } else {
                return random.uniform_int(buffersize_low, buffersize_high);
            }
        }

        ctsUnsignedLong GetMaxBufferSize() throw()
        {
            ctsConfigInitOnce();

            if (buffersize_high == 0) {
                // User didn't specify a range
                return buffersize_low;
            } else {
                return buffersize_high;
            }
        }


        ctsUnsignedLongLong GetTransferSize() throw()
        {
            ctsConfigInitOnce();

            if (0 == transfer_high) {
                // range was not specified
                return transfer_low;
            } else {
                return random.uniform_int(transfer_low, transfer_high);
            }
        }

        ctsSignedLongLong GetTcpBytesPerSecond() throw()
        {
            ctsConfigInitOnce();

            if (0 == ratelimit_high) {
                // range was not specified
                return ratelimit_low;
            } else {
                return random.uniform_int(ratelimit_low, ratelimit_high);
            }
        }

        int GetListenBacklog() throw()
        {
            ctsConfigInitOnce();

            int backlog = SOMAXCONN;
            // Starting in Win8 listen() supports a larger backlog
            if (ctSocketIsRioAvailable()) {
                backlog = SOMAXCONN_HINT(SOMAXCONN);
            }
            return backlog;
        }

        const MediaStreamSettings& GetMediaStream() throw()
        {
            ctsConfigInitOnce();

            ctFatalCondition(
                0 == media_stream_settings.BitsPerSecond,
                L"Internally requesting media stream settings when this was not specified by the user");

            return media_stream_settings;
        }

        bool IsListening() throw()
        {
            ctsConfigInitOnce();

            return !Settings->ListenAddresses.empty();
        }

        float GetStatusTimeStamp() throw()
        {
            return static_cast<float>((ctl::ctTimer::snap_qpc_msec() - static_cast<long long>(Settings->StartTimeMilliseconds)) / 1000.0);
        }

        ////////////////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// Set*Options
        /// - functions capturing any options that need to be set on a socket across different states
        /// - currently only implementing pre-bind options
        ///
        ////////////////////////////////////////////////////////////////////////////////////////////////////
        int SetPreBindOptions(SOCKET _s, const ctl::ctSockaddr& _local_address)
        {
            ctsConfigInitOnce();

            ///
            /// if the user specified bind addresses, enable SO_PORT_SCALABILITY
            /// - this will allow each unique IP address the full range of ephemeral ports
            /// this option is not available when just binding to INET_ANY (making an ephemeral bind)
            /// this option is also not used if the user is binding to an explicit port #
            /// - since the port scalability rules no longer apply
            ///
            if (ProtocolType::TCP == Settings->Protocol && !_local_address.isAddressAny() && _local_address.port() == 0) {
                DWORD optval = 1; // BOOL
                int optlen = static_cast<int>(sizeof optval);

                if (0 != ::setsockopt(
                    _s,
                    SOL_SOCKET,   // level
                    SO_PORT_SCALABILITY, // optname
                    reinterpret_cast<const char *>(&optval),
                    optlen)) {
                    int gle = ::WSAGetLastError();

                    PrintErrorIfFailed(L"setsockopt(SO_PORT_SCALABILITY)", gle);
                    return gle;
                }
            }

            ///
            /// netAdapterAddresses is created when the user has requested a compartment Id
            /// - since we would have had to lookup the interface
            ///
            if (netAdapterAddresses != nullptr) {
                int optval = compartment_id;
                int optlen = static_cast<int>(sizeof optval);

                if (0 != ::setsockopt(
                  _s,
                  SOL_SOCKET,   // level
                  SO_COMPARTMENT_ID, // optname
                  reinterpret_cast<const char *>(&optval),
                  optlen)) {
                    int gle = ::WSAGetLastError();

                    PrintErrorIfFailed(L"setsockopt(SO_COMPARTMENT_ID)", gle);
                    return gle;
                }
            }

            if (Settings->Options & OptionType::LOOPBACK_FAST_PATH) {
                DWORD in_value = 1;
                DWORD out_value;
                DWORD bytes_returned;

                if (0 != ::WSAIoctl(
                    _s,
                    SIO_LOOPBACK_FAST_PATH,
                    &in_value, static_cast<DWORD>(sizeof(in_value)),
                    &out_value, static_cast<DWORD>(sizeof(out_value)),
                    &bytes_returned,
                    NULL,
                    NULL)) {
                    int gle = ::WSAGetLastError();
                    PrintErrorIfFailed(L"WSAIoctl(SIO_LOOPBACK_FAST_PATH)", gle);
                    return gle;
                }
            }

            if (Settings->Options & OptionType::KEEPALIVE) {
                int optval = 1;
                int optlen = static_cast<int>(sizeof optval);

                if (0 != ::setsockopt(
                  _s,
                  SOL_SOCKET,   // level
                  SO_KEEPALIVE, // optname
                  reinterpret_cast<const char *>(&optval),
                  optlen)) {
                    int gle = ::WSAGetLastError();
                    PrintErrorIfFailed(L"setsockopt(SO_KEEPALIVE)", gle);
                    return gle;
                }
            }

            if (Settings->Options & OptionType::MAX_RECV_BUF) {
                static const int recv_buff = 1048576;
                if (0 != setsockopt(
                    _s,
                    SOL_SOCKET,
                    SO_RCVBUF,
                    reinterpret_cast<char *>(const_cast<int*>(&recv_buff)),
                    static_cast<int>(sizeof(recv_buff)))) {
                    int gle = ::WSAGetLastError();
                    PrintErrorIfFailed(L"setsockopt(SO_RCVBUF)", gle);
                    return gle;
                }
            }

            if (Settings->Options & OptionType::NON_BLOCKING_IO) {
                u_long EnableNonBlocking = 1;
                if (0 != ::ioctlsocket(
                    _s,
                    FIONBIO,
                    &EnableNonBlocking)) {
                    int gle = ::WSAGetLastError();
                    PrintErrorIfFailed(L"ioctlsocket(FIONBIO)", gle);
                    return gle;
                }
            }

            if (Settings->Options & OptionType::HANDLE_INLINE_IOCP) {
                if (!::SetFileCompletionNotificationModes(reinterpret_cast<HANDLE>(_s), FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)) {
                    int gle = ::GetLastError();
                    PrintErrorIfFailed(L"SetFileCompletionNotificationModes(FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)", gle);
                    return gle;
                }
            }
            return NO_ERROR;
        }

        int SetPreConnectOptions(SOCKET _s)
        {
            ctsConfigInitOnce();
            UNREFERENCED_PARAMETER(_s);
            return 0;
        }

        void UpdateGlobalStats(const ctsTcpStatistics& _in_stats) throw()
        {
            Settings->HistoricTcpDetails.total_time.add(_in_stats.end_time.get() - _in_stats.start_time.get());
            Settings->HistoricTcpDetails.bytes_recv.add(_in_stats.bytes_recv.get());
            Settings->HistoricTcpDetails.bytes_sent.add(_in_stats.bytes_sent.get());
        }
        void UpdateGlobalStats(const ctsUdpStatistics& _in_stats) throw()
        {
            Settings->HistoricUdpDetails.total_time.add(_in_stats.end_time.get() - _in_stats.start_time.get());
            Settings->HistoricUdpDetails.bits_received.add(_in_stats.bits_received.get());
            Settings->HistoricUdpDetails.dropped_frames.add(_in_stats.dropped_frames.get());
            Settings->HistoricUdpDetails.error_frames.add(_in_stats.error_frames.get());
            Settings->HistoricUdpDetails.duplicate_frames.add(_in_stats.duplicate_frames.get());
            Settings->HistoricUdpDetails.retry_attempts.add(_in_stats.retry_attempts.get());
            Settings->HistoricUdpDetails.successful_frames.add(_in_stats.successful_frames.get());
        }

        ////////////////////////////////////////////////////////////////////////////////////////////////////
        ///
        /// PrintSettings
        /// - public function to write out to the console applied settings
        ///
        ////////////////////////////////////////////////////////////////////////////////////////////////////
        void PrintSettings()
        {
            ctsConfigInitOnce();

            wstring setting_string(
                L"  Configured Settings  \n"
                L"-----------------------\n");

            setting_string.append(L"\tProtocol: ");
            switch (Settings->Protocol) {
                case ProtocolType::TCP:
                    setting_string.append(L"TCP");
                    break;
                case ProtocolType::UDP:
                    setting_string.append(L"UDP");
                    break;
                case ProtocolType::Multicast:
                    setting_string.append(L"UDP Multicast");
                    break;
                case ProtocolType::RAW:
                    setting_string.append(L"RAW");
                    break;
            }
            setting_string.append(L"\n");

            setting_string.append(L"\tOptions:");
            if (OptionType::NoOptionSet == Settings->Options) {
                setting_string.append(L" None");
            } else {
                if (Settings->Options & OptionType::KEEPALIVE) {
                    setting_string.append(L" KeepAlive");
                }
                if (Settings->Options & OptionType::LOOPBACK_FAST_PATH) {
                    setting_string.append(L" TCPFastPath");
                }
            }
            setting_string.append(L"\n");

            setting_string.append(ctString::format_string(L"\tIO function: %s\n", IoFunctionName));

            setting_string.append(L"\tIoPattern: ");
            switch (Settings->IoPattern) {
                case IoPatternType::Pull:
                    setting_string.append(L"Pull <TCP client recv/server send>\n");
                    break;
                case IoPatternType::Push:
                    setting_string.append(L"Push <TCP client send/server recv>\n");
                    break;
                case IoPatternType::PushPull:
                    setting_string.append(L"PushPull <TCP client/server alternate send/recv>\n");
                    setting_string.append(ctString::format_string(L"\t\tPushBytes: %lu\n", static_cast<unsigned long>(Settings->PushBytes)));
                    setting_string.append(ctString::format_string(L"\t\tPullBytes: %lu\n", static_cast<unsigned long>(Settings->PullBytes)));
                    break;
                case IoPatternType::Duplex:
                    setting_string.append(L"Duplex <TCP client/server both sending and receiving>\n");
                    break;
                case IoPatternType::MediaStream:
                    setting_string.append(L"MediaStream <UDP controlled stream from server to client>\n");
            }

            setting_string.append(
                ctString::format_string(
                    L"\tLevel of verification: %s\n",
                    Settings->ShouldVerifyBuffers ? L"Connections & Data" : L"Connections"));

            setting_string.append(ctString::format_string(L"\tPort: %u\n", Settings->Port));

            if (0 == buffersize_high) {
                setting_string.append(
                    ctString::format_string(
                        L"\tBuffer used for each IO request: %u [0x%x] bytes\n",
                        buffersize_low, buffersize_low));
            } else {
                setting_string.append(
                    ctString::format_string(
                        L"\tBuffer used for each IO request: [%u, %u] bytes\n",
                        buffersize_low, buffersize_high));
            }

            if (0 == transfer_high) {
                setting_string.append(
                    ctString::format_string(
                        L"\tTotal transfer per connection: %llu bytes\n",
                        transfer_low));
            } else {
                setting_string.append(
                    ctString::format_string(
                        L"\tTotal transfer per connection: [%llu, %llu] bytes\n",
                        transfer_low, transfer_high));
            }

            if (ProtocolType::UDP == Settings->Protocol) {
                setting_string.append(
                    ctString::format_string(
                        L"\t\tUDP Stream BitsPerSecond: %lld bits per second\n",
                        static_cast<long long>(media_stream_settings.BitsPerSecond)));
                setting_string.append(
                    ctString::format_string(
                        L"\t\tUDP Stream FrameRate: %lu frames per second\n",
                        static_cast<unsigned long>(media_stream_settings.FramesPerSecond)));
                if (media_stream_settings.BufferDepthSeconds > 0) {
                    setting_string.append(
                        ctString::format_string(
                            L"\t\tUDP Stream BufferDepth: %lu seconds\n",
                            static_cast<unsigned long>(media_stream_settings.BufferDepthSeconds)));
                }
                setting_string.append(
                    ctString::format_string(
                        L"\t\tUDP Stream StreamLength: %lu seconds (%lu frames)\n",
                        static_cast<unsigned long>(media_stream_settings.StreamLengthSeconds),
                        static_cast<unsigned long>(media_stream_settings.StreamLengthFrames)));
                setting_string.append(
                    ctString::format_string(
                        L"\t\tUDP Stream FrameSize: %lu bytes\n",
                        static_cast<unsigned long>(media_stream_settings.FrameSizeBytes)));
            }

            if (ProtocolType::TCP == Settings->Protocol && ratelimit_low > 0) {
                if (0 == ratelimit_high) {
                    setting_string.append(
                        ctString::format_string(
                        L"\tSending throughput rate limited down to %lld bytes/second\n",
                        ratelimit_low));
                } else {
                    setting_string.append(
                        ctString::format_string(
                        L"\tSending throughput rate limited down to a range of [%lld, %lld] bytes/second\n",
                        ratelimit_low, ratelimit_high));
                }
            }

            if (netAdapterAddresses != nullptr) {
                setting_string.append(
                    ctString::format_string(
                        L"\tIP Compartment: %u\n", compartment_id));
            }

            if (Settings->ListenAddresses.size() > 0) {
                setting_string.append(L"\tAccepting connections on addresses:\n");
                for (const auto& addr : Settings->ListenAddresses) {
                    wstring wsaddress;
                    if (addr.writeCompleteAddress(wsaddress)) {
                        setting_string.append(L"\t\t");
                        setting_string.append(wsaddress);
                        setting_string.append(L"\n");
                    }
                }

                setting_string.append(
                    ctString::format_string(L"\tAccepting function: %s\n", AcceptFunctionName));

            } else {
                setting_string.append(L"\tConnecting out to addresses:\n");
                for (const auto& addr : Settings->TargetAddresses) {
                    wstring wsaddress;
                    if (addr.writeCompleteAddress(wsaddress)) {
                        setting_string.append(L"\t\t");
                        setting_string.append(wsaddress);
                        setting_string.append(L"\n");
                    }
                }

                setting_string.append(L"\tBinding to local addresses for outgoing connections:\n");
                for (const auto& addr : Settings->BindAddresses) {
                    wstring wsaddress;
                    if (addr.writeCompleteAddress(wsaddress)) {
                        setting_string.append(L"\t\t");
                        setting_string.append(wsaddress);
                        setting_string.append(L"\n");
                    }
                }

                if (Settings->LocalPortLow != 0) {
                    if (0 == Settings->LocalPortHigh) {
                        setting_string.append(
                            ctString::format_string(
                                L"\tUsing local port for outgoing connections: %u\n",
                                Settings->LocalPortLow));
                    } else {
                        setting_string.append(
                            ctString::format_string(
                                L"\tUsing local port for outgoing connections: [%u, %u]\n",
                                Settings->LocalPortLow, Settings->LocalPortHigh));
                    }
                }

                setting_string.append(
                    ctString::format_string(L"\tConnection function: %s\n", ConnectFunctionName));
                setting_string.append(
                    ctString::format_string(
                        L"\tConnection limit (maximum established connections): %u [0x%x]\n",
                        static_cast<unsigned long>(Settings->ConnectionLimit),
                        static_cast<unsigned long>(Settings->ConnectionLimit)));
                setting_string.append(
                    ctString::format_string(
                        L"\tConnection throttling rate (maximum pended connection attempts): %u [0x%x]\n",
                        static_cast<unsigned long>(Settings->ConnectionThrottleLimit),
                        static_cast<unsigned long>(Settings->ConnectionThrottleLimit)));
            }
            // calculate total connections
            if (ctsConfig::Settings->AcceptFunction) {
                if (Settings->ServerExitLimit > MAXLONG) {
                    setting_string.append(
                        ctString::format_string(
                            L"\tServer-accepted connections before exit : 0x%llx\n",
                            static_cast<ULONGLONG>(Settings->ServerExitLimit)));
                } else {
                    setting_string.append(
                        ctString::format_string(
                            L"\tServer-accepted connections before exit : %llu [0x%llx]\n",
                            static_cast<ULONGLONG>(Settings->ServerExitLimit),
                            static_cast<ULONGLONG>(Settings->ServerExitLimit)));
                }
            } else {
                unsigned long long total_connections = 0;
                if (ctsConfig::Settings->Iterations == MAXULONGLONG) {
                    total_connections = MAXULONGLONG;
                } else {
                    total_connections = ctsConfig::Settings->Iterations * static_cast<unsigned long long>(ctsConfig::Settings->ConnectionLimit);
                }
                if (total_connections > MAXLONG) {
                    setting_string.append(
                        ctString::format_string(
                            L"\tTotal outgoing connections before exit (iterations * concurrent connections) : 0x%llx\n",
                            total_connections));
                } else {
                    setting_string.append(
                        ctString::format_string(
                            L"\tTotal outgoing connections before exit (iterations * concurrent connections) : %llu [0x%llx]\n",
                            total_connections,
                            total_connections));
                }
            }

            setting_string.append(L"\n");

            // immediately print the legend once we know the status info object
            switch (verbosity) {
                // case 0: // nothing
                case 1: // status updates
                case 2: // error info
                case 3: // error info + status updates
                case 4: // connection info + error info
                case 5: // connection info + error info + status updates
                case 6: // above + debug info
                {
                    ::fwprintf(stdout, L"%s", setting_string.c_str());
                }
            }

            if (connectionlogger && !connectionlogger->IsCsvFormat()) {
                connectionlogger->LogMessage(setting_string.c_str());
            }
        }

    } // namespace ctsConfig
} // namespace ctsTraffic

