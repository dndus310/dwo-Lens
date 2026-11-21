#pragma once

#include "CoreMinimal.h"

class FInternetAddr;
class FSocket;
class ISocketSubsystem;

/**
 * @brief dwo-Lens 뷰어(127.0.0.1:Port)에 붙는 non-blocking TCP 클라이언트.
 * 뷰어가 없으면 RetrySeconds마다 조용히 다시 시도하고, 게임 스레드를 막지 않는다.
 * 줄 단위(JSON Lines)로 보내고 받으며, 부분 송수신은 내부 버퍼가 이어 붙인다.
 */
class FStateMonitorLiveLink
{
public:
	explicit FStateMonitorLiveLink(int32 InPort);
	~FStateMonitorLiveLink();

	/** @brief 연결 시도, 수신, 송신 버퍼 비우기를 한 번 진행한다. 매 프레임 호출해도 된다. */
	void Tick(double Now);

	bool IsConnected() const;

	/** @brief 직전 Tick에서 새로 연결되었으면 true를 한 번 반환한다. */
	bool ConsumeConnected();

	/** @brief 직전 Tick에서 연결이 끊겼으면 true를 한 번 반환한다. */
	bool ConsumeDisconnected();

	/** @brief 끝에 '\n'을 붙여 송신 버퍼에 넣는다. 연결되어 있지 않으면 버린다. */
	void SendLine(const FString& Line);

	/** @brief 받은 완전한 줄들을 꺼낸다. */
	void PopLines(TArray<FString>& OutLines);

	/** @brief 아직 소켓에 넘기지 못한 바이트 수. */
	int32 GetPendingBytes() const;

	int32 GetPort() const;

	/** @brief 소켓을 닫는다. 연결된 상태였으면 로그를 한 줄 남긴다. */
	void Close(const TCHAR* Reason);

	/** @brief 명령줄로 Live 연결이 켜져 있는지. -StateMonitorLive=0/1, Shipping은 기본 꺼짐. */
	static bool IsEnabledByCommandLine();

	/** @brief -StateMonitorPort=N, 없으면 기본 포트. */
	static int32 GetPortFromCommandLine();

private:
	enum class EState : uint8
	{
		Idle,
		Connecting,
		Connected
	};

	void StartConnect(double Now);
	void PollConnect(double Now);
	void Receive();
	bool Flush();
	void DestroySocket();

	TArray<uint8> SendBuffer;
	TArray<uint8> ReceiveBuffer;
	TArray<FString> ReceivedLines;
	TSharedPtr<FInternetAddr> Address;
	ISocketSubsystem* SocketSubsystem;
	FSocket* Socket;
	double NextAttemptTime;
	double ConnectStartTime;
	int32 SendOffset;
	int32 Port;
	EState State;
	bool bJustConnected;
	bool bJustDisconnected;
};
