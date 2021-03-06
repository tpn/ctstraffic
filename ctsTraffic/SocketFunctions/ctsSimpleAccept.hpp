/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#pragma once

// cpp headers
#include <algorithm>
#include <vector>
#include <memory>
#include <string>
#include <exception>
// os headers
#include <windows.h>
#include <winsock2.h>
// ctl headers
#include <ctSockaddr.hpp>
#include <ctException.hpp>
#include <ctHandle.hpp>
// project headers
#include "ctsSocket.h"
#include "ctsConfig.h"


namespace ctsTraffic {
    ///
    /// Functor class for implementing ctsSocketFunction
    ///
    /// Implements listing/accepting connections in the simplest form
    /// - posting an accept() for each functor instance
    ///
    /// Each listener functor will be copy constructed
    /// - so need to ensure that all members are easily copied
    /// - also means the counter will need to be a ptr
    ///
    /// The refcount_sockets vector will optimize in balancing accept calls
    /// - across all listeners
    ///
    class ctsSimpleAccept {
    private:
        //
        // since this object can be copied, all the members need to be within a single impl object
        // - so that the shared_ptr<> for this struct can remain constant across all copies
        //
        struct ctsSimpleAcceptImpl
        {
            PTP_WORK thread_pool_worker;
            TP_CALLBACK_ENVIRON thread_pool_environment;
            // CS guards access to the accepting_sockets vector
            CRITICAL_SECTION accepting_cs;

            std::vector<SOCKET> listening_sockets;
            std::vector<LONG> listening_sockets_refcount;
            std::vector<std::weak_ptr<ctsSocket>> accepting_sockets;

            ctsSimpleAcceptImpl() :
                thread_pool_worker(nullptr),
                thread_pool_environment(),
                accepting_cs(),
                listening_sockets(),
                listening_sockets_refcount(),
                accepting_sockets()
            {
                ::ZeroMemory(&thread_pool_environment, sizeof thread_pool_environment);
                ::ZeroMemory(&accepting_cs, sizeof accepting_cs);
            }
            ~ctsSimpleAcceptImpl()
            {
                /// close all listening sockets to release any pended accept's
                for (auto& listening_socket : listening_sockets) {
                    if (listening_socket != INVALID_SOCKET) {
                        ::closesocket(listening_socket);
                    }
                }
                listening_sockets.clear();

                if (thread_pool_worker != nullptr) {
                    // ctsSimpleAccept object was initialized
                    ::WaitForThreadpoolWorkCallbacks(thread_pool_worker, TRUE);
                    ::CloseThreadpoolWork(thread_pool_worker);
                }

                ::DeleteCriticalSection(&accepting_cs);
            }

            // non-copyable
            ctsSimpleAcceptImpl(const ctsSimpleAcceptImpl&) = delete;
            ctsSimpleAcceptImpl& operator=(const ctsSimpleAcceptImpl&) = delete;
        };

        std::shared_ptr<ctsSimpleAcceptImpl> pimpl;

    public:
        ctsSimpleAccept() : pimpl(new ctsSimpleAcceptImpl)
        {
            // need a CS to guard access to our vectors
            if (!::InitializeCriticalSectionAndSpinCount(&pimpl->accepting_cs, 4000)) {
                throw ctl::ctException(::GetLastError(), L"InitializeCriticalSectionAndSpinCount", L"ctsSimpleAccept", false);
            }

            // will use the global threadpool, but will mark these work-items as running long
            ::InitializeThreadpoolEnvironment(&pimpl->thread_pool_environment);
            ::SetThreadpoolCallbackRunsLong(&pimpl->thread_pool_environment);
        
            // can *not* pass the this ptr to the threadpool, since this object can be copied
            pimpl->thread_pool_worker = ::CreateThreadpoolWork(ThreadPoolWorker, pimpl.get(), ctsConfig::Settings->PTPEnvironment);
            if (nullptr == pimpl->thread_pool_worker) {
                throw ctl::ctException(::GetLastError(), L"CreateThreadpoolWork", L"ctsSimpleAccept", false);
            }

            // listen to each address
            for (const auto& addr : ctsConfig::Settings->ListenAddresses) {
#pragma warning(suppress: 28193) // PREFast isn't seeing that listening is indeed evaluated before being referenced
                SOCKET listening(::WSASocket(addr.family(), SOCK_STREAM, IPPROTO_TCP, NULL, 0, ctsConfig::Settings->SocketFlags));
                ctlScopeGuard(closeSocketOnError, { if (listening != INVALID_SOCKET) ::closesocket(listening); });

                if (INVALID_SOCKET == listening) {
                    throw ctl::ctException(::WSAGetLastError(), L"socket", L"ctsSimpleAccept", false);
                }
            
                int gle = ctsConfig::SetPreBindOptions(listening, addr);
                if (gle != NO_ERROR) {
                    throw ctl::ctException(gle, L"SetPreBindOptions", L"ctsSimpleAccept", false);
                }
                gle = ctsConfig::SetPreConnectOptions(listening);
                if (gle != NO_ERROR) {
                    throw ctl::ctException(gle, L"SetPreConnectOptions", L"ctsSimpleAccept", false);
                }

                if (SOCKET_ERROR == ::bind(listening, addr.sockaddr(), addr.length())) {
                    throw ctl::ctException(::WSAGetLastError(), L"bind", L"ctsSimpleAccept", false);
                }

                if (SOCKET_ERROR == ::listen(listening, ctsConfig::GetListenBacklog())) {
                    throw ctl::ctException(::WSAGetLastError(), L"listen", L"ctsSimpleAccept", false);
                }

                pimpl->listening_sockets.push_back(listening);
                // socket is now being tracked in listening_sockets, dismiss the scope guard
                closeSocketOnError.dismiss();

                ctsConfig::PrintDebug(
                    L"\t\tListening to %s\n", addr.writeCompleteAddress().c_str());
            }

            if (pimpl->listening_sockets.empty()) {
                throw std::exception("ctsSimpleAccept invoked with no listening addresses specified");
            }
            pimpl->listening_sockets_refcount.resize(pimpl->listening_sockets.size(), 0L);
        }


        ///
        /// ctsSocketFunction functor operator()
        /// - Needs to not block ctsSocketState - will just schedule work on its own TP
        ///
        void operator() (std::weak_ptr<ctsSocket> _socket)
        {
            bool bSubmittedWork = false;
            ::EnterCriticalSection(&pimpl->accepting_cs);
            try {
                pimpl->accepting_sockets.push_back(_socket);
                ::SubmitThreadpoolWork(pimpl->thread_pool_worker);
                bSubmittedWork = true;
            } catch (const std::bad_alloc& e) {
                ctsConfig::PrintException(e);
            }
            ::LeaveCriticalSection(&pimpl->accepting_cs);

            // fail the socket if can't submit to the worker thread
            if (!bSubmittedWork) {
                auto shared_socket(_socket.lock());
                if (shared_socket) {
                    shared_socket->complete_state(ERROR_OUTOFMEMORY);
                }
            }
        }

        static
        VOID NTAPI ThreadPoolWorker(PTP_CALLBACK_INSTANCE, PVOID _context, PTP_WORK) throw()
        {
            ctsSimpleAcceptImpl* pimpl = reinterpret_cast<ctsSimpleAcceptImpl*>(_context);

            // get an accept-socket off the vector (protected with its cs)
            ::EnterCriticalSection(&pimpl->accepting_cs);
            std::weak_ptr<ctsSocket> weak_socket(*pimpl->accepting_sockets.rbegin());
            pimpl->accepting_sockets.pop_back();
            ::LeaveCriticalSection(&pimpl->accepting_cs);

            auto shared_socket(weak_socket.lock());
            ctsSocket* accept_socket = shared_socket.get();
            if (accept_socket == nullptr) {
                // underlying socket went away - nothing to do now
                return;
            }

            // based off of the refcount, choose a socket that's least used
            // - not taking a lock: it doesn't have to be that precise
            LONG lowest_refcount = pimpl->listening_sockets_refcount[0];
            unsigned listener_counter = 0;
            unsigned listener_position = 0;
            for (const auto& refcount : pimpl->listening_sockets_refcount) {
                if (refcount < lowest_refcount) {
                    lowest_refcount = refcount;
                    listener_position = listener_counter;
                }
                ++listener_counter;
            } 

            //
            // increment the listening socket we chose to accept on
            //
            ::InterlockedIncrement(&pimpl->listening_sockets_refcount[listener_position]);
            SOCKET listener = pimpl->listening_sockets[listener_position];

            // call accept on the blocking socket
            ctl::ctSockaddr remote_addr;
            int remote_addr_len = remote_addr.length();
            SOCKET new_socket = ::accept(listener, remote_addr.sockaddr(), &remote_addr_len);
            DWORD gle = ::WSAGetLastError();
            ::InterlockedDecrement(&pimpl->listening_sockets_refcount[listener_position]);

            // take a lock on the accepted socket before setting it
            accept_socket->lock_socket();

            ctsConfig::PrintErrorIfFailed(L"accept", gle);
            if (new_socket == INVALID_SOCKET) {
                // an unexpected error occured, return it so we can track it
                accept_socket->complete_state(gle);
            } else {
                // set the local addr
                ctl::ctSockaddr local_addr;
                int local_addr_len = local_addr.length();
                if (0 == ::getsockname(new_socket, local_addr.sockaddr(), &local_addr_len)) {
                    accept_socket->set_local(local_addr);
                } else if (0 == ::getsockname(listener, local_addr.sockaddr(), &local_addr_len)) {
                    accept_socket->set_local(local_addr);
                }

                accept_socket->set_socket(new_socket);
                accept_socket->set_target(remote_addr);
                accept_socket->complete_state(0);

                ctsConfig::PrintNewConnection(remote_addr);
            }

            // unlock after done touching the SOCKET
            accept_socket->unlock_socket();
        }
    };
} // namespace

