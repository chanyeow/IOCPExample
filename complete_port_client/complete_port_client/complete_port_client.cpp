#include "stdafx.h"
#include <winsock2.h>
#include <iostream>
#include <assert.h>
#include <vector>
#include <process.h>
#include "iocp.h"

using namespace std;

const std::string kIP = "127.0.0.1";
const u_short kPort = 10001;

const std::string kSYN = "(SYN) hello server, I'm client. Can you hear me?";
const std::string kSYN_ACK = "(SYN+ACK) hello client, I'm server. I can hear you, can you hear me?";
const std::string kACK = "(ACK) hello server, I'm client. I can hear you!";

#pragma comment(lib, "Ws2_32.lib")

HANDLE g_IOCP = INVALID_HANDLE_VALUE;
HANDLE g_exit = NULL;

int g_work_thread_num = 0;
HANDLE *g_work_threads = NULL;

IOCP::PER_SOCKET_CONTEXT *g_client_ctx = NULL;


LPFN_CONNECTEX g_ConnectExFn = NULL;


bool PostConnect(IOCP::PER_IO_CONTEXT* io_ctx, const std::string &ip, int port) {
	if (io_ctx == NULL)
		return false;
	io_ctx->operation_type = IOCP::CONNECT_POSTED;
	io_ctx->ResetBuffer();

	// ConnectEx requires the socket to be initially bound.
	struct sockaddr_in addr0 = { 0 };
	addr0.sin_family = AF_INET;
	addr0.sin_addr.s_addr = INADDR_ANY;
	addr0.sin_port = 0;
	int ret = bind(io_ctx->socket, (SOCKADDR*)&addr0, sizeof(addr0));
	if (ret != 0) {
		printf("bind failed: %d\n", WSAGetLastError());
		return false;
	}

	struct sockaddr_in addr1 = { 0 };
	addr1.sin_family = AF_INET;
	addr1.sin_addr.s_addr = inet_addr(ip.c_str());
	addr1.sin_port = htons(port);

	ret = g_ConnectExFn(io_ctx->socket,
		reinterpret_cast<const sockaddr*>(&addr1),
		sizeof(addr1),
		NULL,
		0,
		NULL,
		&io_ctx->overlapped);
	int gle = WSAGetLastError();
	if (ret == SOCKET_ERROR && gle != WSA_IO_PENDING) {
		return false;
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
	int gle = WSAGetLastError();
	if (ret == SOCKET_ERROR && gle != WSA_IO_PENDING) {
		printf("WSASend failed with code: %d\n", gle);
		return false;
	}

	return true;
}

bool DoConnect(IOCP::PER_SOCKET_CONTEXT *socket_ctx, IOCP::PER_IO_CONTEXT *io_ctx) {
	printf("connect to server\n");

	if (!PostRecv(io_ctx)) {
		printf("PostRecv failed\n");
		return false;
	}

	IOCP::PER_IO_CONTEXT* new_io_ctx = socket_ctx->GetNewIoContext();
	new_io_ctx->socket = socket_ctx->socket;

	if (!PostSend(new_io_ctx, kSYN.c_str(), kSYN.length())) {
		printf("PostSend failed\n");
		return false;
	}

	return true;
}

bool DoRecv(IOCP::PER_SOCKET_CONTEXT *socket_ctx, IOCP::PER_IO_CONTEXT *io_ctx) {
	printf("recv: %s\n", io_ctx->wsa_buffer.buf);

	if (strcmp(io_ctx->wsa_buffer.buf, kSYN_ACK.c_str()) == 0) {
		// ACK
		IOCP::PER_IO_CONTEXT * new_io_ctx = socket_ctx->GetNewIoContext();
		new_io_ctx->socket = socket_ctx->socket;

		if (!PostSend(new_io_ctx, kACK.c_str(), kACK.length())) {
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
				printf("server exit\n");
				closesocket(socket_ctx->socket);
				socket_ctx->socket = INVALID_SOCKET;
				break;
			}
			else {
				closesocket(socket_ctx->socket);
				socket_ctx->socket = INVALID_SOCKET;
				break;
			}
		}
		else {
			IOCP::PER_IO_CONTEXT *io_ctx = CONTAINING_RECORD(overlapped, IOCP::PER_IO_CONTEXT, overlapped);

			switch (io_ctx->operation_type)
			{
			case IOCP::CONNECT_POSTED:
				DoConnect(socket_ctx, io_ctx);
				
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
		g_IOCP = IOCP::CreateNewCompletionPort();
		g_exit = CreateEvent(NULL, FALSE, FALSE, NULL);

		g_work_thread_num = IOCP::GetNumberOfProcesser() * 2;

		g_work_threads = new HANDLE[g_work_thread_num];
		for (int i = 0; i < g_work_thread_num; i++) {
			g_work_threads[i] = (HANDLE)_beginthreadex(NULL, 0, WorkThreadProc, NULL, 0, NULL);
		}

		g_client_ctx = new IOCP::PER_SOCKET_CONTEXT;
		g_client_ctx->socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);

		if (!IOCP::AssociateDeviceWithCompletionPort(g_IOCP, (HANDLE)g_client_ctx->socket, (DWORD)g_client_ctx)) {
			printf("AssociateDeviceWithCompletionPort failed with code: %d\n", GetLastError());
			break;
		}

		g_ConnectExFn = IOCP::GetConnectExFnPointer(g_client_ctx->socket);
		if (g_ConnectExFn == NULL) {
			printf("GetConnectExFnPointer failed\n");
			break;
		}

		IOCP::PER_IO_CONTEXT* io_ctx = g_client_ctx->GetNewIoContext();
		io_ctx->socket = g_client_ctx->socket;
		if (!PostConnect(io_ctx, kIP, kPort)) {
			printf("PostConnect failed\n");
		}

	} while (FALSE);


	printf("press any key to exit client...\n");
	getchar();

	SetEvent(g_exit);
	closesocket(g_client_ctx->socket);

	getchar();
	WSACleanup();
	return 0;
}