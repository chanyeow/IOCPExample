// complete_port_server.cpp: 定义控制台应用程序的入口点。
//

#include "stdafx.h"
#include <winsock2.h>
#include <iostream>
#include <assert.h>
#include <vector>
#include <process.h>
#include "iocp.h"

using namespace std;

const u_short kPort = 10001;
const std::string kSYN = "(SYN) hello server, I'm client. Can you hear me?";
const std::string kSYN_ACK = "(SYN+ACK) hello client, I'm server. I can hear you, can you hear me?";
const std::string kACK = "(ACK) hello server, I'm client. I can hear you!";

#pragma comment(lib, "Ws2_32.lib")

HANDLE g_IOCP = INVALID_HANDLE_VALUE;
HANDLE g_exit = NULL;

int g_work_thread_num = 0;
HANDLE *g_work_threads = NULL;

IOCP::PER_SOCKET_CONTEXT *g_listen_ctx = NULL;

CRITICAL_SECTION g_cs_socket_ctx_array;
std::vector<IOCP::PER_SOCKET_CONTEXT*> g_socket_ctx_array;

LPFN_ACCEPTEX g_AcceptExFn = NULL;
LPFN_GETACCEPTEXSOCKADDRS g_AcceptExSockAddrsFn = NULL;

// 管理g_socket_ctx_array
//
void AddSocketContext(IOCP::PER_SOCKET_CONTEXT *socket_ctx) {
	EnterCriticalSection(&g_cs_socket_ctx_array);
	g_socket_ctx_array.push_back(socket_ctx);
	LeaveCriticalSection(&g_cs_socket_ctx_array);
}

void RemoveSocketContext(IOCP::PER_SOCKET_CONTEXT *socket_ctx) {
	EnterCriticalSection(&g_cs_socket_ctx_array);
	for (std::vector<IOCP::PER_SOCKET_CONTEXT*>::iterator it = g_socket_ctx_array.begin(); it != g_socket_ctx_array.end(); it++) {
		if (*it == socket_ctx) {
			delete *it;
			g_socket_ctx_array.erase(it);
			break;
		}
	}
	LeaveCriticalSection(&g_cs_socket_ctx_array);
}

void ClearSocketContextArray() {
	EnterCriticalSection(&g_cs_socket_ctx_array);
	for (std::vector<IOCP::PER_SOCKET_CONTEXT*>::iterator it = g_socket_ctx_array.begin(); it != g_socket_ctx_array.end(); it++) {
		closesocket((*it)->socket);
		delete *it;
	}
	g_socket_ctx_array.clear();
	LeaveCriticalSection(&g_cs_socket_ctx_array);
}

// 发送Accept、Recv、Send请求
//
bool PostAccept(IOCP::PER_IO_CONTEXT* io_ctx) {
	if (io_ctx == NULL)
		return false;

	io_ctx->operation_type = IOCP::ACCEPT_POSTED;
	io_ctx->ResetBuffer();
	io_ctx->socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);

	if (io_ctx->socket == INVALID_SOCKET) {
		printf("WSASocket failed with code: %d\n", WSAGetLastError());
		return false;
	}

	DWORD bytes = 0;
	if (g_AcceptExFn(g_listen_ctx->socket,
		io_ctx->socket,
		io_ctx->wsa_buffer.buf,
		0,
		sizeof(SOCKADDR_IN) + 16,
		sizeof(SOCKADDR_IN) + 16,
		&bytes,
		&io_ctx->overlapped) == FALSE) {
		int gle = WSAGetLastError();
		if (gle != WSA_IO_PENDING) {
			printf("AcceptEx failed with code: %d\n", gle);
			return false;
		}
	}

	return true;
}

bool PostRecv(IOCP::PER_IO_CONTEXT* io_ctx) {
	if (io_ctx == NULL)
		return false;

	io_ctx->operation_type = IOCP::RECV_POSTED;
	io_ctx->ResetBuffer();

	DWORD recv_bytes = 0;
	DWORD flags = 0;
	int ret = WSARecv(io_ctx->socket, &io_ctx->wsa_buffer, 1, &recv_bytes, &flags, &io_ctx->overlapped, NULL);
	if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
		return false;
	}

	return true;
}

bool PostSend(IOCP::PER_IO_CONTEXT* io_ctx, const char* msg, int msg_len) {
	if (io_ctx == NULL)
		return false;

	io_ctx->operation_type = IOCP::SEND_POSTED;
	memcpy(io_ctx->wsa_buffer.buf, msg, msg_len);
	io_ctx->wsa_buffer.len = msg_len;

	DWORD sent_bytes = 0;
	int ret = WSASend(io_ctx->socket, &io_ctx->wsa_buffer, 1, &sent_bytes, 0, &io_ctx->overlapped, NULL);
	if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
		return false;
	}

	return true;
}

// 处理Accept、Recv、Send完成之后的通知
//
bool DoAccept(IOCP::PER_SOCKET_CONTEXT *socket_ctx, IOCP::PER_IO_CONTEXT *io_ctx) {
	SOCKADDR_IN* ClientAddr = NULL;
	SOCKADDR_IN* LocalAddr = NULL;
	int remoteLen = sizeof(SOCKADDR_IN);
	int localLen = sizeof(SOCKADDR_IN);

	g_AcceptExSockAddrsFn(io_ctx->wsa_buffer.buf, io_ctx->wsa_buffer.len - ((sizeof(SOCKADDR_IN) + 16) * 2),
		sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, (LPSOCKADDR*)&LocalAddr, &localLen, (LPSOCKADDR*)&ClientAddr, &remoteLen);

	printf("* new connection(%s:%d): %s\n", inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port), io_ctx->wsa_buffer.buf);

	IOCP::PER_SOCKET_CONTEXT *new_socket_ctx = new IOCP::PER_SOCKET_CONTEXT();
	new_socket_ctx->socket = io_ctx->socket;

	if (!IOCP::AssociateDeviceWithCompletionPort(g_IOCP, (HANDLE)new_socket_ctx->socket, (DWORD)new_socket_ctx)) {
		printf("AssociateDeviceWithCompletionPort failed\n");
		delete new_socket_ctx;
		new_socket_ctx = NULL;
		return false;
	}

	AddSocketContext(new_socket_ctx);

	// post recv
	IOCP::PER_IO_CONTEXT *new_io_ctx = new_socket_ctx->GetNewIoContext();
	new_io_ctx->socket = new_socket_ctx->socket;
	if (!PostRecv(new_io_ctx)) {
		printf("PostRecv failed\n");
		return false;
	}

	// post new accept
	if (!PostAccept(io_ctx)) {
		printf("PostAccept failed\n");
		return false;
	}

	return true;
}

bool DoRecv(IOCP::PER_SOCKET_CONTEXT *socket_ctx, IOCP::PER_IO_CONTEXT *io_ctx) {
	printf("recv: %s\n", io_ctx->wsa_buffer.buf);

	if (strcmp(io_ctx->wsa_buffer.buf, kSYN.c_str()) == 0) {
		// SYN+ACK
		IOCP::PER_IO_CONTEXT * new_io_ctx = socket_ctx->GetNewIoContext();
		new_io_ctx->socket = socket_ctx->socket;

		if (!PostSend(new_io_ctx, kSYN_ACK.c_str(), kSYN_ACK.length())) {
			printf("PostSend failed\n");
			return false;
		}
	}

	// post new recv
	if (!PostRecv(io_ctx)) {
		printf("PostRecv failed\n");
		return false;
	}

	return true;
}

bool DoSend(IOCP::PER_SOCKET_CONTEXT *socket_ctx, IOCP::PER_IO_CONTEXT *io_ctx) {
	printf("send: %s\n", io_ctx->wsa_buffer.buf);
	return true;
}

// 工作线程
unsigned int __stdcall WorkThreadProc(void *arg) {
	DWORD transferred_bytes = 0;
	IOCP::PER_SOCKET_CONTEXT *socket_ctx = NULL;
	OVERLAPPED *overlapped = NULL;
	DWORD gle;

	while (WaitForSingleObject(g_exit, 0) != WAIT_OBJECT_0) {
		BOOL ret = GetQueuedCompletionStatus(g_IOCP, &transferred_bytes, (PULONG_PTR)&socket_ctx, &overlapped, INFINITE);
		gle = GetLastError();

		if (socket_ctx == EXIT_CODE) {
			break;
		}

		if (ret == FALSE) {
			if (gle == WAIT_TIMEOUT) {
				continue;
			}
			else if (gle == ERROR_NETNAME_DELETED) {
				printf("client exit\n");

				RemoveSocketContext(socket_ctx);

				continue;
			}
			else {
				RemoveSocketContext(socket_ctx);
				break;
			}
		}
		else {
			IOCP::PER_IO_CONTEXT *io_ctx = CONTAINING_RECORD(overlapped, IOCP::PER_IO_CONTEXT, overlapped);

			if ((transferred_bytes == 0) && (io_ctx->operation_type == IOCP::RECV_POSTED || io_ctx->operation_type == IOCP::SEND_POSTED)) {
				printf("client disconnect\n");
				RemoveSocketContext(socket_ctx);
				continue;
			}

			switch (io_ctx->operation_type)
			{
			case IOCP::ACCEPT_POSTED:
				DoAccept(socket_ctx, io_ctx);

				break;
			case IOCP::RECV_POSTED:
				DoRecv(socket_ctx, io_ctx);

				break;
			case IOCP::SEND_POSTED:
				DoSend(socket_ctx, io_ctx);

				break;
			default:
				assert(false);
			}
		}
	}
	return 0;
}

int main()
{
	WSADATA wsaData;
	WORD wVersionRequested = MAKEWORD(2, 2);
	WSAStartup(wVersionRequested, &wsaData);

	do 
	{
		InitializeCriticalSection(&g_cs_socket_ctx_array);
		g_IOCP = IOCP::CreateNewCompletionPort();
		g_exit = CreateEvent(NULL, FALSE, FALSE, NULL);

		g_work_thread_num = IOCP::GetNumberOfProcesser() * 2;

		g_work_threads = new HANDLE[g_work_thread_num];
		for (int i = 0; i < g_work_thread_num; i++) {
			g_work_threads[i] = (HANDLE)_beginthreadex(NULL, 0, WorkThreadProc, NULL, 0, NULL);
		}

		g_listen_ctx = new IOCP::PER_SOCKET_CONTEXT;
		g_listen_ctx->socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);

		if (!IOCP::AssociateDeviceWithCompletionPort(g_IOCP, (HANDLE)g_listen_ctx->socket, (DWORD)g_listen_ctx)) {
			printf("AssociateDeviceWithCompletionPort failed with code: %d\n", GetLastError());
			break;
		}

		struct sockaddr_in addr = { 0 };
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons(kPort);
		if (bind(g_listen_ctx->socket, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
			printf("bind failed with code: %d\n", WSAGetLastError());
			break;
		}

		if (listen(g_listen_ctx->socket, SOMAXCONN) == SOCKET_ERROR) {
			printf("listen failed with code: %d\n", WSAGetLastError());
			break;
		}

		g_AcceptExFn = IOCP::GetAcceptExFnPointer(g_listen_ctx->socket);
		if (g_AcceptExFn == NULL) {
			printf("GetAcceptExFnPointer failed\n");
			break;
		}

		g_AcceptExSockAddrsFn = IOCP::GetAcceptExSockAddrsFnPointer(g_listen_ctx->socket);
		if (g_AcceptExSockAddrsFn == NULL) {
			printf("GetAcceptExSockAddrsFnPointer failed\n");
			break;
		}

		int i = 0;
		for (; i < 10; i++) {
			IOCP::PER_IO_CONTEXT *io_ctx = g_listen_ctx->GetNewIoContext();
			if (PostAccept(io_ctx) == FALSE) {
				break;
			}
		}
		if(i != 10)
			break;


	} while (FALSE);


	printf("\npress any ket to stop server...\n");
	getchar();

	SetEvent(g_exit);
	for (int i = 0; i < g_work_thread_num; i++) {
		PostQueuedCompletionStatus(g_IOCP, 0, (DWORD)EXIT_CODE, NULL);
	}
	WaitForMultipleObjects(g_work_thread_num, g_work_threads, TRUE, INFINITE);

	ClearSocketContextArray();

	printf("\npress any ket to exit...\n");
	getchar();

	DeleteCriticalSection(&g_cs_socket_ctx_array);
	WSACleanup();
    return 0;
}