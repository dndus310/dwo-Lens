#include "StateMonitorLiveLink.h"

#include "Containers/StringConv.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Timespan.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "SocketTypes.h"
#include "StateMonitorLiveProtocol.h"

DEFINE_LOG_CATEGORY_STATIC(LogStateMonitorLive, Log, All);

namespace StateMonitorLiveLink
{
constexpr double RetrySeconds = 2.0;
constexpr double ConnectTimeoutSeconds = 1.0;
constexpr int32 MaxReadsPerTick = 16;
constexpr int32 MaxLineBytes = 1024 * 1024;
constexpr int32 MaxSendBytes = 64 * 1024 * 1024;
} // namespace StateMonitorLiveLink

FStateMonitorLiveLink::FStateMonitorLiveLink(int32 InPort)
    : SocketSubsystem(ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)), Socket(nullptr), NextAttemptTime(0.0),
      ConnectStartTime(0.0), SendOffset(0), Port(InPort), State(EState::Idle), bJustConnected(false),
      bJustDisconnected(false)
{
	if (SocketSubsystem)
	{
		Address = SocketSubsystem->GetAddressFromString(TEXT("127.0.0.1"));
		if (Address.IsValid())
		{
			Address->SetPort(Port);
		}
	}
}

FStateMonitorLiveLink::~FStateMonitorLiveLink()
{
	Close(TEXT("shutdown"));
}

bool FStateMonitorLiveLink::IsEnabledByCommandLine()
{
	int32 Value = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("StateMonitorLive="), Value))
	{
		return Value != 0;
	}
#if UE_BUILD_SHIPPING
	return false;
#else
	return true;
#endif
}

int32 FStateMonitorLiveLink::GetPortFromCommandLine()
{
	int32 Value = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("StateMonitorPort="), Value) && Value > 0 && Value < 65536)
	{
		return Value;
	}
	return StateMonitorLive::DefaultPort;
}

void FStateMonitorLiveLink::Tick(double Now)
{
	switch (State)
	{
	case EState::Idle:
		if (Now >= NextAttemptTime)
		{
			StartConnect(Now);
		}
		break;
	case EState::Connecting:
		PollConnect(Now);
		break;
	case EState::Connected:
		Receive();
		if (State == EState::Connected && !Flush())
		{
			Close(TEXT("send failed"));
		}
		break;
	}
}

void FStateMonitorLiveLink::StartConnect(double Now)
{
	NextAttemptTime = Now + StateMonitorLiveLink::RetrySeconds;
	if (!SocketSubsystem || !Address.IsValid())
	{
		return;
	}

	Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("StateMonitorLive"), Address->GetProtocolType());
	if (!Socket)
	{
		return;
	}
	Socket->SetNonBlocking(true);
	Socket->SetNoDelay(true);
	// 뷰어가 없을 때 연결 실패는 정상 상황이므로 로그를 남기지 않는다.
	if (!Socket->Connect(*Address))
	{
		DestroySocket();
		return;
	}
	ConnectStartTime = Now;
	State = EState::Connecting;
}

void FStateMonitorLiveLink::PollConnect(double Now)
{
	// GetConnectionState는 대기 시간이 있는 select를 쓰므로 부르지 않는다. 실패는 짧은 타임아웃으로 처리하고,
	// 다음 시도는 StartConnect에서 정한 시각(시작 + RetrySeconds)에 한다.
	if (Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::Zero()))
	{
		State = EState::Connected;
		bJustConnected = true;
		SendBuffer.Reset();
		SendOffset = 0;
		ReceiveBuffer.Reset();
		ReceivedLines.Reset();
		UE_LOG(LogStateMonitorLive, Log, TEXT("State Monitor live viewer connected (127.0.0.1:%d)."), Port);
		return;
	}
	if (Now - ConnectStartTime > StateMonitorLiveLink::ConnectTimeoutSeconds)
	{
		DestroySocket();
	}
}

void FStateMonitorLiveLink::Receive()
{
	uint8 Buffer[8192];
	for (int32 Read = 0; Read < StateMonitorLiveLink::MaxReadsPerTick; ++Read)
	{
		int32 BytesRead = 0;
		if (!Socket->Recv(Buffer, sizeof(Buffer), BytesRead))
		{
			// 0바이트(정상 종료)나 오류 모두 연결 끝으로 본다.
			Close(TEXT("closed by viewer"));
			return;
		}
		if (BytesRead <= 0)
		{
			break;
		}
		ReceiveBuffer.Append(Buffer, BytesRead);
	}

	int32 LineStart = 0;
	for (int32 Index = 0; Index < ReceiveBuffer.Num(); ++Index)
	{
		if (ReceiveBuffer[Index] != '\n')
		{
			continue;
		}
		const int32 Length = Index - LineStart;
		if (Length > 0)
		{
			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(ReceiveBuffer.GetData() + LineStart), Length);
			FString Line(Converted.Length(), Converted.Get());
			Line.TrimStartAndEndInline();
			if (!Line.IsEmpty())
			{
				ReceivedLines.Add(MoveTemp(Line));
			}
		}
		LineStart = Index + 1;
	}
	if (LineStart > 0)
	{
		ReceiveBuffer.RemoveAt(0, LineStart, EAllowShrinking::No);
	}
	if (ReceiveBuffer.Num() > StateMonitorLiveLink::MaxLineBytes)
	{
		// 줄바꿈 없는 거대한 입력은 버린다.
		ReceiveBuffer.Reset();
	}
}

bool FStateMonitorLiveLink::Flush()
{
	while (Socket && SendOffset < SendBuffer.Num())
	{
		int32 BytesSent = 0;
		const int32 Remaining = SendBuffer.Num() - SendOffset;
		if (!Socket->Send(SendBuffer.GetData() + SendOffset, Remaining, BytesSent))
		{
			const ESocketErrors Error = SocketSubsystem->GetLastErrorCode();
			if (Error == SE_EWOULDBLOCK || Error == SE_EINPROGRESS || Error == SE_NO_ERROR)
			{
				break;
			}
			return false;
		}
		if (BytesSent <= 0)
		{
			break;
		}
		SendOffset += BytesSent;
	}

	if (SendOffset >= SendBuffer.Num())
	{
		SendBuffer.Reset();
		SendOffset = 0;
	}
	else if (SendOffset > 1024 * 1024 && SendOffset > SendBuffer.Num() / 2)
	{
		SendBuffer.RemoveAt(0, SendOffset, EAllowShrinking::No);
		SendOffset = 0;
	}
	return true;
}

bool FStateMonitorLiveLink::IsConnected() const
{
	return State == EState::Connected;
}

bool FStateMonitorLiveLink::ConsumeConnected()
{
	const bool bResult = bJustConnected;
	bJustConnected = false;
	return bResult;
}

bool FStateMonitorLiveLink::ConsumeDisconnected()
{
	const bool bResult = bJustDisconnected;
	bJustDisconnected = false;
	return bResult;
}

void FStateMonitorLiveLink::SendLine(const FString& Line)
{
	if (State != EState::Connected)
	{
		return;
	}
	if (GetPendingBytes() > StateMonitorLiveLink::MaxSendBytes)
	{
		// 뷰어가 읽지 못하고 있다. 버퍼를 무한히 키우지 않고 연결을 끊는다(재연결 시 전체 상태를 다시 보낸다).
		Close(TEXT("viewer is not reading"));
		return;
	}
	const FTCHARToUTF8 Converted(*Line, Line.Len());
	SendBuffer.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	SendBuffer.Add('\n');
}

void FStateMonitorLiveLink::PopLines(TArray<FString>& OutLines)
{
	OutLines = MoveTemp(ReceivedLines);
	ReceivedLines.Reset();
}

int32 FStateMonitorLiveLink::GetPendingBytes() const
{
	return SendBuffer.Num() - SendOffset;
}

int32 FStateMonitorLiveLink::GetPort() const
{
	return Port;
}

void FStateMonitorLiveLink::Close(const TCHAR* Reason)
{
	if (State == EState::Connected)
	{
		// 남은 데이터를 한 번 더 밀어 넣어 본다. 막히면 그대로 닫는다.
		Flush();
		UE_LOG(LogStateMonitorLive, Log, TEXT("State Monitor live viewer disconnected (%s)."), Reason);
		bJustDisconnected = true;
	}
	DestroySocket();
	NextAttemptTime = FPlatformTime::Seconds() + StateMonitorLiveLink::RetrySeconds;
	SendBuffer.Reset();
	SendOffset = 0;
	ReceiveBuffer.Reset();
}

void FStateMonitorLiveLink::DestroySocket()
{
	if (Socket)
	{
		Socket->Close();
		if (SocketSubsystem)
		{
			SocketSubsystem->DestroySocket(Socket);
		}
		Socket = nullptr;
	}
	State = EState::Idle;
}
