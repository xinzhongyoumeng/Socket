#include "stdafx.h"
#include "stdio.h"
#include "winsock2.h"
#include "ws2tcpip.h"
#include "mswsock.h"
#pragma comment(lib,"ws2_32.lib")

#define c_LISTEN_PORT 8600
#define c_MAX_DATA_LENGTH 4096
#define c_SOCKET_CONTEXT 2048
#define c_MAX_POST_ACCEPT 10

enum enumIoType
{
	ACCEPT,
	RECV,
	SEND,
	NONE,
	ROOT
};

//网络操作结构体，包含Overlapped，关联的socket，缓冲区以及这个操作的类型，accpet，received还是send
struct _PER_IO_CONTEXT
{
	OVERLAPPED m_overlapped; // 每一个重叠网络操作的重叠结构(针对每一个Socket的每一个操作，都要有一个           
	SOCKET m_socket; // 这个网络操作所使用的Socket
	WSABUF m_wsaBuf; // WSA类型的缓冲区，用于给重叠操作传参数的
	char* m_szBuffer; // 这个是WSABUF里具体存字符的缓冲区
	enumIoType m_IoType; // 标识网络操作的类型(对应上面的枚举)

	_PER_IO_CONTEXT* pPreIoContext;
	_PER_IO_CONTEXT* pNextIoContext;

	_PER_IO_CONTEXT()
	{
		ZeroMemory(&m_overlapped, sizeof(m_overlapped));
		m_szBuffer = new char[c_MAX_DATA_LENGTH];
		ZeroMemory(m_szBuffer, c_MAX_DATA_LENGTH);
		m_socket = NULL;
		m_wsaBuf.buf = m_szBuffer;
		m_wsaBuf.len = c_MAX_DATA_LENGTH;
		m_IoType = NONE;
		pPreIoContext = NULL;
		pNextIoContext = NULL;
	}

	void CloseIoContext()
	{
		if (pNextIoContext)
		{
			pPreIoContext->pNextIoContext = pNextIoContext;
			pNextIoContext->pPreIoContext = pPreIoContext;
		}
		else
		{
			pPreIoContext->pNextIoContext = NULL;
		}
		delete m_szBuffer;
		ZeroMemory(&m_overlapped, sizeof(m_overlapped));
		m_socket = NULL;
		m_IoType = NONE;
		pPreIoContext = NULL;
		pNextIoContext = NULL;
	}

	// 重置缓冲区内容
	void ResetBuffer()
	{
		ZeroMemory(m_szBuffer, c_MAX_DATA_LENGTH);
	}
};

struct _PER_SOCKET_CONTEXT
{
	SOCKET m_Socket; // 每一个客户端连接的Socket
	SOCKADDR_IN m_ClientAddr; // 客户端的地址
	char m_username[40];
	_PER_IO_CONTEXT* HeadIoContext;

	_PER_SOCKET_CONTEXT()
	{
		m_Socket = INVALID_SOCKET;
		memset(&m_ClientAddr, 0, sizeof(m_ClientAddr));
		ZeroMemory(m_username, 40);
		HeadIoContext = new _PER_IO_CONTEXT();
		HeadIoContext->m_IoType = ROOT;
	}

	_PER_IO_CONTEXT* GetNewIoContext()
	{
		_PER_IO_CONTEXT* temp = new _PER_IO_CONTEXT();
		if (HeadIoContext->pNextIoContext)
		{
			HeadIoContext->pNextIoContext->pPreIoContext = temp;
			temp->pNextIoContext = HeadIoContext->pNextIoContext;
		}
		return temp;
	}

	// 释放资源
	void CloseSocketContext()
	{
		if (m_Socket != INVALID_SOCKET)
		{
			while (HeadIoContext->pNextIoContext)
			{
				HeadIoContext->pNextIoContext->CloseIoContext();
			}
			HeadIoContext = NULL;
			closesocket(m_Socket);
			m_Socket = NULL;
			memset(&m_ClientAddr, 0, sizeof(m_ClientAddr));
			ZeroMemory(m_username, 40);
		}
	}
};

//Socket结构体数组的类，包含上面Socket组合结构体数组，并对改数组增删改
class ARRAY_PER_SOCKET_CONTEXT
{
private:
	_PER_SOCKET_CONTEXT* m_arrayPerSocketContext[c_SOCKET_CONTEXT];
public:
	int num = 0;

	_PER_SOCKET_CONTEXT* GetNewSocketContext(SOCKADDR_IN* pAddressPort, char* szUserName)
	{
		for (int i = 0; i < c_SOCKET_CONTEXT; i++)
		{
			//如果某一个IO_CONTEXT_ARRAY[i]为0，表示哪一个位可以放入PER_IO_CONTEXT  
			if (!m_arrayPerSocketContext[i])
			{
				m_arrayPerSocketContext[i] = new _PER_SOCKET_CONTEXT();
				memcpy(&(m_arrayPerSocketContext[i]->m_ClientAddr), pAddressPort, sizeof(SOCKADDR_IN));
				strcpy_s(m_arrayPerSocketContext[i]->m_username, strlen(szUserName) + 1, szUserName);
				num++;
				return m_arrayPerSocketContext[i];
			}
		}
		return NULL;
	}

	_PER_SOCKET_CONTEXT* getARR(int i)
	{
		return m_arrayPerSocketContext[i];
	}

	// 从数组中移除一个指定的IoContext
	void RemoveContext(_PER_SOCKET_CONTEXT* pRemoveSokcetContext)
	{
		for (int i = 0; i < c_SOCKET_CONTEXT; i++)
		{
			if (m_arrayPerSocketContext[i] == pRemoveSokcetContext)
			{
				num--;
				m_arrayPerSocketContext[i]->CloseSocketContext();
				m_arrayPerSocketContext[i] = NULL;
				break;
			}
		}
	}
};

//用户名数组
char* g_saUsername[3] = {"admin","root" ,"zz"};
//密码数组
char* g_saPassword[3] = {"adminadmin","rootroot" ,"zzzz"};


//完成接口
HANDLE g_hIoCompletionPort;

//创建一个Socket结构体数组的句柄
ARRAY_PER_SOCKET_CONTEXT* m_arraySocketContext;

//AcceptEx的GUID，用于导出AcceptEx函数指针
GUID GuidAcceptEx = WSAID_ACCEPTEX;
//AcceptEx函数指针
LPFN_ACCEPTEX m_AcceptEx;
//AcceptExSockaddrs的GUID，用于导出AcceptExSockaddrs函数指针
GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;
//AcceptExSockaddrs函数指针
LPFN_GETACCEPTEXSOCKADDRS m_AcceptExSockAddrs;

//接下来用来Listen的Socket结构体
_PER_SOCKET_CONTEXT* g_ListenContext;

//声明用来完成端口操作的线程
DWORD WINAPI workThread(LPVOID lpParam);
//声明投递Send请求，发送完消息后会通知完成端口
bool _PostSend(_PER_IO_CONTEXT* pSendIoContext);
//声明投递Recv请求，接收完请求会通知完成端口
bool _PostRecv(_PER_IO_CONTEXT* pRecvIoContext);
//声明投递Accept请求，收到一个连接请求会通知完成端口
bool _PostAccept(_PER_IO_CONTEXT* pAcceptIoContext);
//声明投递结束请求，发送完结束信息后通知完成端口
bool _PostEnd(_PER_IO_CONTEXT* pEndIoContext);

int main()
{
	WSADATA wsaData;
	if (NO_ERROR != WSAStartup(MAKEWORD(2, 2), &wsaData))
	{
		printf_s("初始化Socket库 失败！\n");
		return 1;
	}

	// 建立完成端口
	g_hIoCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (g_hIoCompletionPort == NULL)
	{
		printf_s("建立完成端口失败！错误代码: %d!\n", WSAGetLastError());
		return 2;
	}

	SYSTEM_INFO si;
	GetSystemInfo(&si);
	// 根据本机中的处理器数量，建立对应的线程数
	int m_nThreads = 2 * si.dwNumberOfProcessors + 2;
	// 初始化线程句柄
	HANDLE* m_phWorkerThreads = new HANDLE[m_nThreads];
	// 根据计算出来的数量建立线程
	for (int i = 0; i < m_nThreads; i++)
	{
		m_phWorkerThreads[i] =
			CreateThread(0, 0, workThread, NULL, 0, NULL);
	}
	printf_s("建立 WorkerThread %d 个.\n", m_nThreads);

	// 服务器地址信息，用于绑定Socket
	struct sockaddr_in ServerAddress;

	// 生成用于监听的Socket的信息
	g_ListenContext = new _PER_SOCKET_CONTEXT;

	// 需要使用重叠IO，必须得使用WSASocket来建立Socket，才可以支持重叠IO操作
	g_ListenContext->m_Socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (g_ListenContext->m_Socket == INVALID_SOCKET)
	{
		printf_s("初始化Socket失败，错误代码: %d.\n", WSAGetLastError());
	}
	else
	{
		printf_s("初始化Socket完成.\n");
	}

	// 填充地址信息
	ZeroMemory(&ServerAddress, sizeof(ServerAddress));
	ServerAddress.sin_family = AF_INET;
	ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);
	ServerAddress.sin_port = htons(c_LISTEN_PORT);

	// 绑定地址和端口
	if (bind(g_ListenContext->m_Socket, (LPSOCKADDR)&ServerAddress, sizeof(ServerAddress)) == SOCKET_ERROR)
	{
		printf_s("bind()函数执行错误.\n");
		return 4;
	}

	// 开始对这个ListenContext里面的socket所绑定的地址端口进行监听
	if (listen(g_ListenContext->m_Socket, SOMAXCONN) == SOCKET_ERROR)
	{
		printf_s("Listen()函数执行出现错误.\n");
		return 5;
	}

	DWORD dwBytes = 0;
	//使用WSAIoctl，通过GuidAcceptEx(AcceptEx的GUID)，获取AcceptEx函数指针
	if (SOCKET_ERROR == WSAIoctl(
		g_ListenContext->m_Socket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&GuidAcceptEx,
		sizeof(GuidAcceptEx),
		&m_AcceptEx,
		sizeof(m_AcceptEx),
		&dwBytes,
		NULL,
		NULL))
	{
		printf_s("WSAIoctl 未能获取AcceptEx函数指针。错误代码: %d\n", WSAGetLastError());
		return 6;
	}

	//使用WSAIoctl，通过GuidGetAcceptExSockAddrs(AcceptExSockaddrs的GUID)，获取AcceptExSockaddrs函数指针
	if (SOCKET_ERROR == WSAIoctl(
		g_ListenContext->m_Socket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&GuidGetAcceptExSockAddrs,
		sizeof(GuidGetAcceptExSockAddrs),
		&m_AcceptExSockAddrs,
		sizeof(m_AcceptExSockAddrs),
		&dwBytes,
		NULL,
		NULL))
	{
		printf_s("WSAIoctl 未能获取GuidGetAcceptExSockAddrs函数指针。错误代码: %d\n", WSAGetLastError());
		return 7;
	}

	//将这个将ListenSocket结构体放到完成端口中，有结果告诉我，并将监听ListenContext传进去
	if ((CreateIoCompletionPort((HANDLE)g_ListenContext->m_Socket, g_hIoCompletionPort, (DWORD)g_ListenContext, 0) == NULL))
	{
		printf_s("绑定服务端SocketContext至完成端口失败！错误代码: %d/n", WSAGetLastError());
		if (g_ListenContext->m_Socket != INVALID_SOCKET)
		{
			closesocket(g_ListenContext->m_Socket);
			g_ListenContext->m_Socket = INVALID_SOCKET;
		}
		return 3;
	}
	printf_s("Listen Socket绑定完成端口 完成.\n");


	//循环10次
	for (int i = 0; i < c_MAX_POST_ACCEPT; i++)
	{
		//通过网络操作结构体数组获得一个新的网络操作结构体
		_PER_IO_CONTEXT* newAcceptIoContext = g_ListenContext->GetNewIoContext();
		//投递Send请求，发送完消息后会通知完成端口，
		if (_PostAccept(newAcceptIoContext) == false)
		{
			newAcceptIoContext->CloseIoContext();
			return 4;
		}
	}
	printf_s("投递 %d 个AcceptEx请求完毕 \n", c_MAX_POST_ACCEPT);

	printf_s("INFO:服务器端已启动......\n");

	//主线程阻塞，输入exit退出
	bool run = true;
	while (run)
	{
		char st[40];
		gets_s(st);

		if (!strcmp("exit", st))
		{
			run = false;
		}
	}
	WSACleanup();

	return 0;
}

//定义用来完成端口操作的线程
DWORD WINAPI workThread(LPVOID lpParam)
{
	//网络操作完成后接收的网络操作结构体里面的Overlapped
	OVERLAPPED* pOverlapped = NULL;
	//网络操作完成后接收的Socket结构体，第一次是ListenSocket的结构体
	_PER_SOCKET_CONTEXT* pListenContext = NULL;
	//网络操作完成后接收的字节数 
	DWORD dwBytesTransfered = 0;

	bool run = true;
	// 循环处理请求
	while (run)
	{
		BOOL bReturn = GetQueuedCompletionStatus(
			g_hIoCompletionPort,//这个就是我们建立的那个唯一的完成端口  
			&dwBytesTransfered,//这个是操作完成后返回的字节数 
			(PULONG_PTR)&pListenContext,//这个是我们建立完成端口的时候绑定的那个sockt结构体
			&pOverlapped,//这个是我们在连入Socket的时候一起建立的那个重叠结构  
			INFINITE);//等待完成端口的超时时间，如果线程不需要做其他的事情，那就INFINITE

		//通过这个Overlapped，得到包含这个的网错操作结构体
		_PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, _PER_IO_CONTEXT, m_overlapped);

		// 判断是否有客户端断开了
		if (!bReturn)
		{
			DWORD dwErr = GetLastError();
			//错误代码64，客户端closesocket
			if (dwErr == 64)
			{
				char IPAddr[20];
				inet_ntop(AF_INET, &pListenContext->m_ClientAddr.sin_addr, IPAddr, 16);
				printf_s("客户端 %s:%d 断开连接！\n", IPAddr, ntohs(pListenContext->m_ClientAddr.sin_port));
				m_arraySocketContext.RemoveContext(pListenContext);
			}
			else
			{
				printf_s("客户端异常断开 %d", dwErr);
			}
		}
		else
		{
			//判断这个网络操作的类型
			switch (pIoContext->m_IoType)
			{
			case ACCEPT:
				{
					// 1. 首先取得连入客户端的地址信息(查看业务员接待的客户信息)
					SOCKADDR_IN* pClientAddr = NULL;
					SOCKADDR_IN* pLocalAddr = NULL;
					int remoteLen = sizeof(SOCKADDR_IN), localLen = sizeof(SOCKADDR_IN);
					m_AcceptExSockAddrs(pIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.len - ((sizeof(SOCKADDR_IN) + 16) * 2),
					                    sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, (LPSOCKADDR*)&pLocalAddr, &localLen, (LPSOCKADDR*)&pClientAddr, &remoteLen);

					char IPAddr[16];
					inet_ntop(AF_INET, &pClientAddr->sin_addr, IPAddr, 16);
					printf_s("客户端 %s:%d 连接.\n", IPAddr, ntohs(pClientAddr->sin_port));

					//接收的密码
					char* input_password = new char[40];

					//接收字符串为 用户名#密码 的结构，需要strtok_s分割开
					char* input_username = strtok_s(pIoContext->m_wsaBuf.buf, "#", &input_password);

					//保存连接客户端的用户名
					char* user = new char[40];
					strcpy_s(user, strlen(input_username) + 1, input_username);

					//是否登陆成功
					bool ok = false;

					if (strlen(input_username) > 0 && strlen(input_password) > 0)
					{
						//查找账号是否存在
						for (int i = 0; i < sizeof(g_saUsername) / sizeof(g_saUsername[0]); i++)
						{
							int j;
							for (j = 0; g_saUsername[i][j] == input_username[j] && input_username[j]; j++);
							if (g_saUsername[i][j] == input_username[j] && input_username[j] == 0)
							{
								//账号存在查找密码是否正确
								int k;
								for (k = 0; g_saPassword[i][k] == input_password[k] && input_password[k]; k++);
								if (g_saPassword[i][k] == input_password[k] && input_password[k] == 0)
								{
									ok = true;
								}
								break;
							}
						}
					}

					//无论是否登陆成功，都要反馈一个结果给客户端 登陆成功 or 登陆失败
					//通过Socket结构体数组得到一个新的Socket结构体，并将用户信息保存进去
					_PER_SOCKET_CONTEXT* newSocketContext = m_arraySocketContext.GetNewSocketContext(pClientAddr, user);
					//将Socket结构体保存到Socket结构体数组中新获得的Socket结构体中
					newSocketContext->m_Socket = pIoContext->m_socket;
					//将这个新得到的Socket结构体放到完成端口中，有结果告诉我
					HANDLE hTemp = CreateIoCompletionPort((HANDLE)newSocketContext->m_Socket, g_hIoCompletionPort, (DWORD)newSocketContext, 0);
					if (NULL == hTemp)
					{
						printf_s("执行CreateIoCompletionPort出现错误.错误代码: %d \n", GetLastError());
						break;
					}

					//给这个新得到的Socket结构体绑定一个PostSend操作，将客户端是否登陆成功的结果发送回去，发送操作完成，通知完成端口
					_PER_IO_CONTEXT* pNewSendIoContext = newSocketContext->GetNewIoContext();
					pNewSendIoContext->m_socket = newSocketContext->m_Socket;

					char IpPort[16];
					if (ok)
					{
						inet_ntop(AF_INET, &pClientAddr->sin_addr, IpPort, 16);
						printf_s("客户端 %s(%s:%d) 登陆成功！\n", user, IpPort, ntohs(pClientAddr->sin_port));
						strcpy_s(pNewSendIoContext->m_szBuffer, 11, "登陆成功！");
						pNewSendIoContext->m_wsaBuf.len = 11;
					}
					else
					{
						inet_ntop(AF_INET, &pClientAddr->sin_addr, IPAddr, 16);
						printf_s("客户端 %s(%s:%d) 登陆失败！\n", user, IPAddr, ntohs(pClientAddr->sin_port));
						strcpy_s(pNewSendIoContext->m_szBuffer, 11, "登陆失败！");
						pNewSendIoContext->m_wsaBuf.len = 11;
					}

					//查看是否登陆成功
					if (ok)
					{
						_PostSend(pNewSendIoContext);
						//给这个新得到的Socket结构体绑定一个PostRevc操作，将客户端是否登陆成功的结果发送回去，发送操作完成，通知完成端口
						_PER_IO_CONTEXT* pNewRecvIoContext = newSocketContext->GetNewIoContext();
						pNewRecvIoContext->m_socket = newSocketContext->m_Socket;

						if (!_PostRecv(pNewRecvIoContext))
						{
							pNewRecvIoContext->CloseIoContext();
						}
					}
					else
					{
						_PostEnd(pNewSendIoContext);
					}
					//将之前的Accept的网络操作结构体重置buffer，让该网络操作继续Accept
					pIoContext->ResetBuffer();
					_PostAccept(pIoContext);
				}
				break;
			case RECV:
				{
					char IPAddr[16];
					//执行recv后，进行接收数据的处理，发给别的客户端，并再recv
					if (dwBytesTransfered > 1)
					{
						char* Senddata = new char[c_MAX_DATA_LENGTH];
						ZeroMemory(Senddata, c_MAX_DATA_LENGTH);

						char* temp = new char[c_MAX_DATA_LENGTH];
						ZeroMemory(temp, c_MAX_DATA_LENGTH);

						char* sendname = new char[40];
						ZeroMemory(sendname, 40);
						if (pIoContext->m_wsaBuf.buf[0] == '\\')
						{
							sendname = strtok_s(pIoContext->m_wsaBuf.buf, "\\", &temp);
							strtok_s(sendname, " ", &temp);
							if (temp != NULL)
							{
								inet_ntop(AF_INET, &pListenContext->m_ClientAddr.sin_addr, IPAddr, 16);
								printf_s("客户端 %s(%s:%d) 向 %s 发送:%s\n", pListenContext->m_username, IPAddr, ntohs(pListenContext->m_ClientAddr.sin_port), sendname, temp);
								sprintf_s(Senddata, c_MAX_DATA_LENGTH, "%s(%s:%d)向你发送:\n%s", pListenContext->m_username, IPAddr, ntohs(pListenContext->m_ClientAddr.sin_port), temp);
							}
						}
						else
						{
							inet_ntop(AF_INET, &pListenContext->m_ClientAddr.sin_addr, IPAddr, 16);
							printf_s("客户端 %s(%s:%d) 向大家发送:%s\n", pListenContext->m_username, IPAddr, ntohs(pListenContext->m_ClientAddr.sin_port), pIoContext->m_szBuffer);
							sprintf_s(Senddata, c_MAX_DATA_LENGTH, "%s(%s:%d)向大家发送:\n%s", pListenContext->m_username, IPAddr, ntohs(pListenContext->m_ClientAddr.sin_port), pIoContext->m_szBuffer);
						}
						int count = m_arraySocketContext.num;
						for (int i = 0; i < c_SOCKET_CONTEXT; i++)
						{
							_PER_SOCKET_CONTEXT* cSocketContext = m_arraySocketContext.getARR(i);
							if (count == 0)
							{
								break;
							}
							if (cSocketContext->m_Socket == pListenContext->m_Socket || cSocketContext == NULL)
							{
								continue;
							}
							//判断是否是单对单信息
							if (strlen(sendname) > 0 && !strcmp(sendname, cSocketContext->m_username) && strlen(Senddata) > 0)
							{
								// 给这个客户端SocketContext绑定一个Recv的计划
								_PER_IO_CONTEXT* pNewSendIoContext = cSocketContext->GetNewIoContext();
								strcpy_s(pNewSendIoContext->m_szBuffer, strlen(Senddata)+1, Senddata);
								pNewSendIoContext->m_wsaBuf.len = strlen(Senddata)+1;
								pNewSendIoContext->m_socket = cSocketContext->m_Socket;
								// Send投递出去
								_PostSend(pNewSendIoContext);
							}//判断是否不是单对单消息，且消息有长度
							else if (strlen(sendname) == 0 && strlen(Senddata) > 0)
							{
								// 给这个客户端SocketContext绑定一个Recv的计划
								_PER_IO_CONTEXT* pNewSendIoContext = cSocketContext->GetNewIoContext();
								strcpy_s(pNewSendIoContext->m_szBuffer, strlen(Senddata)+1, Senddata);
								pNewSendIoContext->m_wsaBuf.len = strlen(Senddata)+1;
								pNewSendIoContext->m_socket = cSocketContext->m_Socket;
								// Send投递出去
								_PostSend(pNewSendIoContext);
							}
							count--;
						}
					}
					pIoContext->ResetBuffer();
					_PostRecv(pIoContext);
				}
				break;
			case SEND:
				//发送完消息后，将包含网络操作的结构体删除
				pIoContext->CloseIoContext();
				break;
			case NONE:
				//发送完消息后，将包含网络操作的结构体删除
				m_arraySocketContext.RemoveContext(pListenContext);
				break;
			default:
				// 不应该执行到这里
				printf_s("_WorkThread中的 pIoContext->IoType 参数异常.\n");
				run = false;
				break;
			} //switch
		}
	}
	printf_s("线程退出.\n");
	return 0;
}

//定义投递Send请求，发送完消息后会通知完成端口
bool _PostEnd(_PER_IO_CONTEXT* pSendIoContext)
{
	// 初始化变量
	DWORD dwFlags = 0;
	DWORD dwBytes = 0;
	pSendIoContext->m_IoType = NONE;

	if ((WSASend(pSendIoContext->m_socket, &pSendIoContext->m_wsaBuf, 1, &dwBytes, dwFlags, &pSendIoContext->m_overlapped,
	             NULL) == SOCKET_ERROR) && (WSAGetLastError() != WSA_IO_PENDING))
	{
		return false;
	}
	pSendIoContext->ResetBuffer();
	return true;
}

//定义投递Send请求，发送完消息后会通知完成端口
bool _PostSend(_PER_IO_CONTEXT* pSendIoContext)
{
	// 初始化变量
	DWORD dwFlags = 0;
	DWORD dwBytes = 0;
	pSendIoContext->m_IoType = SEND;

	if ((WSASend(pSendIoContext->m_socket, &pSendIoContext->m_wsaBuf, 1, &dwBytes, dwFlags, &pSendIoContext->m_overlapped,
	             NULL) == SOCKET_ERROR) && (WSAGetLastError() != WSA_IO_PENDING))
	{
		//.RemoveContext(SendIoContext);
		return false;
	}
	pSendIoContext->ResetBuffer();
	return true;
}

//定义投递Recv请求，接收完请求会通知完成端口
bool _PostRecv(_PER_IO_CONTEXT* pRecvIoContext)
{
	// 初始化变量
	DWORD dwFlags = 0;
	DWORD dwBytes = 0;
	pRecvIoContext->m_IoType = RECV;
	WSABUF* p_wbuf = &pRecvIoContext->m_wsaBuf;
	OVERLAPPED* p_ol = &pRecvIoContext->m_overlapped;

	pRecvIoContext->ResetBuffer();

	int nBytesRecv = WSARecv(pRecvIoContext->m_socket, p_wbuf, 1, &dwBytes, &dwFlags, p_ol, NULL);

	// 如果返回值错误，并且错误的代码并非是Pending的话，那就说明这个重叠请求失败了
	if (nBytesRecv == SOCKET_ERROR && (WSAGetLastError() != WSA_IO_PENDING))
	{
		if (WSAGetLastError() != 10054)
		{
			printf_s("投递一个WSARecv失败！%d \n", WSAGetLastError());
		}
		return false;
	}
	return true;
}

//定义投递Accept请求，收到一个连接请求会通知完成端口
bool _PostAccept(_PER_IO_CONTEXT* pAcceptIoContext)
{
	// 准备参数
	DWORD dwBytes = 0;
	pAcceptIoContext->m_IoType = ACCEPT;
	WSABUF* p_wbuf = &pAcceptIoContext->m_wsaBuf;
	OVERLAPPED* p_ol = &pAcceptIoContext->m_overlapped;

	// 为以后新连入的客户端先准备好Socket(准备好接待客户的业务员，而不是像传统Accept现场new一个出来)
	pAcceptIoContext->m_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (pAcceptIoContext->m_socket == INVALID_SOCKET)
	{
		printf_s("创建用于Accept的Socket失败！错误代码: %d", WSAGetLastError());
		return false;
	}

	// 投递AcceptEx
	if (m_AcceptEx(g_ListenContext->m_Socket, pAcceptIoContext->m_socket, p_wbuf->buf, p_wbuf->len - ((sizeof(SOCKADDR_IN) + 16) * 2),
	               sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &dwBytes, p_ol) == FALSE)
	{
		if (WSAGetLastError() != WSA_IO_PENDING)
		{
			printf_s("投递 AcceptEx 请求失败，错误代码: %d", WSAGetLastError());
			return false;
		}
	}
	return true;
}
